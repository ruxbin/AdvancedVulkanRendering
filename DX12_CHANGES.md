# DX12 Backend Implementation - Change Log

## 概览

完成了 DX12 后端的 Phase 0-2：Shader 双后端兼容宏系统、CMake 构建选项、DX12Device 初始化、Descriptor Heap 管理器、资源创建辅助、DX12GpuScene 骨架（含 mesh 加载、uniform 更新、ImGui overlay、相机控制）。DX12 路径可运行完整主循环并呈现带 ImGui 统计的窗口。

---

## Phase 0: Shader 双后端基础设施

### 新建文件

#### `shaders/shadercompat.hlsl`
双后端绑定宏定义文件。所有 shader 通过 `#include "shadercompat.hlsl"` 引入。

- **Vulkan 路径** (无 `DX12_BACKEND` 宏): `VK_BINDING(n,s)` 展开为 `[[vk::binding(n,s)]]`，`REGISTER_*` 宏展开为空
- **DX12 路径** (定义 `DX12_BACKEND`): `VK_BINDING` 展开为空，`REGISTER_*` 展开为 `register(xN, spaceS)`
- Push constants: `DECLARE_PUSH_CONSTANTS(type, name, reg)` — Vulkan 展开为 `[[vk::push_constant]] type name`，DX12 展开为 `cbuffer name_cb : register(bN) { type name; }`
- `NDC_Y_FLIP`: Vulkan = -1.0 (Y-down)，DX12 = 1.0 (Y-up)

提供的宏：
| 宏 | Vulkan 展开 | DX12 展开 |
|----|------------|-----------|
| `VK_BINDING(n,s)` | `[[vk::binding(n,s)]]` | (空) |
| `REGISTER_CBV(n,s)` | (空) | `: register(bN, spaceS)` |
| `REGISTER_SRV(n,s)` | (空) | `: register(tN, spaceS)` |
| `REGISTER_UAV(n,s)` | (空) | `: register(uN, spaceS)` |
| `REGISTER_SAMPLER(n,s)` | (空) | `: register(sN, spaceS)` |
| `DECLARE_PUSH_CONSTANTS(type, name, reg)` | `[[vk::push_constant]] type name` | `cbuffer name_cb : register(bReg) { type name; }` |

#### `shaders/compile_shaders_dx12.bat`
DX12 DXIL 编译脚本，对应现有 `compile_shaders.bat`。使用 `-D DX12_BACKEND` 定义宏，输出 `.cso` 文件。覆盖所有 shader 入口点。

### 修改文件

以下 9 个 shader 文件全部改造为双后端宏：

| Shader 文件 | 改动内容 |
|-------------|---------|
| `drawcluster.hlsl` | 添加 `#include "shadercompat.hlsl"`，所有 `[[vk::binding]]` 改为 `VK_BINDING + REGISTER_*`，`[[vk::push_constant]]` 改为 `DECLARE_PUSH_CONSTANTS`。Bindless 纹理 `_Textures[]` 在 DX12 中改用 `space2` 避免 unbounded array 寄存器冲突 |
| `deferredlighting.hlsl` | 同上，11 个 binding 改为双后端宏 |
| `gpucull.hlsl` | 同上，8 个 binding |
| `hiz.hlsl` | 同上，4 个 binding + push constant |
| `sao.hlsl` | 同上，4 个 binding + push constant |
| `shadowcull.hlsl` | 同上，5 个 binding |
| `drawoccluders.hlsl` | 同上，1 个 cbuffer binding |
| `pointspotlight.hlsl` | 同上，8 个 binding |
| `lightculling.hlsl` | 同上，9 个 binding |

**编译验证**：所有 shader 同时通过 Vulkan SPIR-V 和 DX12 DXIL 编译。

---

## Phase 0: CMake 构建系统

### 修改文件

#### `CMakeLists.txt`
新增 `option(ENABLE_DX12 "Build with DX12 rendering backend (Windows only)" OFF)`

