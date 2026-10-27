# DX12 后端矩阵 / 绑定问题排查记录

> 排查日期：2026-08-28
> 分支：`texture_streaming`
> 证据来源：`dx12.rdc`（RenderDoc capture）+ 源码推导 + DXIL 反汇编
> 参考实现：Vulkan 后端（`Src/GpuScene.cpp`、`Src/Shadow.cpp`）

---

## 0. 基准事实：本项目的矩阵存储/上传约定

排查一切矩阵问题前必须先钉死这条，否则会绕圈。

从 capture 里 `RenderSceneVS` / `RenderSceneVSShadow` 的 DXIL 反汇编可以直接读出乘法展开顺序，
确认 **HLSL cbuffer 按默认的 `column_major` 解释：寄存器 i = 矩阵第 i 列**。

而 `Matrix.h` 的 `mat4`：

```cpp
struct mat4 { vec4 x, y, z, w; };            // 成员在 operator*(vec4) 里当「列」用
vec4 operator*(const vec4 &rhs) { return x*rhs.x + y*rhs.y + z*rhs.z + w*rhs.w; }
```

raw 内存 = `[.x, .y, .z, .w]` 4 个 float4 → 上传后 HLSL 把 `.x` 当第 0 列。

### 因此唯一自洽的约定是：

> **构造时把「数学矩阵的行」写进 `.x/.y/.z/.w`，上传前 `transpose()` → HLSL 拿到正确矩阵。**

遵守这条的：`Camera::updateCameraMatrix`、`invLookAt`、`reverseZperspective`
**不遵守的（坑）**：`Matrix.h:260 orthographic()`、`DX12GpuScene.cpp:1778 手写 shadow view`

### 配套 quirk

`mat4::operator*(mat4)` 在「成员=行」的读法下满足 **`A * B` 算的是数学 `B·A`**。
所以相机 VP 要写 `view * proj`，写成 `proj * view` 得到的是 V·P。

---

## 1. 问题清单与修复进度

| ID | 问题 | 文件:行 | 严重度 | 状态 |
|----|------|---------|--------|------|
| A3 | shadow depth clear = 0.0 但 DepthFunc = LESS，无片元能通过 | `DX12GpuScene.cpp:1579,2454` | 致命 | ✅ 已修 |
| A1 | shadow view 用「列式」构造，上传后 HLSL 收到 Vᵀ | `DX12GpuScene.cpp:1778-1782` | 致命 | ✅ 已修 |
| A2 | `orthographic()` near 传 0.1f，`clip.w != 1` | `DX12GpuScene.cpp:1785` | 高 | ✅ 已修 |
| A4 | `buildFrustumFromMatrix` 用列当行 + plane w 符号反 | `DX12GpuScene.cpp:2259` | 高 | ✅ 已修 |
| B  | D3D12 `SV_InstanceID` 不含 `StartInstanceLocation` | 命令签名 + `drawcluster.hlsl` | 致命 | ✅ 已修 |
| C1 | Hi-Z 遮挡剔除的 VP 乘反（得到 V·P） | `DX12GpuScene.cpp:2657` | 中 | ✅ 已修 |
| C2 | light cull 的 `vpMatrix` 同样乘反（当前 shader 未使用） | `DX12GpuScene.cpp:2019` | 低 | ✅ 已修 |
| C4 | CPU 侧 picking 用 `_invViewProjectionMatrix` 差一个转置 | `Camera.cpp:163`、`GpuScene.cpp:8353` | 低（两后端共有） | ⬜ 未修，见 §2 |
| **E** | **DX12 的 NDC Y 与 Vulkan 相反，全后端未处理** —— 绕序反、所有 `ndc→uv` 换算反 | viewport + 5 个 PSO + 5~6 处 shader | **致命** | ✅ 已修 |
| D1 | shadow PSO 没有 PS，alpha-mask 投射物不做 clip | `DX12GpuScene.cpp` CreateShadowResources | 中 | ✅ 已修（见 §2c） |
| D2 | shadow 相机只退 `range`、far = `4*range`，高物体被近平面裁掉 | `DX12GpuScene.cpp:1778` | 中 | ✅ 已修（见 §2c） |
| D3 | GPU cull 用未抖动的 proj，Vulkan 用 jittered（TAA 边缘闪烁） | `DX12GpuScene.cpp:2657` | 低 | ✅ 已修（`_jitteredProj` 用于 gpucull + light cull） |

**修复顺序（已执行）：A3 → A1 → A2 → A4 → B → C1/C2 → E**，每步单独编译验证。

---

## 2. 问题详情

### A3 — shadow depth clear 值与深度函数互斥【致命】

