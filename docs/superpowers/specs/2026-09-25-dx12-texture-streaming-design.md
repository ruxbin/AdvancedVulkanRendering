# DX12 Texture Streaming 设计

日期：2026-09-25
状态：已确认（用户口头批准设计分段）
分支：texture_streaming

## 目标

为 DX12 后端补齐**真正的 texture streaming**,与 Vulkan 端(`GpuScene.cpp`,镜像 Metal AAPLTextureManager 的 mip-streaming 设计）逐结构对齐：

- 显存占用降到「permanent mip(≤64px)水位 + 按需流入高分辨率 mip」
- 双向 streaming：走近升级（upgrade)、走远驱逐（evict)
- 后台线程做解压/建资源，主线程只做 copy submit 与描述符重写

非目标：不改变 shader、不改变材质系统、不引入 D3D12 reserved/tiled resources（路线 3，已否决）、不做无 stall 描述符按帧复制（路线 2，已否决）。

## 现状

### Vulkan 端（对齐目标）

- 启动只上传 permanent mip(`CreateTextures`,`Src/GpuScene.cpp:5145`);`currentMip = permanentMip`。
- 每帧 `UpdateTextureStreaming`(`Src/GpuScene.cpp:5775`):reset `requiredMip` → 逐 submesh 包围球屏幕面积 → `setRequiredMip` MIN 累积 → dispatch → `processStreamingWork` → 释放 4 帧前的退役纹理。
- 后台 `blitThreadFunc`(`Src/GpuScene.cpp:5451`):建只含目标 mip 段的新 image、解压新 mip 到 per-work staging、算两类 copy region（新 mip:buffer→image；共享 mip:image→image，升级/驱逐两种下标映射）。
- 主线程 `processStreamingWork`(`Src/GpuScene.cpp:5659`):barrier + copy 一次 submit,`endSingleTimeCommands` 内部 `vkQueueWaitIdle`(**全停**)，换 image,descriptor dirty mask 按帧槽置位，旧 image 进 `textureToDelete[frameIndex]`(TEXTURE_RETENTION_FRAMES=4)。
- `inFlight` 防重复 dispatch;`snapshotCurrentMip/snapshotImage` 快照避免与主线程 data race。

### DX12 端（桩）

`DX12GpuScene::UpdateTextureStreaming`(`Src/DX12/DX12GpuScene.cpp:1788`）仅按相机距离改 static SRV 的 `MostDetailedMip`——全部 mip 永久常驻，**零显存节省**，且每次改动都 `WaitForGpu` 全停。32MB `_streamingStagingBuffer` 已分配但代码内标注 stub、从未使用。

### 已确认的决策

| 问题 | 决策 |
|---|---|
| 架构 | 完整对齐 Vulkan（后台线程） |
| 启动常驻 | 只加载 permanent mip（与 Vulkan 一致；开局远景糊、随 streaming 变清晰） |
| 实现路线 | 路线 1：忠实移植 + swap 时 `WaitForGpu` + 原地重写 SRV + retention ring |

### DX12 特有的硬约束

bindless SRV 位于 static shader-visible heap(`_cbvSrvUavHeap`,slot `SRV_BINDLESS_START=15` 起，每纹理 slot 稳定，材质 buffer 按纹理 index 引用）,3 个 in-flight 帧共享同一份描述符 → 原地重写 slot 必须 GPU 空闲。Vulkan 换 mip 时 `vkQueueWaitIdle` 本来也是全停，**停顿特征对等**，且都只在真有 swap 完成时发生。

## 设计

### §1 总体架构（镜像 Vulkan 五件套）

`DX12GpuScene` 新增/升级：

| Vulkan | DX12 |
|---|---|
| `initTextureStreaming` / `shutdownTextureStreaming` | 同名私有方法；启停后台线程 |
| `UpdateTextureStreaming(frameIndex)` | 升级现有桩（`Draw()` 开头现有调用点不变，`DX12GpuScene.cpp:2439`) |
| `dispatchStreamingRequest` → `blitThreadFunc` | 同名私有方法；**CPU only** 后台线程 |
| `processStreamingWork` | 同名私有方法；主线程 swap |
| `streamingEntries` / `streamingEntryMap` / `inFlight` | `_streamEntries`（扩展现有 struct)/ `_streamEntryMap` / `inFlight`，同语义 |
| `textureToDelete` 4 帧 ring | `_retiredTextures[DX12Device::FRAME_COUNT]`，帧 fence 驱动释放（见 §4) |