#### `Src/CMakeLists.txt`
- `if(ENABLE_DX12 AND WIN32)` 条件块：
  - 添加 `Src/DX12/DX12Setup.cpp` 和 `imgui_impl_dx12.cpp` 到源文件
  - 链接 `d3d12 dxgi dxguid`
  - 定义 `ENABLE_DX12=1` 编译宏
  - 添加 `Src/DX12` 到 include 路径

**构建方式**：
```bash
# 仅 Vulkan (默认)
cmake .. && cmake --build . --config Debug

# Vulkan + DX12
cmake .. -DENABLE_DX12=ON && cmake --build . --config Debug
```

---

## Phase 1: DX12Device + 窗口清屏

### 新建文件

#### `Src/DX12/DX12Setup.h`
`DX12Device` 类声明，管理：
- `IDXGIFactory6` / `IDXGIAdapter4` / `ID3D12Device` — 设备创建
- `ID3D12CommandQueue` — 直接命令队列
- `IDXGISwapChain4` — 3 缓冲 flip-discard 交换链
- `ID3D12DescriptorHeap` (RTV) — 每个交换链缓冲一个 RTV
- `ID3D12DescriptorHeap` (DSV) — 深度模板缓冲
- `ID3D12Resource` — 交换链缓冲 × 3 + 深度缓冲 (D32_FLOAT)
- `ID3D12CommandAllocator` × 3 — 每帧独立分配器
- `ID3D12GraphicsCommandList` — 命令列表
- `ID3D12Fence` + `HANDLE` — GPU-CPU 同步

公开方法：
- `BeginFrame()` — 重置命令分配器/列表，转换 swap buffer 到 RENDER_TARGET
- `EndFrameAndPresent()` — 转换到 PRESENT，关闭/执行命令列表，Present，推进帧
- `WaitForGpu()` / `MoveToNextFrame()` — 帧同步
- `GetRTV(i)` / `GetDSV()` — 获取描述符句柄

#### `Src/DX12/DX12Setup.cpp`
完整实现，~240 行：
- Debug layer 启用 (Debug 构建)
- DXGI Factory + Adapter 枚举（选择第一个支持 D3D12 的硬件适配器）
- 命令队列创建 (DIRECT type)
- 交换链创建 (`CreateSwapChainForHwnd`，FLIP_DISCARD，VSync 开启)
- RTV/DSV descriptor heap 创建
- 深度缓冲创建 (D32_FLOAT，reverse-Z clear = 0.0)
- 命令分配器 × 3 + 命令列表
- Fence + Event 帧同步

### 修改文件

#### `Src/Window.cpp`
- 新增 `#include "DX12/DX12Setup.h"` 和 `#include "SDL_syswm.h"` (在 `#ifdef ENABLE_DX12` 下)
- 新增 `#include <cstring>` (for `strcmp`)
- `main()` 开头解析 `--dx12` 命令行参数
- SDL 窗口创建：DX12 路径不传 `SDL_WINDOW_VULKAN` flag
- `#ifdef ENABLE_DX12` 分支：
  - 通过 `SDL_GetWindowWMInfo` 获取 HWND
  - 创建 `DX12Device`
  - 简单主循环：清屏为蓝色 + Present
- `#else` / Vulkan 路径：保持不动

**启动方式**：
```bash
# Vulkan (默认)
AdvancedVulkanRendering.exe

# DX12
AdvancedVulkanRendering.exe --dx12
```

---

## 当前状态

| 组件 | 状态 |
|------|------|
| Shader 双后端宏 | ✅ 完成，Vulkan + DX12 编译通过 |
| CMake ENABLE_DX12 | ✅ 完成，条件编译 + 链接 |
| DX12Device 初始化 | ✅ 完成，设备/交换链/深度/命令/同步 |
| 窗口清屏 + Present | ✅ 完成 (--dx12 启动蓝色清屏) |
| Vulkan 路径回归 | ✅ 不受影响 |

## 下一步 (Phase 2+)

| Phase | 内容 | 状态 |
|-------|------|------|
| 2 | DX12GpuScene + 资源上传 + Occluder 深度渲染 | 待开始 |
| 3 | 级联阴影贴图 | 待开始 |
| 4 | Base Pass + G-Buffers | 待开始 |
| 5 | HiZ + GPU Culling | 待开始 |
| 6 | SAO + Deferred Lighting + Forward Pass | 待开始 |
| 7 | ImGui + 收尾 | 待开始 |

