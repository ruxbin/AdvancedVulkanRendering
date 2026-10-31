# 鼠标单击选择 Cluster 与所属 Mesh：功能设计

日期：2026-10-28
项目：AdvancedVulkanRendering
状态：设计建议，尚未实现。本文以当前工作区源码为依据；交互默认值与技术取舍由设计者提出，未视为用户已确认。

## 1. 目标与推荐方案

在场景窗口内单击鼠标左键，识别该屏幕位置最前方的可选几何表面，同时保存命中的 **cluster ID** 和 **所属 mesh ID**。选中结果保持到下一次有效单击、主动清除或场景重载，鼠标移动不改变选择。

推荐采用 **按需 GPU ID Pass + 单像素异步读回 + CPU cluster→mesh 映射表**。点击时额外绘制几何 ID，复用当前场景的间接绘制数据、相机和材质 alpha 判定；GPU 返回稳定的 cluster ID，CPU 查询所属 mesh。选中反馈包括编辑器信息面板和可关闭的包围盒标记。

此方案解决现有包围盒拾取会选中空隙、遮挡错误、不能识别 cluster 的问题。不要求硬件光线追踪，不修改资产二进制布局，不要求常驻新增 G-buffer MRT。

### 范围

- 完整功能覆盖桌面 Vulkan 和 DX12 光栅渲染路径，建议先完成 Vulkan，再对齐 DX12。
- 支持 opaque、alpha-mask，以及按明确规则参与拾取的 alpha-blend 几何。
- 本期不做多选、框选、物体移动、材质编辑、原始模型节点重建、触屏交互或离线跨加载 ID 持久化。
- Vulkan 全光追模式本期不支持精确拾取：禁用选择入口并显示原因，切换模式取消待处理请求。不得返回与光追画面不一致的光栅结果。

## 2. 现有代码与术语

| 项目概念 | 当前代码 | 设计含义 |
|---|---|---|
| cluster | `AAPLMeshChunk`、`m_Chunks`，shader 中 `meshChunks` | 资产中的全局 chunk 数组索引；UI 统一称 Cluster |
| mesh | `AAPLSubMesh`、Vulkan `m_SubMeshes`、DX12 `_subMeshes` | 用 `chunkStart/chunkCount` 描述所属 cluster 区间的材质分组 |
| 资产计数 | `AAPLMeshData::_chunkCount/_meshCount` | 映射与结果校验的范围依据 |
| GPU 可见集合 | `gpucull.hlsl` 的 `chunkIndices`、间接参数和分类计数 | 压缩后的 draw slot 每帧可变，不能作为选中 ID |
| 旧拾取 | `GpuScene::queryMeshAtScreenPos()` | 对所有 submesh 做射线–AABB 测试，返回 `mesh_N.ply` 字符串 |
| 旧 UI | `GpuScene::buildImGuiFrame()` 的 `Mesh Picker` | 悬停查询，无持久 cluster 选择状态 |
| 输入 | `Window.cpp`、`RouteSceneInput()` | 已有 ImGui 捕获、独立工具窗路由、相机拖拽与贴花操作 |
| 导出名称 | `PbrtExporter.cpp::WritePLYMesh()` | 当前 mesh 索引对应 `mesh_<meshId>.ply` 命名 |

**重要边界：本文的 mesh 是 `AAPLSubMesh`，不承诺对应原始 DCC 场景中的整张桌子、整栋房屋或完整模型节点。** 多材质物体可能由多个 mesh 组成。若以后需要“完整物体”，须在资产管线补充 object/node ID 和层级关系，不能通过材质或包围盒猜测。

旧 AABB 查询可保留为显式标注的调试工具，但新选择功能不能调用它作为精确拾取的自动降级路径。面板改为显示结构化 Selection，停止每帧悬停扫描所有 mesh。

## 3. 交互约定

以下为建议默认值：