**线程分工**：后台线程只做 CPU 工作——从 `_applMesh->_textureData` 解压目标 mip、`CreateCommittedResource` 建新纹理（D3D12 device 方法 free-threaded)、填 per-work upload buffer、算 copy region。主线程独占：命令录制、Execute、SRV 重写、entry 状态 swap。

`_streamEntries` 扩展现有 `TextureStreamEntry`:

```cpp
struct TextureStreamEntry {
  const AAPLTextureData* desc = nullptr; // 指向 _applMesh->_textures[k]
  uint32_t totalMips  = 1;
  uint32_t currentMip = 0;   // 当前常驻的最细 mip(0=满分辨率)
  uint32_t requiredMip = 0;  // 本帧需求(MIN 累积后)
  bool inFlight = false;     // 已有 work item 在途,防重复 dispatch
};
```

### §2 启动路径（CreateTextures 改造)

`DX12GpuScene::CreateTextures`(`Src/DX12/DX12GpuScene.cpp:412`）改为只上传 `[permanentMip, totalMips)` 段，直译 Vulkan `CreateTextures`(`Src/GpuScene.cpp:5199-5257`):

- 纹理创建尺寸 `w>>permanentMip × h>>permanentMip`,`MipLevels = totalMips - permanentMip`;subresource `i` ← 源 mip `permanentMip + i`。
- SRV `MipLevels = totalMips - permanentMip`（从新资源的 mip 0 起）。
- `_streamEntries[k]`: `desc = &_applMesh->_textures[k]`、`currentMip = requiredMip = permanentMip`、`inFlight = false`;`_streamEntryMap[desc->_pathHash] = k`。
- **删除** 32MB `_streamingStagingBuffer` 桩（`DX12GpuScene.h:113-115`)，改 per-work upload buffer（对齐 Vulkan 每 work 独立 staging)。
- `permanentMip = calculateMinMip(desc, PERMANENT_TEXTURE_SIZE=64)`。

### §3 覆盖度计算（每帧）

新增两个 CPU 侧缓存：

- `_subMeshes`(`AAPLSubMesh*`):`LoadMeshData` 时 `decompressToHeap(_applMesh->_meshData, ...)`（对齐 `GpuScene.cpp:2773`)，析构 `free`。
- `_cpuMaterials`(`AAPLMaterial*`):`CreateBuffers` 已经解压过一次 `_materialData`(`DX12GpuScene.cpp:301`)，改为保留成员、不再 `free`，析构释放。

每帧 `UpdateTextureStreaming`:

1. reset:逐 entry `requiredMip = calculateMinMip(*desc, PERMANENT_TEXTURE_SIZE)`。
2. 逐 submesh：`sphereInFrustum` 剔除包围球 → 屏幕面积公式照抄 `GpuScene.cpp:5803-5817`(`focalLength = proj[0][0]`，只用 x 缩放与 view z，与 NDC Y、负高度 viewport 无关）→ 对该 submesh 材质的 4 类纹理（baseColor/normal/metallicRoughness/emissive）按 hash `setRequiredMip`,**MIN 累积**（最近的 chunk 赢）。
3. **清空 `_retiredTextures[_currentFrame]`**——必须早于 dispatch/processStreamingWork：本帧 processStreamingWork 会往同一槽压入新退役资源（其 copy 刚提交、仍在途），不能一并清掉（顺序对齐 Vulkan `UpdateTextureStreaming` 先 swap 出 `toFreeThisFrame` 再 dispatch 的结构；安全性依据见 §4)。
4. dispatch:`currentMip != requiredMip && !inFlight` 的 entry 逐个 `dispatchStreamingRequest`。
5. `processStreamingWork()`。

`calculateMinMip / calculateRequiredMip / sphereInFrustum / BC 块大小与 mip 尺寸计算` 抽成 **`Src/DX12/DX12TextureStreamingPolicy.h`** 纯函数（对齐 `DX12ScenePolicy.h` 模式），运行时代码与测试共用。公式与 `GpuScene.cpp:5363-5383` 相同：

