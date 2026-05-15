# ScreenSpaceDecal 修复记录

本次集中修复 `ScreenSpaceDecal` 分支合并到 `raytracing` 分支后 review 出的 13 个问题。
分严重级整理修复内容、原因和验证方式。

涉及文件：
- `Shaders/decal.hlsl`
- `Shaders/commonstruct.hlsl`
- `Src/GpuScene.cpp`
- `Src/Include/GpuScene.h`

---

## 🔴 严重 / 功能性

### Fix #1 — 多 decal 全部使用最后一个 decal 的数据

**Bug**：原 `drawDecals` 在循环里每个 decal 都把 transform 拷到**同一个** uniform buffer，然后录制 draw 命令。但 `vkCmdDrawIndexed` 只是录制不执行，等 GPU 跑完整个 command buffer 时，buffer 里只剩最后一个 decal 的数据 → 8 个 decal 全部叠在一起渲染同一份。

```cpp
// BEFORE（GpuScene.cpp）
for (size_t d = 0; d < _decals.size(); ++d) {
    vkMapMemory(...); memcpy(data, &_decals[d], ...); vkUnmapMemory(...);
    vkCmdDrawIndexed(commandBuffer, _decalIndexCount, 1, 0, 0, 0);  // 全部用最后一个
}
```

**修法**（push constant + SSBO 方案）：
1. uniform buffer 改 SSBO，大小从 `sizeof(DecalData)` 改 `sizeof(DecalData) * MAX_DECALS`
2. shader 把 `cbuffer DecalData` 改成 `StructuredBuffer<DecalData> decalArray`
3. 加 `[[vk::push_constant]] uint decalIndex` 让 shader 知道这次绘制是哪个 decal
4. CPU 端 `drawDecals` 一次性 memcpy 整个数组到 SSBO，循环里只 `vkCmdPushConstants` 更新 index

```cpp
// AFTER（GpuScene.cpp）
// 一次性上传整个数组
vkMapMemory(...); memcpy(data, _decals.data(), sizeof(DecalData) * decalCount); vkUnmapMemory(...);

for (uint32_t d = 0; d < decalCount; ++d) {
    vkCmdPushConstants(cmd, layout, STAGES, 0, sizeof(uint32_t), &d);
    vkCmdDrawIndexed(cmd, _decalIndexCount, 1, 0, 0, 0);
}
```

```hlsl
// AFTER（decal.hlsl）
[[vk::binding(2, 1)]] StructuredBuffer<DecalData> decalArray;
struct DecalPushConsts { uint decalIndex; };
[[vk::push_constant]] DecalPushConsts pc;

VSOutput DecalVS(VSInput input) {
    DecalData d = decalArray[pc.decalIndex];
    ...
}
```

**附带**：descriptor type 从 `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` 改 `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`，pipeline layout 加 `VkPushConstantRange{ size = sizeof(uint32_t) }`，descriptor pool 类型同步调整。

---

### Fix #2 — uniform buffer 只够一个 decal

**Bug**：`bufInfo.size = sizeof(DecalData)`，但 `MAX_DECALS = 8`。即使没有 #1 的 race，也只能放 1 个 decal。

**修法**：随 #1 一起修。新 SSBO 大小 = `sizeof(DecalData) * MAX_DECALS = 160 * 8 = 1280` 字节。
SSBO 没有 `minUniformBufferOffsetAlignment`（256B）那么严格的对齐要求，自然紧密排列即可。

---

### Fix #3 — `getWorldPosFromDepth` 的 oldLayout 假设错误

**Bug**：函数硬编码 `oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL`，但这个函数从鼠标回调中调用（帧间）。每帧渲染最终把 depth 留在 `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`（drawDecals 或 deferred 路径转换的），不是 `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`。点击放置 decal 时即触发 layout mismatch validation error。

**修法**：用 `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL` 作为 oldLayout 和恢复时的 newLayout，保持帧间 layout 一致性。

