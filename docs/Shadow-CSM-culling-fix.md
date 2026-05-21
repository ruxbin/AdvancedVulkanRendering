# CSM Shadow Pass 剔除修复 + per-cascade 距离 cull

## 现象

启用 GPU-driven shadow 后,每个 cascade 画的物体数量跟没剔一样,等于 GPU shadow culling 完全没起作用。draw call 数 = `chunkCount × 3 cascade`,shadow pass 性能严重浪费。

涉及文件:
- `shaders/shadowcull.hlsl`(shadow cascade culling 计算)
- `Src/Include/Shadow.h`(`ShadowCullParams` 结构)
- `Src/Shadow.cpp`(`UpdateShadowMatrices` cascade 计算)
- `Src/GpuScene.cpp`(每帧上传 cull params)

参考:
- `Src/Camera.cpp:94-101`(主相机用 8 角 → 6 plane 构造 frustum 的现成例子)
- `shaders/gpucull.hlsl`(主视角 culling,工作正常,作为对照基准)

---

## 根本原因

三处问题叠加,导致 shadow 剔除完全失效:

### Bug 1:`shadowcull.hlsl` 里 frustum cull 整段被注释

`shaders/shadowcull.hlsl:29-30`(修复前):

```hlsl
// Frustum cull against cascade frustum
//if (FrustumCull(cascadeFrustum, meshChunks[chunkIndex].boundingBox))
//    return;
```

每个 chunk 直接落到下面的 cascade 循环里,全量写入 3 个 cascade 的 indirect draw buffer,没有任何几何剔除。

### Bug 2:CPU 端上传的是主相机 frustum,不是 cascade frustum

`Src/GpuScene.cpp:3330-3349`(修复前)上传给 cull shader 的是:

```cpp
const Frustum &cascadeFrustum = maincamera->getFrustum(); // ← 主相机
...
// TODO: compute proper cascade frustum from shadow VP matrix
memcpy((char *)data + 16, &cascadeFrustum, sizeof(Frustum));
```

即使把 Bug 1 的注释解开,3 个 cascade 用的也是同一个主相机 frustum——剔除结果跟主视角一样,远 cascade 该剔的远处不会被剔。

### Bug 3:`ShadowCullParams` 结构只能装 1 个 frustum

`Src/Include/Shadow.h:55-61`(修复前):

```cpp
struct ShadowCullParams {
  uint32_t opaqueChunkCount;
  uint32_t alphaMaskedChunkCount;
  uint32_t cascadeMaxChunks;
  uint32_t cascadeIndex;
  Frustum cascadeFrustum;       // ← 只有一个
};
```

要做 per-cascade frustum cull,这个结构必须扩展成数组。

---

## 修法

### 1. `Src/Include/Shadow.h`

新增两个数组成员,扩展 `ShadowCullParams`:

```cpp
std::array<Frustum, SHADOW_CASCADE_COUNT> _cascadeFrustums;
std::array<float,   SHADOW_CASCADE_COUNT> _cascadeSphereRadii;

struct ShadowCullParams {
  uint32_t opaqueChunkCount;
  uint32_t alphaMaskedChunkCount;
  uint32_t cascadeMaxChunks;
  uint32_t cascadeCount;
  vec4    cascadeCullThreshold[SHADOW_CASCADE_COUNT]; // x = 半径阈值
  Frustum cascadeFrustum[SHADOW_CASCADE_COUNT];
};
```

`sizeof(ShadowCullParams)` = 16 + 48 + 288 = **352 字节**,跟 HLSL cbuffer 对齐。

### 2. `Src/Shadow.cpp` `UpdateShadowMatrices`

每个 cascade 算完矩阵后,在 sun-view 空间下取 OBB 8 角(`(±r, ±r, 0 或 200)`),反变换到世界空间,构造 6 个 inward-normal plane:

```cpp
vec3 z_axis = normalize(sunDir * -1.0f);              // 与 invLookAt 同
vec3 x_axis = normalize(vec3(0,1,0).cross(z_axis));
vec3 y_axis = z_axis.cross(x_axis);

vec3 cornersWS[8];
for (int k = 0; k < 8; ++k) {
  float vx = (k & 1) ? sphereRadius : -sphereRadius;
  float vy = (k & 2) ? sphereRadius : -sphereRadius;
  float vz = (k & 4) ? 200.0f : 0.0f;
  cornersWS[k] = shadowCameraPos + x_axis * vx + y_axis * vy + z_axis * vz;
}

_cascadeFrustums[i] = {
    {cornersWS[0], cornersWS[2], cornersWS[4]}, // -X
    {cornersWS[1], cornersWS[5], cornersWS[3]}, // +X
    {cornersWS[0], cornersWS[4], cornersWS[1]}, // -Y
    {cornersWS[2], cornersWS[3], cornersWS[6]}, // +Y
    {cornersWS[0], cornersWS[1], cornersWS[2]}, // -Z (sun-near)
    {cornersWS[4], cornersWS[6], cornersWS[5]}, // +Z (sun-far)
};
_cascadeSphereRadii[i] = sphereRadius;
```

★ **plane 顺序是关键**:`Plane(p1,p2,p3)` 取 `normalize((p2-p1)×(p3-p1))` 作为 normal,`IsInside` 要求 normal **指向 box 内部**。具体每个 plane 的 3 角顺序经过手算验证,叉乘方向恰好朝内(见下方"调试线索")。

### 3. `Src/GpuScene.cpp:3330-3358` 上传逻辑