---

## 文件变更汇总

### 新增文件
| 文件 | 行数 |
|------|------|
| `shaders/shadercompat.hlsl` | 25 |
| `shaders/compile_shaders_dx12.bat` | 40 |
| `Src/DX12/DX12Setup.h` | 96 |
| `Src/DX12/DX12Setup.cpp` | 241 |

### 修改文件
| 文件 | 改动说明 |
|------|---------|
| `CMakeLists.txt` | +1 行 option |
| `Src/CMakeLists.txt` | +10 行 DX12 条件块 |
| `Src/Window.cpp` | +40 行 DX12 分支 |
| `shaders/drawcluster.hlsl` | binding 宏改造 |
| `shaders/deferredlighting.hlsl` | binding 宏改造 |
| `shaders/gpucull.hlsl` | binding 宏改造 |
| `shaders/hiz.hlsl` | binding 宏改造 |
| `shaders/sao.hlsl` | binding 宏改造 |
| `shaders/shadowcull.hlsl` | binding 宏改造 |
| `shaders/drawoccluders.hlsl` | binding 宏改造 |
| `shaders/pointspotlight.hlsl` | binding 宏改造 |
| `shaders/lightculling.hlsl` | binding 宏改造 |

---

## Phase 2: Descriptor Heap + Resource Helpers + DX12GpuScene

### 新建文件

#### `Src/DX12/DX12DescriptorHeap.h` + `.cpp`
Descriptor Heap 管理器，支持 CBV/SRV/UAV 和 Sampler 两种 heap：
- **静态区** [0, staticCount): 固定资源（bindless 纹理、G-buffer SRV、材质等）
- **动态区** [staticCount, total): 每帧环形缓冲，`ResetFrame()` 重置，`AllocateDynamic(count)` 分配
- `GetStaticCPU/GPU(index)`: 获取静态描述符句柄
- 当前配置: CBV/SRV/UAV heap 2048 静态 + 4096 动态, Sampler heap 16 静态

#### `Src/DX12/DX12ResourceHelper.h`
DX12 资源创建辅助函数（全部 inline，头文件 only）：

| 函数 | 用途 |
|------|------|
| `CreateDefaultBuffer()` | GPU-local buffer + upload staging，用于 VB/IB/SB 上传 |
| `CreateUploadBuffer()` | HOST_VISIBLE buffer（uniform/constant，persistently mapped） |
| `CreateTexture2D()` | GPU-local 2D 纹理，支持 UAV flag、mip levels、array |
| `CreateGPUBuffer()` | GPU-local buffer（无初始数据，用于 indirect draw / UAV） |
| `TransitionBarrier()` | 单资源状态转换 |
| `UAVBarrier()` | UAV barrier |
| `ReadShaderFile()` | 读取 .cso 文件 |

#### `Src/DX12/DX12GpuScene.h`
`DX12GpuScene` 类声明（并行 GpuScene），成员包括：
- 所有 Vulkan 路径对应的 DX12 资源：VB/IB、mesh chunks、材质、G-buffers、HiZ/SAO pyramid、AO texture
- 每帧资源结构 `PerFrameResources`：uniform buffer (persistently mapped)、drawParams (UAV)、writeIndex (upload, readback)、cullParams (upload)
- Root Signature × 6（occluder、drawCluster、gpuCull、deferredLighting、hiZ、sao）
- Pipeline State × 9 (各 pass)
- `ID3D12CommandSignature` for `ExecuteIndirect`（替代 `vkCmdDrawIndexedIndirectCount`）
- 剔除统计 `CullingStats`

公开方法: `Draw()`, `InitImGui()`, `ProcessImGuiEvent()`, `GetMainCamera()`