| | clear | DepthFunc |
|---|---|---|
| Vulkan `Shadow.cpp:646` / `:492` | **1.0** | `VK_COMPARE_OP_LESS` |
| DX12 `:1579`（优化清除值）/ `:2454`（实际清除）/ `:1644` | **0.0**（注释写 reverse-Z） | `COMPARISON_FUNC_LESS` |

深度值经视口变换后被 clamp 到 `[0,1]`，`LESS` 要求 `z < 0.0` —— 不可能成立。
shadow map 永远停在清除值 0.0。

比较采样器 `:762` 已经是 `LESS_EQUAL`（standard-Z 约定），所以**要改的是 clear 而不是 DepthFunc**。
commit `c2bd318` 把 DepthFunc 从 GREATER 改成 LESS 是对的，只是漏了清除值。

**修法**：`:1579` 的 `depthClear.DepthStencil.Depth` 和 `:2454` 的 `ClearDepthStencilView` 都改成 `1.0f`。

> ⚠️ 这条不修，A1/A2/A4 全修完 shadow map 依然是空的。

---

### A1 — shadow view 矩阵构造违反存储约定【致命，主因】

`DX12GpuScene.cpp:1778-1782`：

```cpp
mat4 view(1.0f);
view[0] = vec4(x.x, y.x, z.x, 0);
view[1] = vec4(x.y, y.y, z.y, 0);
view[2] = vec4(x.z, y.z, z.z, 0);
view[3] = vec4(-x.dot(eye), -y.dot(eye), -z.dot(eye), 1);  // translation 落在成员3 = 第3列
```

translation 在「列」上 ⇒ 成员存的是数学矩阵的**列**，正好是约定的转置。
再经 `:1788` 的 `transpose()`，HLSL 最终收到 **Vᵀ**。

后果：
```
clip.w = -(x·eye)·px - (y·eye)·py - (z·eye)·pz + 1
```
量级几百到几千（而不是 1），透视除法后几何全部塌缩或被 `w<0` 裁掉 —— 即「全画到 viewport 外面」。

**修法**：照 `Shadow.cpp:136` 用 `invLookAt`：
```cpp
_shadowViewMatrices[i] = invLookAt(eye, vec3(0, 1, 0), sunDir * -1.f);
```

**历史**：commit `ef1e060`（改成 invLookAt）是对的，`df8489f` 的 revert 是错的。

---

### A2 — `orthographic()` 的 near 不能给非 0【高】

`Matrix.h:260` 的 `orthographic()` 本身就是「列式」写法（offset 落在 `m[3].x/.y` = 第 3 列），
是约定的例外。它经上传的 `transpose()` 后 HLSL 收到的是 Oᵀ。

Vulkan 侧之所以没暴露：`Shadow.cpp:135` 传 `nearp = 0.f`，且 texel-snap offset 极小
⇒ 矩阵近似对角对称 ⇒ 转置近似无害（只有约 10% 的 `clip.w` 抖动）。

DX12 `:1785` 传 `nearp = 0.1f`：
```
-n/(f-n) 被塞进 HLSL 的 row3col2  ⇒  clip.w = 1 - n/(f-n)·z_view  ≠ 1
```

**修法（二选一）**：
1. 传 `0.0f`（对齐 Vulkan，最小改动）；
2. 显式写 `transpose(orthographic(...))`，语义更正确，但要同步改 A4 的推导。

---

### A4 — `buildFrustumFromMatrix` 两处错【高】

`DX12GpuScene.cpp:2259`：

1. **用列当行**：`vp[ci]` 返回第 ci 个成员 = 数学 VP 的**列**，
   而 Gribb–Hartmann 平面提取需要的是**行**。
2. **plane w 符号反**：项目的平面测试是 `dot(n, x) - w > 0`
   （`Plane.h:48` `w = p1.dot(normal)`、`:74` `d1 = corner·normal - w`），
   Gribb–Hartmann 给出的是 `n·x + d > 0`，所以应该是 `w = -d/len`，代码写的是 `+d/len`。

capture 佐证：shadow ExecuteIndirect 确实出了 draw（302/24/131/9/314/33，对应 8162/785 的上限），
说明不是「全被剔光」，但选出来的集合是错的。

**修法**：直接搬 `Shadow.cpp:165-188` 的做法 —— 用 8 个 OBB 角点
（sun-view 空间 `[-r,+r] × [-r,+r] × [0, far]` 映回世界）直接构造 6 个平面，
法线朝内，不做矩阵提取。

> A1 修完后 `_shadowProjectionMatrices[c] * _shadowViewMatrices[c]` 的含义会变，
> 这个函数**无论如何都得重写**。

---

### B — D3D12 `SV_InstanceID` 拿不到 `firstInstance`【致命】