| 操作/场景 | 行为 |
|---|---|
| 场景内左键按下后原地松开 | 松开时提交一次拾取请求 |
| 按住左键移动超过 4 个 SDL 逻辑像素 | 进入原有相机旋转；本次松开不拾取 |
| 左键移动未越过阈值 | 不旋转相机，避免微小抖动改变点击目标 |
| 右键拖拽 | 继续允许相机旋转，不产生选择请求 |
| 点击有效表面 | 同时更新 cluster 与 mesh；再次点击同一对象仍保持选中 |
| 点击背景/没有满足规则的表面 | 清空选择 |
| Esc 或面板 Clear Selection | 清空并使所有旧请求失效；Esc 服从键盘捕获 |
| 鼠标移出场景/移到 Editor | 保持已有选择，不发起新请求 |
| 贴花编辑模式 | 左键优先归贴花工具；保留现有选择，取消尚未完成的拾取 |
| UI 捕获、失焦、最小化、跨窗口释放 | 取消候选单击；不能遗留拖拽状态 |
| 资源/设备错误 | 显示错误，保留上次有效选择；不能把错误当作背景命中 |

点击候选保存按下位置、窗口 ID、最大移动距离及是否已进入拖拽。用整个按下期间的最大距离判断阈值，不能仅比较松开位置，否则“拖出去再拖回来”会被误判为单击。超过阈值时重置旋转起点，只从越过阈值后的增量开始旋转，避免跳变。

处理顺序：ImGui 先接收事件 → 取消/释放状态清理 → `RouteSceneInput` → 贴花模式判断 → 单击/拖拽状态机。特别注意当前 `resetDrag` 对任意鼠标释放均成立：不能先清除点击候选再检查合法的左键释放，应先保存候选或单独管理点击状态。

信息面板建议显示：状态（None/Pending/Selected/Error）、Cluster ID、Mesh ID、该 cluster 在 mesh 内的局部索引、材质 ID、cluster 三角形数、mesh 的 cluster 数及三角形数、包围盒、`mesh_N.ply` 导出名称。名称仅代表对应关系，不表示文件已生成。

待回读时保留旧结果并显示 Pending，不将旧数据伪装为新命中。选择标记默认显示 cluster 橙色包围盒和 mesh 青色包围盒；明确标注为包围盒辅助标记，可被遮挡关系穿透显示，不能用其轮廓判断实际命中精度。首版可用 ImGui 场景窗口前景绘制投影线段，需裁剪近面并限制在场景矩形；独立 Editor 窗口不能成为标记绘制目标。真实表面着色/描边留作后续增强。

## 4. 方案比较

| 方案 | 优点 | 代价/局限 | 结论 |
|---|---|---|---|
| CPU 射线–AABB | 可复用旧实现，开发量小 | 不是表面命中；重叠、空洞、alpha 都可能错误 | 仅调试用途 |
| CPU 射线–三角形 + BVH | 不用 GPU 读回，可返回三角形信息 | 新增加速结构；材质 alpha、纹理驻留及 GPU 可见性要额外对齐 | 可作为未来离线工具 |
| 主渲染常驻 ID MRT | ID 与主光栅直接对应，无额外几何 pass | 修改 G-buffer/PSO/透明路径，每帧写 ID 增加带宽 | 连续悬停需求出现后再评估 |
| 点击时独立 GPU ID Pass | 复用几何和材质规则，无点击时无额外几何绘制 | 有异步延迟；点击帧增加一次几何处理 | **本期推荐** |

## 5. 数据模型与 ID 归属

### 5.1 映射建立

场景加载且 cluster/submesh 解压完成后，构建只读 `std::vector<uint32_t> clusterToMesh`，长度为 `_chunkCount`，初始值 `InvalidId = UINT32_MAX`。

对每个 mesh `m`：

1. 用 64 位中间值验证 `[chunkStart, chunkStart + chunkCount)` 不越界；零 cluster 的 mesh 合法但不可命中。
2. 将区间中的 cluster 映射到 `m`，检测区间重叠，禁止静默覆盖。
3. 结束后检查所有可绘制 cluster 均有唯一归属。
4. 数据非法时禁用本场景选择功能并报告明确错误，正常渲染继续。不要用 `materialIndex` 代替归属关系：多个 mesh 可以使用同一材质。

映射初始化 O(cluster 数 + mesh 数)，查询 O(1)，额外 CPU 内存约 `4 × clusterCount` 字节。各后端共用构建与验证逻辑；原 `AAPLMeshChunk`、`AAPLSubMesh` 和压缩文件格式不变。