#### `Src/DX12/DX12GpuScene.cpp`
当前实现内容（~220 行）：
- **构造函数**: 初始化 Camera、Descriptor Heap、每帧 Upload Buffer、加载 mesh 数据
- **LoadMeshData()**: 加载 `debug1.bin` 和 `debug1.bin.json`
- **CreateCommandSignature()**: 创建 `DRAW_INDEXED` 间接命令签名（20 字节 stride = `D3D12_DRAW_INDEXED_ARGUMENTS`）
- **UpdateUniforms()**: 每帧更新 projection/view/invProjection 矩阵 + frameCounter 到 persistently mapped uniform buffer
- **ReadbackCullingStats()**: 从上一帧的 upload buffer 读取可见 chunk 计数
- **Draw()**: 主帧循环
  1. `UpdateUniforms()`
  2. `ReadbackCullingStats()`
  3. `BeginFrame()` + 设置 descriptor heaps + viewport/scissor
  4. Clear RTV (黑色) + DSV (0.0, reverse-Z)
  5. `RenderImGuiOverlay()` — DX12 版 ImGui 统计窗口
  6. `EndFrameAndPresent()`
- **ImGui 集成**: `ImGui_ImplDX12_Init` + `ImGui_ImplSDL2_InitForD3D`，描述符从静态 heap[0] 分配

### 修改文件

#### `Src/CMakeLists.txt`
DX12 源文件列表新增 `DX12DescriptorHeap.cpp` 和 `DX12GpuScene.cpp`

#### `Src/Window.cpp`
DX12 主循环从简单清屏升级为完整的 `DX12GpuScene` 流程：
- 创建 `DX12Device` + `DX12GpuScene`
- `InitImGui(window)`
- 完整 SDL 事件处理（键盘 WASDQE 移动 + 鼠标拖拽旋转）
- 每帧调用 `dx12Scene.Draw()`

---

## 当前状态

| 组件 | 状态 |
|------|------|
| Shader 双后端宏 | ✅ 完成 |
| CMake ENABLE_DX12 | ✅ 完成 |
| DX12Device | ✅ 完成 |
| Descriptor Heap 管理器 | ✅ 完成 |
| Resource Helper | ✅ 完成 |
| DX12GpuScene 骨架 | ✅ 完成（Clear + ImGui） |
| Mesh 数据加载 | ✅ 完成（AAPLMeshData） |
| Uniform 更新 | ✅ 完成（Camera matrices + frameCounter） |
| ImGui DX12 后端 | ✅ 完成 |
| Camera 控制 | ✅ 完成（WASDQE + mouse） |
| ExecuteIndirect 签名 | ✅ 完成 |
| VB/IB/Texture 上传 | ✅ 完成 |
| Root Signature 创建 | ✅ 完成 (×6) |
| PSO 创建 | ✅ 完成 (×7) |
| Occluder 深度渲染 | ✅ 完成 |
| G-Buffer 创建 | ✅ 完成 |
| Shadow Maps | ✅ 完成（3 cascade 渲染 + SRV） |
| Base Pass 渲染 | ✅ 完成（ExecuteIndirect） |
| HiZ + GPU Culling | ✅ 完成（GPU Cull dispatch + ExecuteIndirect） |
| SAO Compute | ✅ 完成 |
| Deferred Lighting | ✅ 完成 |
| Forward Pass | ✅ 完成（alpha blending + ExecuteIndirect） |
| ExecuteIndirect | ✅ 完成 |
| Bindless 纹理 | ✅ 完成（BC1/BC3/BC5 + mip upload） |

---

## Phase 3: VB/IB 上传 + Root Signatures + PSOs + Occluder 渲染

### DX12GpuScene.cpp 新增实现

#### `CreateBuffers()` (~100 行)
LZFSE 解压 → `CreateDefaultBuffer()` staging 上传：
- VB: position/normal/tangent/UV (各一个 ID3D12Resource)
- IB: uint32, mesh chunks SB, material SB
- Occluder VB/IB (从 JSON)
- Per-frame: drawParams UAV + chunkIndices UAV

#### `CreateRootSignatures()` (~200 行)
| Root Signature | 参数 |
|---------------|------|
| `_occluderRootSig` | [0] CBV b0 |
| `_drawClusterRootSig` | [0] CBV, [1] SRV table (space1 + space2 bindless), [2] Root constants |
| `_gpuCullRootSig` | [0] CBV cullParams, [1] UAV+SRV table, static sampler |
| `_deferredLightingRootSig` | [0] CBV, [1] SRV table (11 slots), 2 static samplers |
| `_hizRootSig` | [0] SRV+UAV table, [1] Root constants ×4 |
| `_saoRootSig` | [0] SRV+UAV table, [1] CBV cam, [2] Root constants ×2 |

