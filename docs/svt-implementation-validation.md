# Streaming Virtual Texture 实现与验证

日期：2026-10-17。对应设计：`docs/superpowers/specs/2026-10-16-streaming-virtual-texture-design.md`。

本次在上一位 agent 的 Cooker、文件格式和全驻留采样基础上，补齐桌面 Vulkan 的反馈驱动流式读取、上传、固定预算淘汰和安全复用。`--svt=on` 现在实际运行 streaming 模式。实现、测试与说明文档随本次 SVT 提交保存；生成的纹理数据与构建产物不纳入版本库。

## 启动

在工程根目录执行，已有 `svt/manifest.json` 可直接启动：

```powershell
.\Bin\AdvancedVulkanRendering.exe --svt=on
```

Editor 的 SVT 面板可以暂停 tile 读取、关闭反馈、调节 mip bias，并选择 Shaded / Requested mip / Resident mip / Tile grid / Physical slot / Fallback depth。调试颜色以 base-color 资产为依据，未绑定该资产的材质显示灰色。面板展示各 pool 的 resident/retiring/free、实际 atlas/tail 分配、页表、请求、上传、读取和反馈统计。

默认 atlas 总预算 128 MiB；可在加载场景时指定：

```powershell
.\Bin\AdvancedVulkanRendering.exe --svt=on --svt-atlas-mib=4
```

合法范围为 2–4096 MiB；各格式 pool 共享该总预算，按 Vulkan 实际分配大小计账。tail 上限 32 MiB，上传上限每帧 32 页且不超过 4 MiB。预算过小或能力不足时，初始化会记录原因并回退 legacy。改变 pool 大小需要重新启动。

首次使用或原始纹理内容改变后重新 cook：

```powershell
cmake --build build-msvc --config Debug --target SvtCooker --parallel 4
.\Bin\SvtCooker.exe bistro .\bistro.dxt.bin .\svt
.\Bin\SvtCooker.exe diag .\build-svt-tests\diagnostic.svt 1024 512
```

主程序构建：`cmake --build build-msvc --config Debug --target AdvancedVulkanRendering --parallel 4`。使用工程已有的 CMake / Vulkan SDK / SDL / spdlog 配置；shader 编译脚本已包含两种 SVT fragment 变体，修改 shader 后须重新编译。

## 已补齐的行为

- GPU 反馈去重、合成父 mip 请求、粗页优先、读取失败退避；Reading 预留和 Ready payload 共同受 CPU 字节预算约束。
- 两个 I/O worker 按 tile 读取和校验；排队、正在读取及未消费结果共同占用 outstanding 配额。重复请求、过期 generation、取消和 shutdown 均受控。
- 每个 sync slot 独立 staging、反馈、readback、UBO 和页表快照；只在对应 fence 完成后写 host 数据，索引不依赖 swapchain imageIndex。
- 图形队列中先复制 tile、做 image barrier，再采样；成功提交后才发布 CPU resident 状态。SVT 每帧路径不调用 queue/device idle，也不在渲染线程读取 tile 文件。
- 淘汰先撤销 authoritative 页表映射，物理页进入 Retiring；最后引用它的提交 serial 完成后才能复用。未来提交始终从最新页表生成快照。
- 原始 `.dxt.bin` 纹理 blob 改为独立文件句柄的范围读取，兼容 legacy mip streamer 和 PBRT 导出；SVT 启用时不再常驻约 686 MiB 的原始纹理 blob。
- manifest admission 检查规范化路径、hash 冲突、尺寸、格式、mip 和 manifest/SVT 内容 hash 一致性；不兼容条目回退 legacy。
- 修复反馈 UBO 跨 slot 覆写、透明路径误用 SVT，以及 swapchain slot 数变化后资源重建的接入问题。
- 验证过程中发现两条与 SVT 无关、on/off 均可复现的深度 LOAD 同步错误；补齐 deferred/forward 的深度 STORE 与读取依赖，最终同步验证通过。