链路：
```
cull CS 写 firstInstance = globalIdx        (gpucull.hlsl:135, shadowcull.hlsl:167)
  → VS  chunkid = chunkIndex[SV_InstanceID] (drawcluster.hlsl:70, 104)
  → PS  meshChunks[chunkid].materialIndex → materials[...] → _Textures[...]
```

- **Vulkan**：`gl_InstanceIndex` = `firstInstance + i` ⇒ 成立（设计见 `docs/csm_cull_dedup_and_merge.md:128`）。
- **D3D12**：`SV_InstanceID` 每个 draw 从 0 开始，**不含 `StartInstanceLocation`**；
  indirect draw 的 `InstanceCount` 全是 1 ⇒ `SV_InstanceID` **恒为 0**
  ⇒ 全场景所有物体都取 `chunkIndex[0]` 那一个 material。

**capture 实证**：
- `chunkIndex` buffer（`ResourceId::330`）开头是 `4249, 4250, 4251, 4252, …`
- eid **1104** 与 eid **2500**（相隔约 1400 个 draw）采样的是**完全相同的三张贴图**：
  `ResourceId::2753` (BC1_SRGB) / `2766` (BC3_SRGB) / `2772` (BC5_UNORM)

610 张贴图、202 个 material 的场景里这不可能是巧合。

**已排除**：bindless descriptor 布局是好的 ——
`_textures[k]` ↔ 槽位 `SRV_BINDLESS_START + k` 一一对应（`:446` / `:1749`），
table 绑在 `GetStaticGPU(SRV_BINDLESS_START)` 且 `OffsetInDescriptorsFromTableStart = 0`。

**修法（三选一）**：

1. **per-instance vertex buffer**：把 `chunkIndicesBuffer` 当 VB slot 4 绑，
   `D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA`，stride 4，`InstanceDataStepRate = 1`。
   D3D12 的 per-instance 顶点取数**是**带 `StartInstanceLocation` 的。改动最小。
2. **【已采用】command signature `[CONSTANT(b1), DRAW_INDEXED]`**，arg stride 20 → 24，
   `DrawIndexedIndirectCommand` 加 DX12 变体。
3. SM 6.8 的 `SV_StartInstanceLocation` —— 本项目 shader 是 SM 6.0，用不了。

#### 已采用方案（2）的落地细节

**HLSL**
- `commonstruct.hlsl`：`DrawIndexedIndirectCommand` 加 DX12 分支，首字段 `uint drawSlot`；
  配套 `MAKE_DRAW_CMD(slot, idxCount, idxBegin)` 宏，两个 backend 各自展开正确的初始化列表。
- `gpucull.hlsl`（3 处）/ `shadowcull.hlsl`（1 处）改用 `MAKE_DRAW_CMD`。
- `drawcluster.hlsl`：`PushConstants` 加 DX12-only 的第三个字段 `drawSlot`
  （Vulkan 的 push constant block 大小保持不变）。
- `shadercompat.hlsl`：
  - DX12：`DECLARE_CHUNK_ID_INPUT` 展开为空，`GET_CHUNK_ID(input)` → `chunkIndex[pushConstants.drawSlot]`
  - Vulkan：`uint instancid : SV_InstanceID;` + `chunkIndex[input.instancid]`

**C++**
- 新增 `DX12DrawIndexedArgs { uint32_t drawSlot; D3D12_DRAW_INDEXED_ARGUMENTS draw; }`，
  带 `static_assert(sizeof == 24)`；全文件的 `sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)` 换成它
  （buffer 分配、UAV `StructureByteStride`、ExecuteIndirect 偏移）。
- 两个 command signature 都改成 `[CONSTANT, DRAW_INDEXED]`，`ByteStride = 24`。
  **必须传 root signature**（会改 root argument 的 signature 不能传 nullptr）：
  - `_drawIndexedCmdSig` → `_drawClusterRootSig`，`RootParameterIndex = 3`
  - `_shadowDrawCmdSig`  → `_shadowRootSig`，`RootParameterIndex = 1`
  - 两者 `DestOffsetIn32BitValues = 2`、`Num32BitValuesToSet = 1`
- 两个 root signature 的 `Num32BitValues` 2 → 3。
- ⚠️ ExecuteIndirect 会把它改过的 root argument 置为 undefined，
  所以 shadow pass 里 `SetGraphicsRoot32BitConstants(1, 2, push, 0)` 移到
  **每个** ExecuteIndirect 之前重发（一个 cascade 有 opaque / alpha 两次）。

---

### C1 / C2 — VP 乘法顺序反了

`DX12GpuScene.cpp:2657`（Hi-Z 遮挡剔除）与 `:2019`（light cull）：

```cpp
mat4 vp = transpose(_mainCamera->getProjectMatrix() * _mainCamera->getObjectToCamera());
```