### 5.2 建议接口与状态

以下是拟新增接口，尚不存在于项目中：

```cpp
constexpr uint32_t InvalidId = UINT32_MAX;

struct SelectionState {
    uint32_t clusterId = InvalidId;
    uint32_t meshId = InvalidId;
    uint64_t sceneGeneration = 0;
    uint64_t acceptedRequestId = 0;
};

struct PickRequest {
    uint64_t requestId;
    uint64_t sceneGeneration;
    uint64_t viewportGeneration;
    uint64_t interactionGeneration; // 清空、模式切换等取消边界
    uint32_t pixelX, pixelY;
};

// 具体类型/命名可由实现者调整，行为契约保持不变。
void RequestPick(const PickRequest& request);
void ConsumeCompletedPick();
void ClearSelection();
const SelectionState& GetSelection() const;
```

`meshLocalClusterId = clusterId - submesh.chunkStart`。三角形数来自三角列表的 `indexCount / 3`，初始化应确认索引数能被 3 整除。选中状态不能保存指向已卸载资产的裸指针。

## 6. 从鼠标坐标到渲染像素

使用场景 SDL 窗口的逻辑尺寸与实际场景渲染目标尺寸分别换算，不能使用独立 Editor 窗口尺寸，也不能假设 DPI 为 100%。

```text
u = (mouseX - sceneRect.x) / sceneRect.width
v = (mouseY - sceneRect.y) / sceneRect.height
pixelX = floor(u * renderWidth)
pixelY = floor(v * renderHeight)
```

先验证坐标处于半开场景区域，尺寸均大于零，再转换。越界请求直接拒绝，不把窗口外点击 clamp 成边缘命中。当前场景占满主窗口；保留 `sceneRect` 参数便于以后嵌入视口或 letterbox。

这里得到的是目标图像的屏幕像素坐标，不要再无条件翻转 Y。两后端拾取 pass 应复用各自实际有效的主场景 viewport、投影矩阵、front-face 和裁剪约定；当前 DX12 的 viewport 设置与注释需要在实现时以实际 API 验证结果为准，不能把未经验证的负高度设置移植到新路径。用上/下、左/右四个已知目标验证方向。

首版定义“点击帧”为收到有效松开后**下一个成功提交的光栅帧**，拾取与该帧使用同一份相机矩阵、TAA jitter、场景数据和可见集合。不是对操作系统最近显示帧的历史回放。快速运动或 TAA 混合边缘可能与用户刚看到的像素有轻微差异，不能声称与时间重建后的颜色逐像素严格对应。无需从 TAA 历史颜色推导 ID，也不对整数 ID 做插值或 TAA。

## 7. GPU ID Pass

### 7.1 资源与 ID 输出

- ID 附件：Vulkan `VK_FORMAT_R32_UINT` / DX12 `DXGI_FORMAT_R32_UINT`，单采样，关闭 blending，清为 `UINT32_MAX`。
- 深度附件：独立的、与主场景匹配的受支持深度格式，单采样；reverse-Z 清为 0，使用 `GREATER` 并开启写深度。
- 两张图与场景分辨率一致。viewport 保持完整大小，scissor 限定到点击位置的 1×1 区域；不能将 viewport 改成 1×1 而不调整投影。
- Vulkan 可将 render area 限定为该像素并在区域内清 ID/深度；DX12 用局部 clear rect。其他像素未定义也无妨，只读取本次明确初始化的像素。
- shader 用独立输出结构，`nointerpolation uint clusterId` 从 VS 传到 PS，PS 输出 `uint` 到 `SV_Target0`。
- 初始化确认 ID 格式支持颜色附件与拷贝源；不支持则禁用并说明原因，不能降为有损浮点/颜色编码。

ID 只写全局 cluster 索引。Vulkan 通过 `firstInstance → chunkIndex[input.instancid]` 得到 ID；DX12 通过间接记录中的 root constant `drawSlot → chunkIndex[drawSlot]` 得到 ID。复用 `shadercompat.hlsl::GET_CHUNK_ID` 的契约。不能写 draw slot、`SV_PrimitiveID` 或 material ID。

### 7.2 绘制与透明规则

