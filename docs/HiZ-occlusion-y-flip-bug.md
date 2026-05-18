# HiZ 遮挡剔除失效:`mat4::operator*` 顺序反转引发的 viewProj 错矩阵

## 现象

GPU culling 中 `FrustumCull` 工作正常,但 `IsOccludedByHiZ` 几乎无法剔除任何物体——所有进入视锥的 chunk 都被当作可见绘制。

涉及文件:
- `Src/GpuScene.cpp`(viewProj 矩阵填充)
- `shaders/gpucull.hlsl`(HiZ 测试)
- `Src/Include/Matrix.h`(quirky 的 mat4 operator)
- 参考 `Src/Camera.cpp`(已经在用正确的顺序)

---

## 根本原因:`mat4::operator*` 实际算的是 `B * A`

`Src/Include/Matrix.h:197-207` 的实现:

```cpp
mat4 operator*(const mat4 &rhs) {
    mat4 m;
    for (int lrow = 0; lrow < 4; ++lrow) {
        for (int rcol = 0; rcol < 4; ++rcol) {
            m[rcol][lrow] = 0.0f;
            for (int k = 0; k < 4; ++k)
                m[rcol][lrow] += (*this)[k][lrow] * rhs[rcol][k];
        }
    }
    return m;
}
```

注意索引方式 `(*this)[k][lrow]`:这里 `m[i]` = row i(虽然结构体注释写的 `// columns` 是错的,从 `reverseZperspective` 元素位置可以反推确认 `m[i]` 是 row),所以实际计算的是:

```
result[row=rcol, col=lrow] = sum_k this[row=k, col=lrow] * rhs[row=rcol, col=k]
                           = sum_k this^T[lrow, k] * rhs^T[k, rcol]
                           = (this^T * rhs^T)[lrow, rcol]
                           = (rhs * this)[rcol, lrow]
```

也就是说 **代码里写 `A * B`,数学上算的是 `B * A`**。这是个反直觉的 quirk。

证据:`Src/Camera.cpp:74` 已经按这个 quirk 写过:

```cpp
mat4 viewprojmatrix = _objectToCameraMatrix * _projectionMatrix;  // 代码 V*P → 数学 P*V
_invViewProjectionMatrix = inverse(viewprojmatrix);                 // = (P*V)^-1 ✓
```

如果 operator 是标准乘法,`V * P` 是错的(应该是 `P * V`)。代码工作正常,反证了 operator 在反向。

### gpucull 写错了

`Src/GpuScene.cpp:3370` 的旧代码:

```cpp
// BEFORE
mat4 viewProj = maincamera->getProjectMatrix() * maincamera->getObjectToCamera();
//              代码 P*V    →    数学 V*P    ← 错!
```

经过 `transpose` + memcpy + HLSL 列向量读取后,HLSL 收到的 `viewProjMatrix` 是数学的 `V*P`。然后 `mul(viewProjMatrix, world)` 得到 `V*P*world`,根本不是正确的 clip。

随机的 clip 中 `clip.w = view.x` 之类的项很容易接近 0 或负值,导致 `IsOccludedByHiZ` 里 `if (clip.w <= 0.0) anyBehindCamera = true;` 在大量 chunk 上触发,然后 early return false → 一律不剔除。

`drawoccluders` 把 P 和 V 分开传,在 HLSL 里 `mul(cameraParams.projectionMatrix, cameraParams.viewMatrix)` 是 HLSL 标准 matmul,完全不经过 C++ operator,所以那条路一直工作正常。

---

## 修复

`Src/GpuScene.cpp:3370` 把矩阵顺序换过来,跟 `Camera.cpp:74` 对齐:

```cpp
// AFTER
mat4 viewProj = maincamera->getObjectToCamera() * maincamera->getProjectMatrix();
//              代码 V*P    →    数学 P*V    ← 对!
mat4 viewProjT = transpose(viewProj);
memcpy(params.viewProjMatrix, viewProjT.value_ptr(), sizeof(mat4));
```

shader 端 `mul(viewProjMatrix, world)` 现在拿到正确的 `P*V*world = clip`。

---

## 关于 Y 翻转

调试过程中曾怀疑 `shaders/gpucull.hlsl:61` 的 `uv.y = 1.0 - uv.y; // Vulkan Y flip` 是错的,做了删除。事实证明:

- Occluder pass 用 `viewport.height = +H` 渲染深度
- Vulkan 光栅化:`y_framebuffer = (ndc.y + 1) * 0.5 * H`
- 采样 UV:`uv.y = ndc.y * 0.5 + 0.5` 直接对应,**不需要再翻转**

所以删除 Y flip 这一步也是对的。但单独删 Y flip 不能修复问题——因为 viewProj 矩阵本身已经错,ndc 都是乱的,uv 翻不翻没区别。**两个修复要一起做**。

---

## 调试方法(以后类似问题可参考)

通过逐步替换 `IsOccludedByHiZ` 的早 return 行为,二分定位卡点:

1. **`IsOccludedByHiZ` 整个 `return true`**(始终剔除):
   - 场景大部分变空 → cull 调度路径 OK,问题在测试逻辑内
2. **保守路径全部 `return true`,只留最终比较**:
   - 仍大部分变空 → 早 return 主导,继续分
3. **逐个把 early return 改回 `return false`**:
   - `hizDepth > 1.0` 改回:仍大部分变空 → 不是这个
   - `anyBehindCamera` 改回:大部分又可见 → **就是这个**主导
4. → `clip.w <= 0.0` 频发 → viewProj 矩阵方向反了 → 锁定 `mat4::operator*` quirk

---

## 备注:这个 operator 还有别的隐患吗

整个项目里所有用 `projectMatrix * objectToCamera` 的地方都需要复查:

- `Src/GpuScene.cpp:3370` ✅ 已修
- `Src/GpuScene.cpp:6617`(decal click test)— 用的是 `viewProj * vec4(center, 1)`,而 mat4*vec4 又是另一个 quirky operator(实际算的是 `v * M`),刚好两个 quirk 互相抵消,所以这里**无意中是对的**;不要"修"它

如果哪天有人想把数学库重写成"正常"的 operator,要注意一并修这些地方。

---

## 文件改动总览

```
shaders/gpucull.hlsl
  - 删掉 `uv.y = 1.0 - uv.y;` Y flip(line 61),改注释解释为什么不翻

Src/GpuScene.cpp:3370
  - viewProj 矩阵顺序由 P*V 改成 V*P(代码顺序,数学上得到正确的 P*V)
```