按 `A*B` = 数学 `B·A` 的 quirk，这算出来是 **V·P**。

Vulkan 参考实现 `GpuScene.cpp:3590`：
```cpp
mat4 viewProj = maincamera->getObjectToCamera() * jitteredProj;   // → 数学 P·V ✓
mat4 viewProjT = transpose(viewProj);
```

- **C1** 消费方 `gpucull.hlsl:54` 的 `mul(viewProjMatrix, float4(corner,1))` 拿到垃圾 NDC
  ⇒ 遮挡剔除结果是错的。（视锥剔除不受影响，走独立的 `frustum` 平面数组。）
- **C2** `lightculling.hlsl:22` 只声明了 `viewProjMatrix` 没使用 ⇒ 当前无害，顺手一起改。

**附带差异**：Vulkan 这里用的是 **jittered** projection，避免 TAA 抖动导致边缘 chunk 闪进闪出；
DX12 用的是未抖动的。不是 bug，但建议对齐。

---

### C4 — CPU 侧 picking 差一个转置【两后端共有，本次不修】
`Camera::_invViewProjectionMatrix = inverse(_objectToCameraMatrix * _projectionMatrix)`
在存储上等于 `(VP⁻¹)ᵀ`。

- 上传 GPU 时 `DX12GpuScene.cpp:1797` 又 `transpose()` 了一次 ⇒ **GPU 侧正确**；
- CPU 侧 `Camera.cpp:163` `ScreenToWorldRay` 直接拿它乘 ndc ⇒ 得到 `(VP⁻¹)ᵀ·ndc` ⇒ **picking 会偏**。
- `ScreenToWorldPos`（`Camera.cpp:179`）用的 `_invViewProj` 是对的。
- Vulkan 侧 `GpuScene.cpp:8353` 的 picking 有同样问题。

---

## 2b. E — DX12 的 NDC Y 方向与 Vulkan 相反【致命，2026-08-28 第二轮】

### 现象

「front face / back face 不太对」—— 物体看得见背面、看不见正面。

### 根因

两个后端用**完全相同的投影矩阵**和**正高度 viewport**，但 NDC Y 方向相反：

| | `ndc.y = +1` 落在 |
|---|---|
| Vulkan | 屏幕**下方**（framebuffer 原点左上，+Y 向下） |
| D3D12 | 屏幕**上方** |

所以对同一份 clip 坐标，D3D12 的光栅化结果相对 Vulkan 是**垂直镜像**的。
镜像会**反转三角形的屏幕空间绕序** → 正反面判定整个反过来。

`shadercompat.hlsl` 里的 `NDC_Y_FLIP`（DX12 = 1.0 / Vulkan = -1.0）**定义了但全项目零引用**，
说明这块当初留了坑没填。

### 为什么画面没有整个上下颠倒

`resolve.hlsl:79` 的 `output.TextureUV.y = 1.0f - output.TextureUV.y`：
Vulkan 下这是恒等映射，DX12 下它**又翻了一次**，把 base pass 的镜像抵消掉。
所以最终画面朝向看着正常，但 G-buffer 里的几何其实是镜像的 —— 绕序就跟着错。

### 同一根因的连带 bug（都靠这次修复一并解决）

所有 NDC↔UV 换算都是按 Vulkan 的 Y-down 写的：

| 位置 | 内容 |
|---|---|
| `gpucull.hlsl:64` | `uv = ndc.xy*0.5+0.5` —— Hi-Z 采样点垂直镜像 |
| `deferredlighting.hlsl:122` | `shadowUv = lsp.xy*0.5+0.5` —— cascade shadow 采样 V 反 |
| `pointspotlight.hlsl:55` | 同类 |
| `decal.hlsl:63` | 同类 |
| `commonstruct.hlsl:139` | `worldPositionForTexcoord`（反方向换算） |

`docs/HiZ-occlusion-y-flip-bug.md:91-93` 明确写了「不需要翻转」的结论，
但那是**基于 Vulkan 的光栅化公式** `y = (ndc.y+1)*0.5*H` 推出来的，DX12 不成立。

### 采用的修法：负高度 viewport

D3D12 的视口变换是：

```
y_w = (1 - ndc.y) * Height/2 + TopLeftY
```

代入 `TopLeftY = H, Height = -H`：

```
y_w = (1 - ndc.y) * (-H)/2 + H = (ndc.y + 1) * H/2
```

—— 正是 Vulkan 正高度 viewport 的公式。于是 **DX12 的光栅化与 Vulkan 逐像素一致**，
所有按 Vulkan 写的 shader 假设原样成立，**shader 零改动**。

改动点（`DX12GpuScene.cpp` 一共只有 3 处 `RSSetViewports`，两个 viewport 结构体）：