在 GPU culling 完成且间接参数/索引 SSBO 对 graphics 可见后，复用本帧三类可见 cluster 列表绘制。Picking PSO 复用主场景对应类别的几何、索引、背面剔除和纹理绑定；独立深度保证不需要依赖主深度最终布局，也不写入主深度。

| 类别 | 拾取规则 |
|---|---|
| Opaque | 覆盖该像素且通过深度测试的最近表面 |
| Alpha-mask | 与 `RenderSceneBasePassAlphaMask` 一致：采样 albedo alpha，执行 `clip(baseColor.a - ALPHA_CUTOUT)`；当前阈值 0.1 |
| Alpha-blend | 建议默认参与：`effectiveAlpha = baseColor.a * material.alpha`，小于 0.1 则丢弃；其余作为可选表面写 ID/深度 |

半透明像素没有唯一“所属物体”；本设计明确选取超过阈值的最近表面，**不计算最终颜色贡献最大者**。因此玻璃可能先于其后的物体被选中。面板提供 `Pick transparent surfaces`，关闭后跳过透明类别并允许选中其后的对象；该行为必须在 UI 可见。

透明拾取仍要写专用深度，不能复制主 forward 的禁写深度状态，否则结果依赖绘制顺序。opaque、alpha-mask、transparent 共用此专用深度。完全共面的重叠几何沿用绘制顺序决定胜者，不承诺跨帧固定胜者。

纹理与 sampler 使用本帧主渲染实际绑定版本，避免流式纹理不同 mip 导致孔洞判定不一致。当前 SVT 主要接入 opaque，opaque 拾取无需材质纹理；alpha-mask 继续使用原路径。未来 alpha-mask 接入 SVT 时必须同步更新 picker。拾取 shader 不执行光照、阴影、SVT feedback 或其他主渲染副作用。

1×1 scissor 限制像素处理，**不会消除顶点处理、间接命令执行或全分辨率附件的显存占用**。不能把它宣传成 O(1) GPU 拾取成本。

## 8. 异步读回与生命周期

### 8.1 流程

```text
有效单击 → Pending 请求（保留最后一个未提交请求）
         → 成功获取场景帧/资源槽
         → 本帧 culling → ID Pass → 拷贝 1 像素到 readback buffer
         → 随正常 graphics submission 提交并记录完成标识
         → 后续帧检查完成 → 校验请求版本及 ID → 更新 Selection
```

每个 in-flight 资源槽持有 ID 图、深度图、小型 readback buffer 和元数据。没有空闲槽时不阻塞，未提交请求只保留最新一次。GPU 已提交的请求必须等完成才能回收其资源，即使逻辑上已经取消。

仅接受 `requestId == latestRequestedId` 且 scene/viewport/interaction generation 均匹配的结果。用“等于最新请求”，而非仅“大于上次已接受”，避免新点击等待期间旧点击回读先覆盖 UI。背景 sentinel 清空选择；越界 cluster ID、缺失 mesh 映射是错误，不是背景。

取消、Esc、场景卸载、resize、切换渲染/工具模式均推进相关 generation 并丢弃逻辑结果。resize 保留已选 ID（同一场景），场景重载清空已选 ID。零尺寸和最小化暂停拾取，正常恢复时再分配资源。

### 8.2 Vulkan

当前 `GpuScene::Draw()` 中 `currentFrame == imageIndex`，`_syncSlot` 才是 command buffer/fence 的复用槽。建议新拾取资源按 `_syncSlot` 分配，取用主场景 descriptor 和间接参数时仍按 `currentFrame`。两个索引必须同时明确传入，禁止混用。

ID 写入完成后，用颜色附件写 → transfer read 的依赖转为 `TRANSFER_SRC_OPTIMAL`，通过 `vkCmdCopyImageToBuffer` 拷贝指定 1×1 texel；随后建立 transfer write → host read 的可见性依赖。buffer 使用 `TRANSFER_DST` 和 host-visible 内存；优先 coherent，否则 CPU 读取前按 `nonCoherentAtomSize` 对齐执行 invalidate。