- `calculateMinMip(desc, maxSize)`:`log2(max(dim)/maxSize)` clamp 到 `[0, mipCount-1]`。
- `calculateRequiredMip(desc, screenArea)`:`0.5*log2(texArea/screenArea)` clamp 到 `[topMip(MAX_TEXTURE_SIZE=4096), botMip(PERMANENT_TEXTURE_SIZE=64)]`;`screenArea<=0` 取 botMip。

### §4 Swap 时序与同步（核心节)

**Work item 结构**（镜像 Vulkan `PendingBlit` / `CpuStreamingWork`):

```cpp
struct PendingStreamWork {
  size_t entryIndex;
  uint32_t targetMip;
  uint32_t snapshotCurrentMip;          // 快照,防与主线程 data race
  ComPtr<ID3D12Resource> snapshotSource; // 旧资源,保活到 copy
};

struct CpuStreamingWork {
  size_t entryIndex;
  ComPtr<ID3D12Resource> newTexture;     // 失败时为 nullptr
  ComPtr<ID3D12Resource> staging;        // 纯驱逐时可为空
  uint32_t targetMip, currentMip, oldMipCount, newMipCount;
  DXGI_FORMAT format;
  struct BufferCopy { uint64_t stagingOffset; uint32_t w, h, rowPitch, dstSubresource; };
  std::vector<BufferCopy> bufferCopies;  // staging→new(新增 mip)
  struct ImageCopy { uint32_t srcSubresource, dstSubresource, w, h; };
  std::vector<ImageCopy> imageCopies;    // old→new(共享 mip)
  ComPtr<ID3D12Resource> sourceTexture;  // 旧资源(拷贝源)
};
```

**后台线程**（每个 work):

