# Cluster / Mesh Picking Implementation Plan & Progress

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task. Checkboxes and the ledger below are the durable progress record.

**Goal:** 本轮完成 Vulkan 光栅路径左键单击选择真实 cluster 与所属 mesh，并提供持续反馈。DX12 按用户 2026-10-30 指示暂缓。

**Architecture:** 共享输入、映射与选择状态；独立后端实现按需 R32_UINT ID pass 和帧同步后的单像素读回。沿用现有 shader 绑定和 indirect draw ID 契约。

**Tech Stack:** C++20、SDL2、ImGui、Vulkan、D3D12、HLSL/DXC、CMake/CTest。

**Spec:** [功能设计](../specs/2026-10-28-cluster-mesh-picking-design.md)

## Global Constraints

- Mesh 指 AAPLSubMesh，Cluster 指全局 AAPLMeshChunk 索引；不能把 draw slot 当 ID。
- 左键移动超过 4 个逻辑像素成为拖拽；ImGui、贴花、失焦优先处理。
- alpha-mask 阈值 0.1；透明采用 albedo alpha × material alpha >= 0.1，可关闭。
- 新请求/取消使旧结果失效；不为点击增加 GPU idle 等待。
- 保留原工作区既有改动；不修改资产布局；不把构建成功等同 GPU 验收。

## Review Focus

1. 任意按钮释放/工具窗事件取消候选时，不能误触选择或卡住相机。
2. 多帧并行与 resize 后不得读取旧 ID；Vulkan imageIndex 与 syncSlot 不混用。
3. 透明/alpha-mask 剔除、深度和纹理采样必须符合可见性规则。
4. 共享材质的不同 mesh、重叠/缺失区间不能错误归属。
5. TAA 之后绘制辅助标记，不污染 history；DX12 坐标需独立验证。

## Task 1: 共享选择逻辑与输入

**Files:** 新增 `Src/Include/SceneSelection.h`、`SceneClickTracker.h`、`Tests/SceneSelectionTests.cpp`、`SceneClickTrackerTests.cpp`；修改 `Tests/CMakeLists.txt`。

**Interfaces:** `scene_selection::Controller` 提供 BuildMapping、Request、TakePending、Complete、Cancel、Clear、状态查询；`scene_selection::ClickTracker` 提供 Press、Move、Release、Cancel；`ScreenPixel` 处理 DPI 与边界。

- [x] 写覆盖映射、请求失效、背景、错误与输入/坐标的测试；观察缺失功能导致失败。
- [x] 实现共享 header-only 逻辑，运行 Selection/Click/SceneInput 测试通过。

## Task 2: Shader 与 Vulkan

**Files:** `shaders/picking.hlsl`、shader 编译脚本、`Src/Include/GpuScene.h`、`Src/GpuScene.cpp`、新增 `Src/ScenePicking.cpp`、`Src/CMakeLists.txt`。

**Interfaces:** `RequestScenePick(float,float)` / `ClearSceneSelection()`；后端私有 create/record/consume/destroy；资源按 sync slot，主场景数据按 image index。

- [ ] 编译测试缺失 shader 入口；实现 opaque/mask/transparent ID shader 并编译 SPIR-V/DXIL。
- [ ] 实现 Vulkan 资源、PSO、indirect draw、copy/barrier、fence 后消费及 resize 清理。
- [ ] 构建 Vulkan 程序；GPU 能运行时使用 validation 与实际点击验证。

## Task 3: DX12

**Files:** `Src/DX12/DX12GpuScene.h/.cpp`、新增 `DX12ScenePicking.cpp`、必要的 `DX12Setup` 完成查询。

**Interfaces:** 与 Vulkan 同名公共选择 API，使用共享 Controller；沿用 drawSlot root constant。

- [ ] 实现三类 PSO、资源 footprint、copy 与安全帧槽消费。
- [ ] 构建启用 DX12 的程序并验证 shader/root signature 一致性。

## Task 4: 输入与反馈

**Files:** `Src/Window.cpp`、新增 `Src/Include/SceneSelectionUi.h`、后端 ImGui hook。

- [ ] 将 ClickTracker 接入两个输入循环；保留贴花优先、右键旋转、取消规则。
- [ ] 以共享 UI 展示 ID、统计、透明开关、Clear 和场景窗口双色包围盒。
- [ ] 对旧悬停 Mesh Picker 去除扫描行为；RT 模式禁用并取消请求。

## Task 5: 验证、审阅与交付