#### `CreatePipelineStates()` (~180 行)
| PSO | 类型 | DSVFormat | RTVFormats |
|-----|------|-----------|------------|
| `_occluderPSO` | Graphics | D32_FLOAT | 无 (depth-only) |
| `_basePassPSO` | Graphics | D32_FLOAT | 4 MRT (sRGB×3 + F16×1) |
| `_deferredLightingPSO` | Graphics | D32_FLOAT | 1 (swap chain) |
| `_gpuCullPSO` | Compute | - | - |
| `_hizCopyPSO` | Compute | - | - |
| `_hizDownsamplePSO` | Compute | - | - |
| `_saoPSO` | Compute | - | - |

#### `CreateGBuffers()` (~30 行)
4 G-buffer + occluder depth + AO texture

#### Draw() 渲染管线（当前）
```
Clear → Occluder depth pass → (TODO: HiZ/Cull/BasePass/Deferred/Forward) → ImGui → Present
```

### 其他修改
- `shaders/shadowcull.hlsl`: `[unroll]` → `[unroll(3)]` 修复 DX12 编译
- 全部 20 个 .cso 入口点编译通过

---

## Phase 4: Base Pass G-buffer 渲染 + Deferred Lighting Draw

### DX12GpuScene.cpp 改动

#### `CreateGBuffers()` 升级
- 新增 `_gbufferRtvHeap` (4 descriptors, non-shader-visible) 和 `_gbufferDsvHeap` (1 descriptor)
- 为每个 G-buffer 创建 RTV descriptor
- 为 occluder depth 创建 DSV descriptor
- G-buffer 带 `D3D12_CLEAR_VALUE` 以支持快速清除

#### `Draw()` 渲染管线（当前完整流程）
```
1. UpdateUniforms → 写 matrices + frameCounter
2. ReadbackCullingStats → 读上一帧 writeIndex
3. BeginFrame → barrier to RENDER_TARGET
4. Set heaps + viewport + scissor
5. Clear swap chain RTV + window DSV
6. ★ Occluder depth pass → 渲染 occluder 到 _depthTexture (单独 DSV)
7. ★ Base pass → 清除 4 G-buffer RTVs + 绑定 4 MRT + window DSV
        → 设置 drawCluster root sig + PSO + VB/IB
        → (TODO: 绑定 material descriptor table, 录制 draw calls)
8. ★ G-buffer transition → RENDER_TARGET → PIXEL_SHADER_RESOURCE (×4)
9. ★ Deferred lighting → 设置 deferred root sig + PSO
        → 绑定 swap chain RTV (无 DSV)
        → DrawInstanced(3,1,0,0) 全屏三角形
        → (TODO: 绑定 G-buffer SRV descriptor table)
10. G-buffer transition back → PIXEL_SHADER_RESOURCE → RENDER_TARGET (×4)
11. ImGui overlay → DX12 ImGui render
12. EndFrameAndPresent → barrier to PRESENT + execute + present
```

### DX12GpuScene.h 改动
新增成员: `_gbufferRtvHeap`, `_gbufferDsvHeap`, `_gbufferRtvSize`

---

## Phase 5: Descriptor 绑定 + GPU Cull 准备

### 新增实现

#### `CreateStaticDescriptors()` (~60 行)
在 shader-visible CBV/SRV/UAV heap 中创建固定位置的 SRV：

| 索引 | 内容 | 用途 |
|------|------|------|
| 0 | ImGui font SRV | ImGui 字体纹理 |
| 1-4 | G-buffer SRVs | deferred lighting 读取 |
| 5 | Window depth SRV (R32_FLOAT view of D32_FLOAT) | deferred + SAO |
| 6 | AO texture SRV (R8_UNORM) | deferred lighting |
| 7 | Shadow maps SRV (Texture2DArray R32_FLOAT) | deferred lighting |
| 10 | Materials structured buffer SRV | base pass pixel shader |
| 11 | MeshChunks structured buffer SRV | base pass + cull shader |