## 验证记录

| 验证 | 结果 |
|---|---|
| MSVC Debug 主程序（ENABLE_DX12=ON） | 构建通过 |
| SVT base / forward fragment shader | DXC SPIR-V 编译通过 |
| CPU CTest | 15/15 target 通过；SvtSystem 内含 19 个用例 |
| PBRT exporter | 35/35 通过；新增 resident/lazy blob 导出逐字节一致性 |
| Cooker 长方形诊断资产 | 1024×512，42 tiles，8 tail mips；读取信息正确 |
| 默认预算冷启动 | 240 帧；591 assets；120.6 MiB atlas、6.6 MiB tails；494 页上传；无常规 validation 错误 |
| 最终 4 MiB 旋转测试 + synchronization validation | 720 帧；936 页上传，760 次淘汰；退出码 0，0 validation 错误、0 I/O 失败、0 feedback overflow |
| 最终 SVT off + synchronization validation | 120 帧；退出码 0，0 validation 错误 |

小缓存镜头持续旋转，结束时仍有 1257 个有界 pending 请求，176 个 resident 页；该测试证明过载时持续淘汰/上传而非工作集完全收敛。缺页由父 mip / pinned tail 提供内容。

日志位于 `build-svt-tests/app-build.log`、`pbrt-tests.log`、`final-on.log`、`final-off.log`；最终 on/off 使用同一份二进制。应用原有的相机矩阵和 index-type error 级别日志仍存在，上表统计的是 Vulkan Validation Error / VUID / SYNC-HAZARD，不把普通日志级别等同于校验结果。

复现同步验证：

```powershell
$env:VK_LAYER_ENABLES='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT'
.\Bin\AdvancedVulkanRendering.exe --svt=on --svt-atlas-mib=4 --svt-validation-tour --vlm-validation-frames 720
.\Bin\AdvancedVulkanRendering.exe --svt=off --vlm-validation-frames 120
ctest --test-dir build-svt-tests -C Debug --output-on-failure
python -m unittest discover -s Tests -p test_pbrt_exporter.py
```

`--svt-validation-tour` 每绘制帧旋转 0.5 度；与现有 `--vlm-validation-frames` 一起使用可自动退出。该 frame limit 只复用主循环的有界运行机制，不触发 VLM bake。

## 实现取舍和验证边界

1. 页表使用现有 coherent host-visible 的 per-slot 全量快照，未引入 GPU 增量更新；实现简单，但独显页表读取和大页表 CPU 复制仍需性能测量。沿用实际 set2 bindings 0–9。
2. alpha-mask、透明、阴影、RT、VLM、DX12 继续使用 legacy 纹理。legacy GPU 纹理仍可能与 SVT atlas/tail 双份驻留，120.6 MiB atlas 不能解释为整个渲染器的 GPU 内存占用或节省量。
3. admission 不在启动时重新解码并哈希原始纹理全部像素；原始文件内容修改后须重新 cook，即便路径和尺寸未变。
4. 当前提供手动 mip bias。自动压力 bias、完整命中比例/延迟分位数、atlas 图像查看及普通纹理的像素级 GPU 对照仍待补充；没有声称达到设计中的 CPU/GPU p95 性能目标。
5. 已验证主路径的运行与同步；尚未执行 1/2/3 slot 强制配置、100 次 resize/重载、慢 GPU/I/O 注入、所有 debug view 的视觉比对，以及 RT/VLM/DX12 的完整运行矩阵。DX12 证据目前是主工程同时编译通过。
6. GPU 资产生命周期固定于场景加载/销毁；CPU generation/cancel 已测试，不等于已经提供运行时任意资产热卸载 UI。
7. 独立 reviewer agent 因账户额度失败而未完成；本次做了作者代码复查和上述实际验证，不宣称独立代码审查通过。