```cpp
// 主 viewport
viewport.TopLeftY = (float)_device.GetHeight();
viewport.Height   = -(float)_device.GetHeight();

// shadow viewport
D3D12_VIEWPORT shadowViewport = {0, (float)SHADOW_MAP_SIZE,
                                 (float)SHADOW_MAP_SIZE, -(float)SHADOW_MAP_SIZE,
                                 0.0f, 1.0f};
```

ImGui 自己 `SetupRenderState` 里设视口，不受影响。

### 绕序：光栅化对齐后回到「朴素映射」

`VK_FRONT_FACE_CLOCKWISE` ↔ `FrontCounterClockwise = FALSE`，直接对应：

| DX12 PSO | 行 | Vulkan 对应 | VK frontFace | 结论 |
|---|---|---|---|---|
| occluder | 1135 | `GpuScene.cpp:202` | CW | FALSE ✓ 不动 |
| base pass | 1165 | `GpuScene.cpp:515` | CW | FALSE ✓ 不动 |
| base alpha-mask | 1202 | 同上 | CW | 无所谓（CullMode NONE） |
| forward | 1238 | `GpuScene.cpp:515` | CW | FALSE ✓ 不动 |
| **shadow** | **1669** | **`Shadow.cpp:467`** | **CCW** | **FALSE → TRUE ✗ 已改** |

> Vulkan 侧两种绕序都在用，是因为相机投影带 `flipX`（`Camera.cpp:28-34`）而
> shadow 正交投影不带，两条路径的手性本来就相反。DX12 之前五个 PSO 全 `FALSE`，
> 光这一点就说明至少有一个是错的。

---

- 相机矩阵全对：`projectionMatrix` / `viewMatrix` / `invViewMatrix` / `invProjectionMatrix` /
  `invViewProjectionMatrix` / `prevViewProjectionMatrix`（`:1794-1808`）逐个推导过，进 HLSL 都是正确的。
  `:1807` 的 `view * proj` 写法正确。
- bindless texture descriptor 布局（见 B 节「已排除」）。
- `DrawIndexedIndirectCommand`（`commonstruct.hlsl:32`）字段顺序与
  `D3D12_DRAW_INDEXED_ARGUMENTS` 完全一致。
- `Frustum` / `Plane` 的 C++ ↔ HLSL 内存布局一致（都是 `float3 normal; float w;` × 6 = 96 字节）。
- `GPUCullParams` 的 cbuffer 偏移与 HLSL 声明对齐。

---

## 2c. D1 / D2 / D3 — 聚光灯补全时一并修复的 CSM 阴影遗留项【2026-10-26】

### D1 — CSM alpha-mask 投射物不做 clip【已修】

Vulkan 每个 cascade 跑两遍 ExecuteIndirect（opaque + alphamask），alphamask 那遍用
`RenderSceneVSShadow` VS + `RenderSceneShadowDepthIndirect` PS（`specAlphaMask=true` 特化）。
DX12 之前两个 ExecuteIndirect 都绑同一个 VS-only 的 `_shadowPSO` —— 叶子/栅栏投射物投出实心影子。

**修法**：
- `_shadowRootSig` 扩到 5 参数：新增 `[3]` PIXEL 可见表（t0,space1 materials + t3,space1
  meshChunks）、`[4]` PIXEL 可见表（t0+ space2 bindless，unbounded）、静态采样器 s1,space1
  （linear wrap）。参数 0/1/2 不动 —— 命令签名仍 patch 参数 1，opaque PSO 多余参数无副作用。
- 新增 `_shadowAlphaMaskPSO`（同一 VS + `drawcluster.shadow.indirect.alphamask.ps.cso`，
  `SPEC_ALPHA_MASK` 宏编译，无 $Globals cbuffer —— 见 §2d 的 DXC 坑）。
- 渲染循环里 alpha 那次 ExecuteIndirect 前切 PSO 并绑参数 3/4 表。

### D2 — shadow 相机退距不足，高投射物被裁【已修】

旧代码 `eye = center + sunDir * range`、`far = 4*range`：range=3 的近 cascade 相机只退
3 单位，比眼位高的物体（10 单位的树）整个在近平面外 → 近 cascade 丢影子。
Vulkan（`Shadow.cpp:131-135`）固定退 100、far 固定 200。

**修法**：`eye = center + sunDir * 100.0f`、`far = 200.0f`（`ShadowCascadeBox.farZ` 同步）。
GPU dump 实证：cascade 0 几何覆盖率 97.41% → 100%，深度分布从贴边 `[0.0003, 1.0]` 收敛到
安全区间 `[0.39, 0.52]`（场景内容落在 200 单位箱中部，D32_FLOAT 精度绰绰有余）。