#### Deferred Lighting 绑定
- `SetGraphicsRootDescriptorTable(1, static GPU[1])` → 绑定 G-buffer + depth + AO SRVs
- 在 deferred lighting 前转换 window depth → `DEPTH_READ`，之后转回 `DEPTH_WRITE`

#### Base Pass Draw
- `SetGraphicsRootDescriptorTable(1, static GPU[10])` → 绑定 materials SRV
- 解压 chunk 数据，逐 chunk 设置 push constant (`materialIndex`) 并 `DrawIndexedInstanced`
- 当前 CPU-driven draws（后续可切换到 ExecuteIndirect）

#### GPU Cull 准备
- 每帧重置 writeIndex 计数器为零（memcpy to mapped upload buffer）
- 填充 `GPUCullParams` 结构体（matrices + frustum + chunk counts + screen size）
- drawParams/chunkIndices 转换为 UAV（待实际 dispatch 时使用）

---

## Phase 6: HiZ Pyramid + SAO Compute

### 新增实现

#### HiZ Pyramid Dispatch (~80 行)
1. 转换 occluder depth → `NON_PIXEL_SHADER_RESOURCE`
2. 转换 HiZ texture → `UNORDERED_ACCESS`
3. Copy pass: 动态分配 SRV+UAV descriptors，dispatch `_hizCopyPSO`
4. Downsample loop: 每 mip 间 UAV barrier，动态 SRV (prev mip) + UAV (cur mip)
5. 转换 HiZ → `NON_PIXEL_SHADER_RESOURCE` (供 cull shader 读取)

#### SAO Compute Dispatch (~120 行)
1. 从 window depth 构建 SAO depth pyramid（复用 HiZ copy + downsample PSO）
2. 转换 SAO pyramid → `NON_PIXEL_SHADER_RESOURCE`
3. 分配 3 个动态 descriptors：window depth SRV + SAO pyramid SRV + AO output UAV
4. 绑定 camera uniform CBV + screenSize root constants
5. Dispatch `_saoPSO`
6. 转换 AO texture → `PIXEL_SHADER_RESOURCE`

所有 descriptor 使用每帧环形缓冲动态分配，无需预分配固定 descriptor set。

#### `CreateHiZResources()` (~20 行)
创建 HiZ + SAO depth pyramid textures (R32_FLOAT, full mip chain, ALLOW_UNORDERED_ACCESS)

---

## Phase 7: Shadow Maps

### 新增实现

#### `CreateShadowResources()` (~50 行)
- `_shadowMapArray`: `Texture2DArray` (R32_TYPELESS, 1024×1024, 3 cascades, ALLOW_DEPTH_STENCIL)
- `_shadowDsvHeap`: 3 per-cascade DSV descriptors (D32_FLOAT view)
- Shadow SRV 写入 static heap[7] (R32_FLOAT view, Texture2DArray)

#### Draw() 中的 shadow pass
- TODO: 实际 shadow draw 需要 shadow VP matrices (来自 Shadow 类)
- 当前已创建资源和 DSV/SRV，待录制 shadow pass 绘制命令

---

## 当前完整渲染管线 (Draw())

```
1. UpdateUniforms → matrices + frameCounter
2. ReadbackCullingStats → 读上一帧 writeIndex
3. BeginFrame → reset cmd, barrier to RT
4. Set heaps + viewport + scissor
5. Clear swap chain RTV + window DSV
6. Occluder depth pass → DrawIndexedInstanced into _depthTexture
7. ★ HiZ pyramid → Copy occluder depth + downsample mip chain (compute)
8. GPU cull prep → Reset counters + fill cullParams
9. ★ Base pass → 4 MRT G-buffers + window depth, CPU-driven chunk draws
10. G-buffer transition → RT → PIXEL_SHADER_RESOURCE
11. ★ SAO pyramid → Copy window depth + downsample (compute)
12. ★ SAO compute → Dispatch SAO → AO texture (compute)
13. ★ Deferred lighting → 全屏三角形, bind G-buffer SRVs + depth + AO
14. G-buffer transition back → SRV → RT
15. AO transition → SRV → COMMON
16. ImGui overlay
17. EndFrameAndPresent → barrier to PRESENT + execute + present
```