```cpp
const VkImageLayout depthCurrentLayout =
    VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
// barrier oldLayout / newLayout 都用 depthCurrentLayout
```

---

### Fix #4 — `vkQueueWaitIdle` 全管线 stall

**Bug**：原 `getWorldPosFromDepth` 通过 `device.endSingleTimeCommands()` 提交，内部用 `vkQueueWaitIdle(graphicsQueue)` —— 鼠标点一下整个 GPU 等到当前帧完成才能继续。

**修法**：手写一个轻量提交：分配一次性 command buffer + 创建临时 fence，用 `vkWaitForFences` 只等这个 submit。其他帧可以继续在 queue 里推进。

```cpp
VkFence fence;
vkCreateFence(...);
vkQueueSubmit(graphicsQueue, 1, &submitInfo, fence);
vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
vkDestroyFence(...);
```

---

## 🟡 中等

### Fix #5 — decal 路径的 depth transition 无条件执行

**Bug**：原代码无论 `_decals` 是否为空都做 `transitionImageLayout`，浪费 barrier 不说，`drawDecals` 内部又判 `_decals.empty() return`，外层做了 transition 但 render pass 没跑。

**修法**：把 transition 移进 `drawDecals` 内部、放在 early-return 之后，让它和实际绘制绑定。下游 deferred 路径做了对应判断：`_decals` 为空时它自己负责把 depth 转到 read-only。

```cpp
// drawDecals 内部
if (_decalPipeline == VK_NULL_HANDLE || _decals.empty())
    return;
transitionImageLayout(... DEPTH_STENCIL_ATTACHMENT_OPTIMAL → DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL ...);

// 调用方
drawDecals(commandBuffer);

// 下游 useClusterLighting=false 分支
if (_decals.empty()) {
    transitionImageLayout(... DEPTH_STENCIL → DEPTH_READ_ONLY_STENCIL ...);
}
```

---

### Fix #6 — depth descriptor 的 imageLayout 固定为 `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`

**状态**：保持现状，但已通过 #3、#5 保证整条流水线一致使 depth 维持在该 layout。
未来如果新 pass 在 decal 之前/之后切换 depth 到其他 layout，这里需要同步更新。

---

### Fix #7 — `loadDecalTexture` use-after-free 风险

**Bug**：函数开头直接 destroy 旧 image / view / memory，但 in-flight 的 frame command buffer 可能还在引用它们。

**修法**：开头加 `vkDeviceWaitIdle(device.getLogicalDevice())`。这是切换 decal 纹理的低频操作（用户按键触发），全管线 stall 是可以接受的代价。

```cpp
void GpuScene::loadDecalTexture(const char *path) {
  vkDeviceWaitIdle(device.getLogicalDevice());  // ← Fix #7
  if (_decalTextureView != VK_NULL_HANDLE) { vkDestroyImageView(...); }
  ...
}
```

---

### Fix #8 — descriptor set update 的并发风险

**Bug**：`loadDecalTexture` 末尾遍历 framesInFlight 重写 binding 3 的 descriptor，没用 `UPDATE_AFTER_BIND` 而 GPU 可能还在用旧 descriptor。

**修法**：随 Fix #7 一起 —— 入口 `vkDeviceWaitIdle` 后 GPU 已经空闲，descriptor write 安全。

---

## 🟢 优化 / 健壮性

### Fix #9 — depth test 行为在 decal volume 跨 receiver 时有 artifact

**Bug**：原 pipeline 用 `depthTestEnable = TRUE` + `depthCompareOp = LESS_OR_EQUAL`（reverse-Z 项目里这条搭配有点 hack），靠 cube back face 与 receiver depth 的相对关系来粗筛。当 decal volume 部分穿过 receiver 表面时，back face 在某些像素被深度测试拒掉，产生条带 artifact。