### D3 — GPU cull / light cull 未用 jittered 投影【已修】

`UpdateUniforms` 现在算 `_jitteredProj`（TAA Halton 抖动写进 m[2].x/.y，用 `Zt = m[2].w`
缩放），gpucull（Hi-Z）与 light cull 的 VP 上传全部改用 `_jitteredProj` —— 与 Vulkan
`GpuScene.cpp:3590` 一致，消除 TAA 边缘 chunk/灯光闪进闪出。

---

## 2d. 聚光灯/阴影补全记录 + 验证方法【2026-10-26】

### 实现清单（DX12 对齐 Vulkan，不含 path tracing）

- **聚光灯数据**：`CreateLights` 解析 `spot_lights`（31 个），包围球锥体拆分写
  `AAPLSpotLightCullingData`（80B/灯），`invLookAt` + `perspective` 构造每灯 VP，
  以 `transpose(spotP) * transpose(spotV)` 存储（**已是 HLSL 形态，deferred 直接用**）。
- **聚光灯 tile 剔除**：Clear/Coarse/Traditional 三段 compute 链（`lightculling.hlsl`
  的 `*Spot` 入口），Coarse 按 `(n+127)/128` 组，spot 索引 UAV 全部用结构化形式
  （`DXGI_FORMAT_UNKNOWN` + `StructureByteStride=4`，**不是** R32_UINT typed UAV）。
- **聚光阴影**：256×256×32 `D32` Texture2DArray，静态场景只渲染一次
  （`_spotShadowsRendered`）；负高度 viewport + `FrontCounterClockwise=TRUE`（E 的结论
  对 shadow 类 pass 同样成立）；LESS 深度、clear 1.0；`[CONSTANT(param0,offset16),
  DRAW_INDEXED]` 命令签名 patch chunkId（B 的同款手法）；opaque + alphamask 两个 PSO。
  灯 VP 用 `SetGraphicsRoot32BitConstants(0, 16, ...)` 每 ExecuteIndirect 重发
  （ExecuteIndirect 会把它 patch 过的 root argument 置 undefined，但矩阵在 offset 0-15
  不被 patch，一次设置即可跨两次 ExecuteIndirect 存活 —— 只有 chunkId(offset16) 被改）。
- **Deferred 接线**：t11 spotLightCullingData / t12 spotLightIndices / t13 spotShadowMaps
  （R32_FLOAT TEXTURE2DARRAY）/ t15 spotViewProjMatrices；s14 静态 cmp 采样器。
- **HDR + TAA resolve**：G-buffer 后渲进 `_hdrBuffer`（R16G16B16A16_FLOAT），resolve
  pass 双 RT（swapchain + history），TAA 历史双缓冲 ping-pong，首帧历史清黑。
- **Scatter volume** 表槽 7/8/9/11 绑聚光灯数据/索引/阴影/VP。

### DXC 坑：`const bool` 全局变量会物化成 $Globals cbuffer

`drawcluster.hlsl` 原来用 `const bool specAlphaMask = ...` + Vulkan 的
`[[vk::constant_id(0)]]`。DXC 编 PS 时 `const bool` 全局会生成一个 `$Globals` cbuffer
（b0）→ PSO 创建 0x80070057（E_INVALIDARG），因为 root sig 里没有它。
**修法**：宏化 —— `#if defined(SPEC_ALPHA_MASK) #define specAlphaMask true
#elif defined(DX12_BACKEND) #define specAlphaMask false #else [[vk::constant_id(0)]]...`
（`compile_shaders_dx12.bat` 的 alphamask 目标加 `-D SPEC_ALPHA_MASK`）。

### 验证方法：GPU 直读 dump（`AVR_DUMP=<dir>`）—— 屏幕截图不可信

本机 `CopyFromScreen` 抓 swapchain 会被 DWM/z-order/焦点坑（后台窗口拿到的是
ghost 内容：一次全白、一次纯灰、一次还会"闪烁"—— 实为多个残留进程的窗口轮流在前）。
`DebugDumpFrame()`（`DX12GpuScene.cpp`，设 `AVR_DUMP=<dir>` 后第 60 帧触发一次）直接从
GPU 读回：HDR buffer（含 min/max/mean 统计 + ACES tonemap 后的 PPM）、31 张聚光阴影
slice 统计、3 个 CSM cascade 统计、tile 灯光索引计数。

> 踩坑记录：`GetCopyableFootprints` 的 `pNumRows` 是**每子资源数组**，传单个栈变量
> 会在 31-slice 时写穿栈（0xC0000005）；PPM 写出循环 `px += 4` 但每像素只写 3 字节
> 也会堆溢出。两处都已修。

### Scatter Volume 对齐【2026-10-26 第二轮：发现并修复 6 项缺陷】