在对应 `_syncSlot` fence 已完成后、reset/复用之前消费结果，复用现有帧等待。可以用非阻塞 fence 查询提前消费，但必须有 pending/consumed 标记，不能在 fence 重用后错认旧结果。不要为每次点击调用 `vkQueueWaitIdle` 或 `vkDeviceWaitIdle`。拷贝区域、单采样及容量要求参见 [Vulkan vkCmdCopyImageToBuffer](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImageToBuffer.html)。

### 8.3 DX12

ID 资源从 `RENDER_TARGET` 转到 `COPY_SOURCE`，使用 `CopyTextureRegion` 将单像素 source box 拷贝到 READBACK heap buffer 的有效 placed footprint。目标行 pitch/offset 必须满足 D3D12 对齐要求，不能只分配 4 字节就假设可拷贝。footprint 可通过 API 查询或按有效的单像素目标布局构造；CPU 只解析首个 uint。

为本次 graphics submission 记录真实 signal fence value，且在其完成后读取。现有 `DX12Device::MoveToNextFrame()` 管理 signal 和复用等待，实现时需要暴露提交完成值/查询接口，或提供等价的已完成帧槽回调；不要把 swapchain frame index 当完成证据。`Map` 本身不等待 GPU，见 [Microsoft：Read back data via a buffer](https://learn.microsoft.com/en-us/windows/win32/direct3d12/readback-data-using-heaps)。

两后端资源销毁均需等相关提交安全结束；逻辑取消不等于 GPU 已停止使用资源。设备丢失走已有设备错误处理，不重用未确认完成的 readback 数据。

## 9. 反馈与渲染隔离

首版的持久反馈是 Selection 信息面板与可关闭的包围盒标记，不依赖每帧全屏 ID 图，也不会让相机移动后选择自动变化。盒子投影随相机更新，ID 保持不变。没有选中时不绘制标记。

包围盒标记作为最后的 UI overlay，放在 TAA/色调映射之后，且不得写入 TAA history。它允许透视遮挡显示是调试标记的产品规则，与拾取仅选最前方有效表面的规则相互独立。

后续如需真实表面高亮，可新增 selected cluster/mesh mask pass，采用 alpha 与深度一致的可见性判断，在 resolve 后只叠加显示颜色。当前 `resolve.hlsl` 同时输出 color/history，不能简单在两者共同的 `result` 上加高亮，否则会污染 history。此增强不作为首版验收条件。

## 10. 预计代码落点

| 文件 | 拟议改动 |
|---|---|
| `Src/Include/SceneSelection.h`（新增） | Selection、请求版本、cluster→mesh 映射与校验，后端无关 |
| `Src/Include/SceneClickTracker.h`（新增） | 单击/拖拽状态机，坐标转换辅助逻辑 |
| `Src/Window.cpp` | Vulkan/DX12 输入接入；处理贴花优先级、取消和右键拖拽 |
| `Src/Include/SceneInput.h` | 尽量复用既有路由；若扩展，保持工具窗隔离与释放清理 |
| `Src/Include/GpuScene.h`、`Src/GpuScene.cpp` | 初始化映射、拾取资源/PSO、record hook、fence 后消费、resize/销毁、替换旧悬停 picker |
| `Src/DX12/DX12GpuScene.h/.cpp` | 同等选择语义、PSO/root signature、资源状态与 readback |
| `Src/DX12/DX12Setup.h/.cpp` | 必要时提供提交 fence value 和完成查询接口 |
| `Src/Include/EditorUi.h` 或共享 UI helper | 两后端共用 Selection 展示和盒子标记，后端负责资源而非 UI 逻辑 |
| `shaders/picking.hlsl`（新增） | 最小 VS/PS、ID 输出与三类材质判定；共享既有结构/后端宏 |
| `shaders/compile_shaders.bat`、`compile_shaders_dx12.bat` | 新 shader 变体构建，保持 shader 来源与构建产物一致 |
| `Tests/SceneInputTests.cpp`、新增 selection/click 测试、`Tests/CMakeLists.txt` | 输入回归、映射校验、代际与坐标测试 |

不直接改生成的 `.vcxproj`；从项目 CMake/现有构建流程维护源文件。具体资源封装拆分、descriptor binding 编号、日志字段和 helper 命名留给实现者，但不能改变本设计的身份、交互、透明与异步安全契约。

## 11. 实施顺序

1. **共享逻辑**：建立并验证归属映射；实现点击状态机、坐标换算、请求版本和 selection；先通过无需 GPU 的测试。
2. **Vulkan 拾取**：接入 opaque/alpha-mask/transparent ID Pass、资源和异步读回，替换旧悬停查询。
3. **完整交互反馈**：信息面板、Clear、透明策略开关、盒子标记、贴花/UI/拖拽冲突处理。
4. **DX12 对齐**：接入 ID pass、间接 drawSlot、readback footprint 和 fence 完成追踪。
5. **整体验收**：覆盖 resize、多帧并行、纹理流送、TAA 和快速连续点击，记录性能与已知限制。

单独完成 Vulkan 可以作为阶段交付，不能宣称两个后端都已支持。

## 12. 验证与验收

### CPU 自动化

- 映射：正常区间、共享材质但不同 mesh、空 mesh、范围溢出、重叠、孤立 cluster。
- 输入：微动单击、越阈值拖拽、拖回原位、非左键、工具窗点击、失焦、UI 捕获、跨窗释放、贴花模式。
- 坐标：100%/150%/200% DPI、非零 viewport 原点、四角和右下半开边界、零尺寸。
- 时序：A→B 连点且回读乱序，仅 B 可更新；Pending 后 Esc、resize、场景重载/模式切换，旧结果不恢复选择。

### GPU 与人工验收

| 场景 | 预期 |
|---|---|
| 一个 mesh 内两个可辨认 cluster | 两次命中 cluster 不同，mesh 相同 |
| 两个 mesh 共用同一材质 | mesh ID 仍不同 |
| 两层遮挡几何及 AABB 空隙 | 命中真实最前表面；空隙不能由 AABB 误选 |
| alpha-mask 叶片/栅栏 | 实体部分选前景，透明孔洞选后景或清空 |
| 半透明玻璃 | 默认命中达到阈值的最近玻璃；关闭透明参与后选后景 |
| ID 为 0、最后一个合法 ID、背景 | 均正确区分，背景 sentinel 不冲突 |
| 相机移动、culling 顺序改变 | 选中 ID 不因 draw slot 改变而漂移 |
| TAA 开/关和高 DPI | 目标内部一致；边界按点击提交帧的采样语义验收 |
| 连点、resize、最小化、重新加载 | 无旧结果复活、越界、资源提前释放或死锁 |
| Editor 脱离主窗口/覆盖场景 | UI 操作不选场景，标记只画在场景 |
| Vulkan validation / D3D12 debug layer | 无新增同步、布局、格式、descriptor 或 readback 警告 |

使用小型确定性场景验证 ID 与预先已知的 cluster/mesh 对应关系；两后端分别保存结果，不能只看画面“像是选对了”。首次测试关闭 TAA，选三角形内部稳定像素，之后再加边界与 jitter 测试。

### 性能指标

- 无点击且未选择：不执行 picking pass、读回拷贝或全 mesh 射线扫描。
- 单击：最多提交一次新增 picking pass、一次单像素拷贝；每帧未提交请求合并为最后一次。
- 不新增因点击产生的 GPU idle 等待；利用正常帧 fence 安全读取。
- 目标反馈延迟为约 1～3 个渲染帧，实际受 GPU 排队和资源槽复用影响，应记录提交/消费帧号而非承诺固定毫秒数。
- 记录点击帧 GPU 增量和附件内存；例如 R32_UINT + 32 位深度在 1080p 约 15.8 MiB/槽，3 槽约 47.5 MiB，尚未计分配对齐。资源可在首次开启选择时延迟创建。
- 若顶点成本或内存超预算，再评估裁剪投影的小目标、候选 cluster 过滤或常驻 ID MRT；这些优化需重新验证 alpha 导数、坐标与主渲染一致性。

## 13. 完成定义

在两个桌面光栅后端中，场景左键单击能稳定返回真实表面的 cluster ID 及唯一所属 mesh ID，选择可保留/清空、可检查、有明确可视反馈；不会与相机拖拽、贴花或 Editor 抢输入；不会读到尚未完成或已失效的 GPU 结果。上述测试和性能记录满足后，才能认为实现完成。

本次交付仅为这份设计文档；没有修改功能代码，也没有声称已通过运行时验证。