**修法**：彻底关闭 depth test，仅靠 shader 里的 bbox discard（`if (any(abs(localPos) > 1.0)) discard;`）做范围判断，完全准确。Depth write 本来就是关的。

```cpp
depthStencil.depthTestEnable = VK_FALSE;
depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
```

---

### Fix #10 — render pass 绑定 4 个 GBuffer attachment 但只写 SV_Target0

**Bug**：原 render pass / framebuffer / pipeline 都按 4 个 color attachment + 1 个 depth 配置，但 shader 只输出 `SV_Target0`，其余 3 个 attachment 用 `colorWriteMask = 0` 屏蔽。功能正确但浪费验证开销和可读性。

**修法**：render pass 改 1 color + 1 depth，framebuffer 跟着改，pipeline blend state 只配 1 个 attachment。

```cpp
// AFTER
VkAttachmentDescription colorAttachment{};   // 单个 albedo
VkAttachmentDescription depthAttachment{};   // sampled depth, read-only
VkAttachmentDescription allAttachments[2] = {colorAttachment, depthAttachment};
// framebuffer 里只放 _gbuffersView[0][f] + 深度 view
// pipeline.colorBlend.attachmentCount = 1
```

---

### Fix #11 — 移除未使用的成员

`_activeDecalCount` 在 `GpuScene.h` 里声明从未被引用，删掉。

```diff
- uint32_t _activeDecalCount = 0;
```

---

### Fix #12 — `_Textures[]` bindless 访问需要 `NonUniformResourceIndex`?

**结论**：不需要。`decal.hlsl` 里的 `decalAlbedoTex` 是单个 `Texture2D<float4>`（不是数组），不存在 subgroup 内非一致索引问题。同 `depthTex` 也是单 texture。

参见 `docs/NonUniformResourceIndex-bindless.md` 已记录的判断标准。

---

### Fix #13 — `worldPositionForTexcoord` 的 Y-flip 约定加注释

**Bug**：函数里 `ndc.xy = texCoord * 2 - 1` 没有显式 Y-flip，但 Vulkan NDC Y 朝下，看起来"应该"翻转。实际上是因为 `projectionMatrix` 内部已经包含负 Y scale，invViewProjectionMatrix 时会自动撤销，所以 shader 里不需要再手动翻 Y。

**修法**：给注释解释为什么 `//ndc.y *= -1` 是注释掉的，提醒未来如果换投影矩阵约定要注意。

---

## 验证

- shader 重新编译：`decal.vs.spv` / `decal.ps.spv` 已用 `dxc -T vs_6_0/-T ps_6_0` 重新生成（VulkanSDK 1.3.296.0）
- bat 文件已包含 decal 编译命令
- C++ 侧的 Type 检查 / clang static analysis 没有新增引用（除了和当前问题无关的 `imgui.h` / `STL1000` 既有警告）

## 测试建议

1. **多 decal**：放置 2 个以上 decal，确认每个都在自己位置渲染（修 #1、#2 验证）
2. **空 decal**：不放任何 decal 跑一帧，看是否 validation 干净（修 #5 验证）
3. **decal 跨墙**：把 decal 放在墙边缘让 cube 跨过墙面，看是否仍然完整无条带（修 #9 验证）
4. **切换纹理**：连按 cycle 键多次，看是否还会触发 use-after-free 崩溃（修 #7、#8 验证）
5. **鼠标点击**：在 RT 模式下点击放置 decal，看 layout validation 是否消失（修 #3 验证）

## 仍存在的工程债

- `useClusterLighting=true` 分支里那条 `barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL`（line ~3536，`_STENCIL_ATTACHMENT_OPTIMAL` 才对）—— 这是 ScreenSpaceDecal 之前就有的问题，不在本次 review 范围。
- decal "选中"逻辑用射线和 decal 中心点距离做命中判断，准确度不高，应该改用 OBB 测试。属于交互 UX 问题。