按新 layout 上传 3 个 frustum + 3 个距离阈值。阈值用 cascade 球半径自动算:

```cpp
for (int i = 0; i < SHADOW_CASCADE_COUNT; ++i) {
  float texelWS = _shadow->_cascadeSphereRadii[i] * 2.0f
                  / (float)_shadow->_shadowResolution;
  vec4 threshold(texelWS * 1.5f, 0.f, 0.f, 0.f);
  memcpy(p + 16 + i * sizeof(vec4), &threshold, sizeof(vec4));
}
memcpy(p + 16 + SHADOW_CASCADE_COUNT * sizeof(vec4),
       _shadow->_cascadeFrustums.data(),
       sizeof(Frustum) * SHADOW_CASCADE_COUNT);
```

近 cascade radius 小 → 阈值小(基本不剔);远 cascade radius 大 → 阈值大(剔掉亚 texel 的小物体)。不需要场景调参。

同时删除 `const Frustum &cascadeFrustum = maincamera->getFrustum();` 这行死代码和 `// TODO: compute proper cascade frustum from shadow VP matrix` 注释。

### 4. `shaders/shadowcull.hlsl`

cbuffer 改成数组,cascade 循环里启用 per-cascade frustum cull + 半径 cull:

```hlsl
[unroll]
for (uint cascadeIndex = 0; cascadeIndex < cascadeCount; cascadeIndex++)
{
    if (FrustumCull(cascadeFrustum[cascadeIndex], chunkAABB))
        continue;
    if (chunkRadius < cascadeCullThreshold[cascadeIndex].x)
        continue;
    // ... 原有 opaque/alphaMasked 写入逻辑 ...
}
```

剔除变成 per-cascade `continue` 而不是顶层 `return`——某个 cascade 剔掉不影响其他 cascade 仍然能收到这个 chunk。

### 5. 重编译

```bash
dxc -spirv -T cs_6_2 shadowcull.hlsl -E ShadowCull -Fo shadowcull.cs.spv
```

---

## 调试线索/对照基准

### Plane 朝向的右手系推导

`Plane(p1,p2,p3)` 算的 normal 必须指向 box 内部,否则 `IsInside` 全反。`(p2-p1)×(p3-p1)` 的方向取决于 3 个点的顺序,每个 plane 都得单独验。

invLookAt 里 sun-view 的基满足:
- `_x = up × _z`
- `_y = _z × _x`

由此推出两个恒等式(右手系):
- `_y × _z = _x`
- `_z × _x = _y`

对 -X 面(corner 0/2/4/6,normal 应指向 +x_axis),取 `p1=0, p2=2, p3=4`:
- `p2-p1 = 2r × y_axis`
- `p3-p1 = 200 × z_axis`
- cross = `400r × (y_axis × z_axis) = 400r × x_axis` ✓

其他 5 个面同理一一验证。如果运行时发现 cascade 把场景全部剔光(或全部不剔),八成是 plane 顺序写反——找一个 plane 把 p2、p3 交换试试。

### 对照基准

`Src/Camera.cpp:94-101` 给主相机构造 frustum 的写法是直接的范本——用 NDC 8 角反变换到世界空间然后挑 3 个角构造每个 plane。Camera 的 frustum 经过主视角 cull 验证过工作正常,可以作为"plane 构造方向是否对"的对照。

### Layout 风险

CPU `Plane` 类是 `union { float, vec3 } + float w`,实际 sizeof = 16 字节(`vec3` 无 alignas,union 12 字节 + float 4 字节)。HLSL `Plane` 是 `float3 normal + float w`,cbuffer 里也是 16 字节。**这点已经被主视角 cull 在用,不用担心**——但如果未来给 `vec3` 加上 `alignas(16)`,CPU `Plane` sizeof 会变 32,GPU 那边没变,layout 就崩了。

---

## 文件改动总览

```
Src/Include/Shadow.h
  + std::array<Frustum, 3> _cascadeFrustums
  + std::array<float,   3> _cascadeSphereRadii
  ~ ShadowCullParams: cascadeIndex → cascadeCount
                      + cascadeCullThreshold[3]
                      + cascadeFrustum 改成 [3]

Src/Shadow.cpp::UpdateShadowMatrices
  + 每个 cascade 算 sun-aligned OBB 8 角 → 6 inward plane
  + 记录 sphereRadius

Src/GpuScene.cpp::recordCommandBuffer
  - 删 const Frustum &cascadeFrustum = maincamera->getFrustum();
  - 删 TODO 注释
  ~ 上传 layout 换成 3 frustum + 3 vec4 threshold

shaders/shadowcull.hlsl
  ~ cbuffer:cascadeFrustum 改数组 + 新增 cascadeCullThreshold[3]
  ~ 主循环改 [unroll] for(cascadeIndex) + per-cascade frustum cull + 半径 cull
  + 解开第 29-30 行原本注释掉的 frustum cull
```

## 未做(留待后续)

- **Shadow 视点 Hi-Z occlusion cull**:需要先 prepass 一张 shadow Hi-Z,复杂度高。当前 frustum + 半径 cull 已经能砍掉大头。
- **沿 -sun 方向 near plane 延伸到 -infinity 的 caster 延伸**:当前 cascade box 沿 sun 方向 [0, 200],等价于 ±100 单位的 caster 容纳范围。bistro 场景够用。如果未来场景里出现远处大物体(超过 100 单位以外的 caster)阴影缺失,再加。