此前只做了"表槽接线"，因为 `scatterScale` 默认 0，整条管线从未跑过 —— GPU dump 打开后一次暴露 6 个问题：

| # | 缺陷 | 修复 |
|---|------|------|
| S1 | **dispatch 网格错**：shader 是 `numthreads(4,4,4)`，C++ 发 `(W+7)/8` 组 → XY 只覆盖一半 froxel（右/下半体积从未写入，垃圾数据被积分），Z 发 64 组×4=256 越界（靠 bounds check 撑着） | `(W+3)/4, (H+3)/4, (64+3)/4`，三轴全按 4 |
| S2 | **history 语义错**：拿 accumVolume（前向积分结果）当 t4 scatterPrev —— 透射率被二次累乘，时间混合收敛到只有第一片 | 独立 `_scatterHistoryVolume`，帧尾 CopyTextureRegion(scatter→history)，`_scatterHistoryValid` 管理 resetHistory |
| S3 | **blueNoise(t5)/perlinNoise(t13) 是 null 描述符** → 无深度抖动（时间抖动退化）；Perlin 采到 0 → `smoothstep(0.3,0.7,0)=0` → **远处雾密度恒 0** | Vulkan 逐字节同款生成上传：64×64 Bayer8×4 蓝噪、32³ 经典 Perlin（Ken Perlin 2002 改进版 + 3 octave fBm，同一张 256 排列表） |
| S4 | **s14 采样器 CLAMP**，Vulkan 是 REPEAT —— Perlin 世界空间平铺采样会被 clamp 拉边 | `D3D12_TEXTURE_ADDRESS_MODE_WRAP` |
| S5 | **固定 160×90 froxel**，与 1224×691 屏幕的 dispatch/灯光 tile 映射对不上 | 屏幕推导 `ceil(w/8)×ceil(h/8)`（=153×87），OnResize 重建（窗口实际不可调，防御性）；pc.screenWidth/Height 按 Vulkan 传 volumeW*8（padding 值） |
| S6 | **scatterScale 默认 0**（Vulkan 默认 2.0，Metal parity） | 默认 2.0 + `globalNoiseOffset` 风动画（t×0.5, 0, t×0.3，t=frame×0.016）补上 |

其它：noise 纹理常驻 NPSR（只被 compute 读）；toggle-off 时 accum 回 UAV + history 失效（对齐 Vulkan `invalidateHistory`）；ImGui Scatter 滑条 0..1 → 0..4（覆盖默认 2.0）。

**GPU dump 验证**（新增 `scatter_stats.txt`：逐深度片统计 accum 体积）：
```
z= 0: a-mean=0.9884  rgb-mean=(0.0026,0.0032,0.0043)
z=32: a-mean=0.4606  rgb-mean=(0.117,0.149,0.211)
z=63: a-mean=0.0346  rgb-mean=(0.229,0.289,0.406)
monotonic: alpha-nonincreasing=YES rgb-nondecreasing=YES
```
透射率 0.988→0.035 严格单调不增（Beer-Lambert ✓），inscatter 单调不减且远处收敛（解析 slab 积分的饱和行为 ✓），雾偏蓝（skyColor 深夜蓝的环境贡献 ✓）。
HDR 对比（同场景同机位）：mean 0.0711→0.1194（+68%，暗部获得环境雾），min 0.0000→0.0077（天空不再纯黑），max 64.6→43.1（远处亮物被雾透射率衰减）—— 三项全部符合体积合成特征。45s 浸泡（~600 帧时间混合）稳定无漂移。

> t15(inDepth)/s16 声明在 DX12 CSO 里存在但喂的是死代码（worldPosDebug/shadowDebug 未参与输出），DXC DCE 后不进 root sig 校验 —— PSO 创建不受影响，无需绑空描述符。

**验证结果（2026-10-26）**：
- HDR 线性均值 ch0=0.070，max 64.6，结构完整；
- 31 个聚光阴影 slice 全部有几何（37%~100% 覆盖，min 深度 0.002~0.9）；
- tile 剔除：858 tiles 中 305 有聚光灯（平均 1.0/灯/tile）、201 有点光源；
- **A/B（GPU dump 对拍）**：去掉全部聚光灯后 ch0 均值 0.0700 → 0.0645（-8%），
  逐块对比 **214 块变亮、0 块变暗** —— 聚光贡献严格可加，无任何状态污染；
  之前"加聚光灯反而变暗"的结论纯属截图伪影。

---

## 4. 修复日志