- [ ] 运行独立 CPU 测试集、双后端构建、shader 编译。
- [ ] 进行可行的 GPU 验证，记录未覆盖场景，不声称未经验证的行为已通过。
- [ ] 独立代码审阅，修复重要问题；更新本文结果与剩余限制。

## Ledger

### 2026-10-29 — 开始

- 已读取设计、现有输入路由、旧 AABB picker、两个后端的 indirect ID 和帧同步。
- 工作区分支 `svt`，不是 main/master；包含原有未提交测试及生成文件。
- Ruling: 在现有工作区执行，沿用本地依赖与未提交设计文件；不创建额外 checkout，不覆盖原有改动。用户已要求继续实现，无需重复审批设计。
- Ruling: 本文件同时作为实现计划和用户要求的进度文件，不依赖临时会话状态。未完成项保持未勾选。
- Pre-flight: 两个后端共用 Controller / ClickTracker / shader ID 契约；共享接口先实现与测试，后端不自行复制状态机。
- 尚未修改功能代码、未运行本轮验证。

### 2026-10-30 — 核对其他 agent 改动后继续

- 用户指示：DX12 暂时不用管。本轮仅实现/验证 Vulkan；已存在的 DX12 shader 编译条目保留，不视为 DX12 功能完成。
- 已核对：映射初始化、fence 后消费、resize picking 清理、专用 picking VS 已接入；旧悬停 AABB 查询已删除。
- 发现并清理未完成的诊断代码：固定 draw 代替 indirect、CPU 映射 GPU draw 数据、自动点击与 resize、临时关闭 validation。恢复三类正常 indirect draw 与 validation。
- 保留前一 agent 的 GBuffer resize 生命周期修复，待本轮 GPU 验证。
- 新增 SceneSelectionInputTests：先因缺失 SceneSelectionInput.h 编译失败，再实现真实事件路由（合法 release 不被 resetDrag 提前清除）。
- 已接入 Vulkan 左键单击/阈值拖拽、右键旋转、贴花优先、Esc、UI 捕获/跨窗取消。
- 已添加 Selection 面板与橙色 cluster / 青色 mesh 包围盒。UI 移到 resolve 后单独 swapchain pass，RT 模式取消拾取。
- 当前正在构建与验证；此时尚未宣称 GPU 验收通过。

### 2026-10-30 — 验证结果与中断点

- 4 个 Vulkan SPIR-V 变体编译成功（picking VS / opaque / alpha-mask / transparent）。
- `cmake --build build-picking-tests --config Debug` 成功；`ctest --test-dir build-picking-tests -C Debug --output-on-failure`：18/18 通过。日志 `.cache/picking-cpu-tests.log`。
- `cmake --build build-msvc --config Debug --target AdvancedVulkanRendering` 成功。保留已有 spdlog 弃用告警与 LNK4098，见 `.cache/picking-app-build.log`。
- GPU 程序启动并正常显示 Bistro 场景；validation 已恢复启用。日志 `.cache/picking-runtime-ui.log`。尚未完成有效点击、ID/包围盒与透明规则验收。
- 独立只读审阅发现 resize 事件到达后旧结果可能先于重建被接受。已添加事件阶段 cancelPick、失焦/最小化取消和回归用例。该用例先因缺少 cancelPick 字段失败，随后已实现修复；**最后这次修复尚未重新构建/运行测试**。
- 审阅指出原有 resize 尚有独立问题：occluder depth/Hi-Z、SAO 尺寸没有重建；decal depth descriptor 可能指向已销毁 view；swapchain image count 改变时 per-image buffers/descriptors 未完整扩容。当前主窗口原本不支持用户拖拽 resize。不能宣称 resize 整体已通过。
- 用户通过物理 Escape 停止 Computer Use；按中断信号停止界面操作。实际点击工具返回用户停止，不能视为点击成功。未继续操作或关闭验证窗口。

#### 下次继续顺序

1. 重新运行 SceneSelectionInputTests 和完整 CPU 测试；重建主程序，验证最新 cancelPick 修复。
2. 获得继续界面操作的指示后，验证左键选择不同 cluster/mesh、背景清空、Esc、拖拽、Editor 捕获、透明开关和 TAA 后包围盒。当前 UI 验证因 Escape 中断。
3. 核对 validation 日志并修复本次功能错误；原有 resize 问题明确标注，不把部分 GBuffer 修复当成整个 resize 已完成。
4. 清理最终 diff，完成进度勾选与交付。DX12 本轮暂缓。
2026-10-31：提交前重新构建 SceneSelectionInputTests 并通过；18 项 CPU 测试通过。实际 GPU 单击验收仍未完成。