---

## 下一步

所有核心功能已实现。后续优化:
- Shadow pass 使用 shadow cull compute + ExecuteIndirect（当前使用全量 DrawIndexedInstanced）
- HiZ texture 绑定到 GPU cull descriptor table（当前 HiZ pyramid 已生成但 cull shader 中的 HiZ 采样待验证）
- 纹理 sampler 精确绑定验证（bindless array 的 register space 对应关系）
- PIX 抓帧对比 Vulkan/DX12 输出一致性

---

## Phase 8: ExecuteIndirect + GPU Cull 完善

### 改动

#### writeIndex 架构重构
- `writeIndexBuffer` 从 upload heap 改为 **default heap + UAV**（`D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`）
- 新增 `writeIndexUpload`（upload heap，用于每帧零初始化 copy）
- 新增 `writeIndexReadback`（readback heap，`D3D12_HEAP_TYPE_READBACK`，用于 CPU 读取剔除统计）
- 流程: upload(zeros) → CopyBufferRegion → GPU(UAV) → compute dispatch → CopyBufferRegion → readback

#### GPU Cull Compute Dispatch
1. 从 upload buffer 拷贝零值到 writeIndex GPU buffer
2. 更新 cullParams (matrices + frustum + chunk counts + screen size)
3. 动态分配 5 个 descriptors: u0(drawParams), u1(writeIndex), u2(chunkIndices), t3(meshChunks), t4(hizTexture)
4. `SetComputeRootSignature` + `SetComputeRootConstantBufferView(0, cullParams)`
5. `SetComputeRootDescriptorTable(1, cullDescs.gpu)`
6. `Dispatch((totalChunks+127)/128, 1, 1)`
7. UAV barrier → 转换 drawParams/writeIndex 到 `INDIRECT_ARGUMENT`

#### ExecuteIndirect 替代 CPU draws
- Opaque: `ExecuteIndirect(cmdSig, opaqueCount, drawParams[0], writeIndex[0])`
- Alpha-mask: `ExecuteIndirect(cmdSig, alphaCount, drawParams[opaqueOffset], writeIndex[sizeof(uint32_t)])`
- Transparent: `ExecuteIndirect(cmdSig, transpCount, drawParams[transpOffset], writeIndex[2*sizeof(uint32_t)])`

#### Readback
- 帧末 `CopyBufferRegion(readback, writeIndex)` → 下帧 CPU 读取

---

## Phase 9: Shadow Pass 渲染

### 改动

#### Shadow VP 矩阵计算
- 使用太阳方向构建 3 个 cascade 的正交投影 shadow view/projection 矩阵
- 手动构建 view matrix（eye + sunDir, cross product basis vectors）
- `orthographic()` 正交投影

#### Shadow PSO
- 复用 `drawclusterShadow.vs.cso`，depth-only，`D3D12_COMPARISON_FUNC_GREATER`
- depth bias: 1000 + slope scaled 1.0（减少 shadow acne）
- DSVFormat: `DXGI_FORMAT_D32_FLOAT`

#### Shadow 渲染流程
- 3 cascade loop: 清除 cascade DSV → 设置 shadow viewport (1024×1024) → draw all indices
- 完成后转换 shadow map array → `PIXEL_SHADER_RESOURCE`
- 帧末转换回 `DEPTH_WRITE`

---

## Phase 10: Forward Pass

### 改动

#### Forward PSO
- `drawcluster.forward.indirect.ps.cso` + alpha blending
- `SrcBlend=SRC_ALPHA, DestBlend=INV_SRC_ALPHA`
- depth test 开启，depth write 关闭（`DEPTH_WRITE_MASK_ZERO`）
- 输出到 swap chain RTV

#### Forward 渲染
- 在 deferred lighting 之后，绑定 VB/IB/materials
- `ExecuteIndirect` 绘制透明 chunks: offset = opaqueCount + alphaCount, count from writeIndex[2]