验证手段说明：
- **编译**：`msbuild build-msvc/Src/AdvancedVulkanRendering.vcxproj /p:Configuration=Debug /p:Platform=x64`
- **shader**：`cd shaders && .\compile_shaders_dx12.bat`（注意 `.\` 前缀）
- **冒烟**：`.\Bin\AdvancedVulkanRendering.exe --dx12` 跑 ~50s，D3D12 debug layer 开启，
  无 `ThrowIfFailed` 抛出 / 无 device removal
- **GPU dump**（2026-10-26 新增）：`AVR_DUMP=<dir> .\Bin\AdvancedVulkanRendering.exe --dx12`
  第 60 帧自动 dump HDR/阴影/tile 统计（见 §2d），比屏幕截图可靠得多

| ID | 改动 | 验证 | 结果 |
|----|------|------|------|
| A3 | clear value 0.0 → 1.0（`D3D12_CLEAR_VALUE` + `ClearDepthStencilView`） | 编译 | ✅ |
| A1 | 手写 view → `invLookAt(eye, (0,1,0), -sunDir)` | 编译 | ✅ |
| A2 | `orthographic(..., 0.1f, ...)` → `0.0f` | 编译 | ✅ |
| A4 | `buildFrustumFromMatrix` → `buildCascadeFrustum`（8 角点）；新增 `ShadowCascadeBox` 成员 | 编译 | ✅ |
| B  | command signature `[CONSTANT, DRAW_INDEXED]`，stride 24；root constants 2→3；shader 侧 `MAKE_DRAW_CMD` / `GET_CHUNK_ID` | shader 编译 + DXIL 确认 `pushConstants_cb` size 12、`drawSlot` offset 8；冒烟 | ✅ |
| C1/C2 | `proj * view` → `view * proj`（gpucull + light cull 两处） | 编译 + 冒烟 | ✅ |
| E | 主 / shadow viewport 改负高度；shadow PSO `FrontCounterClockwise` FALSE → TRUE | 编译 + 冒烟（跑满 90s 到 render loop） | ✅ |
| D3 | gpucull + light cull VP 改用 `_jitteredProj` | 编译 + 冒烟 | ✅ |
| D1 | `_shadowRootSig` 扩 5 参数 + `_shadowAlphaMaskPSO`（VS+alphamask PS）；渲染循环 alpha 段切 PSO 并绑参数 3/4 表 | 编译 + GPU dump（CSM 统计正常、无 PSO 报错） | ✅ |
| D2 | shadow eye 退距 `range` → 固定 100，far `4*range` → 固定 200 | GPU dump：cascade 0 覆盖率 97.41%→100%，深度从贴边 `[0.0003,1.0]` 收敛到 `[0.39,0.52]` | ✅ |
| 聚光灯全套 | 31 灯解析/tile 剔除链/聚光阴影/deferred t11-t15/scatter 接线 + HDR+TAA resolve | GPU dump A/B：有灯 vs 无灯 214 块变亮、0 块变暗（严格可加），见 §2d | ✅ |
| Scatter S1-S6 | dispatch 网格/独立 history/蓝噪+Perlin/s14 WRAP/屏幕推导 froxel/默认 2.0 + 风动画 | GPU dump：透射率 0.988→0.035 单调不增、inscatter 单调收敛；HDR mean +68%、max 被雾衰减 64.6→43.1；45s 浸泡稳定，见 §2d | ✅ |

### 待验证（下一步）

用 RenderDoc 重新截帧，逐条确认：

1. **正反面** —— base pass 不再看到物体背面（E 的直接目标）。
2. **shadow map 非空** —— 抓 `_shadowMapArray`（3 个 slice），确认深度不再是全 1.0。
   （GPU dump 已确认三张 cascade 均有几何；RenderDoc 复核可看具体内容。）
3. **shadow 几何在视口内** —— 在某个 shadow draw 上看 VS 输出的 `SV_Position`，
   `clip.w` 应该恒等于 1（正交投影）。
4. **base pass 贴图正确** —— 对比首尾两个 draw 的 PS SRV 列表，
   采样的 `_Textures` 资源应该**不同**（修复前 eid 1104 与 2500 都是 2753/2766/2772）。
5. **阴影投在正确位置** —— E 修复后 `deferredlighting.hlsl:122` 的 shadow UV 才对得上。
6. **Hi-Z 剔除数量合理** —— ImGui 里的 Visible/Culled 统计，对比 Vulkan 后端同视角的数字。
   注意 C1 修好之前 Hi-Z 基本不剔除（见 `docs/HiZ-occlusion-y-flip-bug.md`）。

> 2026-10-26 更新：本机 Vulkan 后端在创建窗口前挂死（环境问题，与 DX12 改动无关），
> RenderDoc 逐帧对拍暂不可行；§2d 的 GPU dump A/B 已在数据层面验证聚光灯管线。
> 屏幕截图验证**不可用**（DWM ghost 窗口伪影，见 §2d）。