- 新资源：`CreateCommittedResource`(DEFAULT heap，尺寸 `w>>targetMip × h>>targetMip`,`MipLevels = totalMips - targetMip`，初始状态 `COPY_DEST`,flag 无）。
- `addingMips = targetMip < currentMip`:
  - 升级：`mipStart=targetMip, mipEnd=currentMip`（只上传新增段）；共享 mip 映射 `old sub m → new sub m + (currentMip - targetMip)`。
  - 驱逐：`mipStart=mipEnd=targetMip`（不上传）；共享映射 `old sub m + (targetMip - currentMip) → new sub m`。
- staging：按 BC 块尺寸算每个新 mip 的 `rowPitch`(`(w+3)/4 * bytesPerBlock`,8 或 16 字节/块，与 `CreateTextures` 现有口径一致），对齐 `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`;`decompressToHeap` 解压源 mip 后逐行拷入（行对齐）。
- 失败：`newTexture` 置空，仍产出 work（主线程据此只复位 `inFlight`，对齐 Vulkan `continue` 语义）。
- 线程同步原语照抄 Vulkan:`_pendingMutex + _pendingCv`、`_cpuWorkMutex`、`_streamingThreadRunning`。

**主线程 `processStreamingWork`**（只在完成队列非空时）:

1. `_device.WaitForGpu()` — GPU 空闲（对等 Vulkan `vkQueueWaitIdle`)。
2. 专用 `_streamingCmdList + _streamingAllocator`(DIRECT 类型，与主 cmdlist 同 queue）逐 work 录：
   - old:`PIXEL_SHADER_RESOURCE → COPY_SOURCE`（仅当有共享 mip copy)
   - `CopyTextureRegion`:staging→new(`PLACED_FOOTPRINT` + `SUBRESOURCE_INDEX`);old→new(`SUBRESOURCE_INDEX` 对）
   - new:`COPY_DEST → PIXEL_SHADER_RESOURCE`
   - old **不转回**（即将退役；GPU 已空闲，无引用）
   - 注：new 初始状态即 `COPY_DEST`，无需进入 barrier。
3. Close + Execute。帧尾 `MoveToNextFrame` 的 Signal 覆盖本次提交。
4. 重写 SRV slot(`SRV_BINDLESS_START + k`,slot 稳定）:`MostDetailedMip=0, MipLevels=newMipCount`。CPU 写描述符时 GPU 在跑 copy 合法——copy 不读该描述符；后续帧的记录晚于此刻，见到的是新描述符。
5. swap `_textures[k] = work.newTexture`;`entry.currentMip = targetMip; inFlight = false`。
6. 旧资源 + staging 压入 `_retiredTextures[_currentFrame]`。**不等 copy 完成**。

**Retired 释放的安全性**:`DX12Device::MoveToNextFrame`(`DX12Setup.cpp:233`）复用帧槽前必等该槽上次的 fence。`Draw()` 开头 `_currentFrame = GetFrameIndex()` 时，该槽上一轮的 GPU 工作（含引用旧资源的 copy）已完成 → 清空 `_retiredTextures[_currentFrame]`(§3 第 3 步，在 dispatch 之前）安全。无需第二次 stall。

**`WaitForGpu` 与帧 fence 的共存**:`WaitForGpu`(`DX12Setup.cpp:222`）将当前槽 fence 加 1 并等待；`MoveToNextFrame` 读到抬高后的值继续单调递增。init 路径（`CreateTextures` 经 `FlushCommandQueue`）已多次这样交错，现有代码证明无恙。

**录制位置**:`UpdateTextureStreaming` 在 `_device.BeginFrame()` 之前被调用（`DX12GpuScene.cpp:2439`)，主 cmdlist 尚未录制；专用 streaming cmdlist/allocator 不干扰主流程。

### §5 错误处理与关闭

- 后台线程 `CreateCommittedResource` / Map / 解压失败：产出 `newTexture==nullptr` 的 work；主线程只复位 `inFlight`，下一帧可按新需求重新 dispatch。
- `shutdownTextureStreaming`：置 running=false + notify + join；清 pending 队列、完成队列；`_retiredTextures` 全部槽清空（析构路径先 `WaitForGpu`)。在 `~DX12GpuScene` 中先于纹理资源释放调用。

### §6 可观测性与测试

- ImGui 现有 streaming 块（`DX12GpuScene.cpp:3686`）扩展：常驻显存估算（按各 entry `currentMip` 的 BC 块尺寸累计）、pending/in-flight 计数。
- spdlog debug 级 swap 日志（hash、old→new mip、resident 变化）。
- **`Tests/DX12TextureStreamingPolicyTests.cpp`**：沿用 `DX12ScenePolicyTests.cpp` 的无框架 main 模式，覆盖：
  - `calculateMinMip` / `calculateRequiredMip` 边界（clamp、screenArea≤0、非 2 幂尺寸）
  - upgrade 与 evict 两个方向的 copy region：子资源下标映射、staging 偏移累加、rowPitch 256 对齐
  - MIN 累积语义
  - （测试构建接线现状：`CMakeLists.txt:75-77` 被注释，本次不顺手恢复；与 `DX12ScenePolicyTests.cpp` 保持同构即可。)
- 端到端手工验证：开局显存降到 64px 水位、走近物体变清晰、走远回落；RenderDoc 抓帧看纹理 mip 数与 SRV。

## 影响文件

| 文件 | 改动 |
|---|---|
| `Src/DX12/DX12GpuScene.h` | entry struct 扩展、work struct、线程/同步原语、`_subMeshes`/`_cpuMaterials`/`_retiredTextures`/streaming cmdlist 成员、5 个方法声明；删 `_streamingStagingBuffer` |
| `Src/DX12/DX12GpuScene.cpp` | `CreateTextures` 改造、`LoadMeshData`/`CreateBuffers` 保留 CPU 数据、`UpdateTextureStreaming` 重写、新增 dispatch/blitThreadFunc/processStreamingWork/init/shutdown、析构、ImGui 扩展 |
| `Src/DX12/DX12TextureStreamingPolicy.h` | 新增：纯函数（mip 公式、块尺寸、copy region 计算、sphereInFrustum) |
| `Tests/DX12TextureStreamingPolicyTests.cpp` | 新增：policy 单测 |

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| swap 帧全停造成卡顿 | 与 Vulkan 停顿特征对等（只在 swap 完成时）;AAPL 式设计本就接受；后续如需消除可走路线 2，架构上留有余地 |
| 后台线程与主线程竞态 | 快照字段 + `inFlight` + 互斥锁照抄 Vulkan 已验证模式 |
| 多 work/帧导致 staging 内存尖峰 | per-work staging 随 swap 完成即进 retention，最坏 3 帧释放；单帧 dispatch 量自然受「每 entry 一个 inFlight」限制 |
| BC 格式 rowPitch/对齐错误 | 口径与 `CreateTextures` 现有逐 mip 上传完全一致；policy 单测覆盖对齐计算 |
| 开局糊（产品观感） | 与 Vulkan 行为一致，用户已确认接受 |