---

## Phase 11: Bindless 纹理加载

### 改动

#### Metal → DXGI 格式映射
| MTLPixelFormat | DXGI_FORMAT |
|---------------|-------------|
| BC3_RGBA_sRGB (302) | BC3_UNORM_SRGB |
| BC5_RGUnorm (312) | BC5_UNORM |
| BC1_RGBA_sRGB (292) | BC1_UNORM_SRGB |

#### `CreateTextures()` (~160 行)
1. 遍历 `AAPLMeshData._textures`
2. 为每个纹理创建 `Texture2D` (BC compressed, multi-mip)
3. 逐 mip 解压 LZFSE → upload buffer（行对齐到 `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT=256`）
4. `CopyTextureRegion` 上传到 GPU
5. 转换到 `PIXEL_SHADER_RESOURCE`
6. 在 `_cbvSrvUavHeap` 的 bindless 区域 `[SRV_BINDLESS_START + texIdx]` 创建 SRV
7. 记录 `textureHashMap[pathHash] = texIdx`

#### 材质纹理索引配置
- 解压材质数据，通过 `textureHashMap` 查找 hash → texture index
- 配置 `AAPLShaderMaterial.albedo_texture_index` / `roughness` / `normal` / `emissive`
- 重新上传 material buffer + 重建 SRV

---

## 最终渲染管线 (Draw())

```
1. UpdateUniforms → matrices + shadow VP + frameCounter
2. ReadbackCullingStats → 读上一帧 readback buffer
3. BeginFrame → reset cmd, barrier to RT
4. Set heaps + viewport + scissor
5. Clear swap chain RTV + window DSV
6. ★ Shadow pass → 3 cascade depth rendering, transition to SRV
7. ★ Occluder depth pass → DrawIndexedInstanced into _depthTexture
8. ★ HiZ pyramid → Copy + downsample (compute)
9. ★ GPU Cull → Zero writeIndex, dispatch compute, transition to INDIRECT_ARG
10. ★ Base pass → 4 MRT G-buffers, ExecuteIndirect (opaque + alpha-mask)
11. G-buffer transition → RT → SRV
12. ★ SAO pyramid → Copy + downsample (compute)
13. ★ SAO compute → AO texture
14. ★ Deferred lighting → 全屏三角形, G-buffer + depth + shadow + AO SRVs
15. Depth transition back
16. ★ Copy writeIndex → readback buffer
17. ★ Forward pass → ExecuteIndirect (transparent + alpha blend)
18. G-buffer/AO/shadow transitions back
19. ImGui overlay
20. EndFrameAndPresent
```

---

## 文件变更总汇 (总计 ~2884 行新增 DX12 代码)

### 新增文件
| 文件 | 行数 | Phase |
|------|------|-------|
| `shaders/shadercompat.hlsl` | 25 | 0 |
| `shaders/compile_shaders_dx12.bat` | 40 | 0 |
| `Src/DX12/DX12Setup.h` | 96 | 1 |
| `Src/DX12/DX12Setup.cpp` | 241 | 1 |
| `Src/DX12/DX12DescriptorHeap.h` | 54 | 2 |
| `Src/DX12/DX12DescriptorHeap.cpp` | 62 | 2 |
| `Src/DX12/DX12ResourceHelper.h` | 188 | 2 |
| `Src/DX12/DX12GpuScene.h` | 198 | 2-final |
| `Src/DX12/DX12GpuScene.cpp` | 1965 | 2-final |

### 修改文件
| 文件 | Phase | 改动说明 |
|------|-------|---------|
| `CMakeLists.txt` | 0 | +1 行 ENABLE_DX12 option |
| `Src/CMakeLists.txt` | 0+2 | DX12 源文件 + 库链接 |
| `Src/Window.cpp` | 1+2 | DX12 后端分支 + DX12GpuScene 主循环 |
| `shaders/*.hlsl` (×9) | 0 | 双后端绑定宏改造 |
| `shaders/shadowcull.hlsl` | 3 | `[unroll]` → `[unroll(3)]` 修复 DX12 编译 |
