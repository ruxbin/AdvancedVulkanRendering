# TAA 阴影抖动修复记录

## 问题描述

开启 TAA（Temporal Anti-Aliasing）后，远处墙面上的阴影出现逐帧闪烁抖动，静止场景下肉眼可见。

---

## 根因分析

问题由两个独立缺陷叠加造成。

### 缺陷 1：`invViewProjectionMatrix` 与深度缓冲不一致

**文件：** `Src/GpuScene.cpp`

G-buffer / 深度缓冲是用 **jittered 投影矩阵**渲染的，但上传到 GPU UBO 的 `invViewProjectionMatrix` 却是 **unjittered（clean）** 的逆矩阵。

`deferredlighting.hlsl` 的 `worldPositionForTexcoord()` 用这个 unjittered 逆矩阵从深度缓冲还原世界坐标：

```
worldPos = cleanInvViewProj × (ndc_center, depth_jittered, 1)
```

对**非垂直墙面**（倾斜面），深度值会随 jitter 的 XY 偏移逐帧微小变化（因为不同子像素位置采样到面上深度不同），而 NDC 的 XY 仍使用像素中心。两者不匹配，导致还原出的世界坐标每帧抖动约 ½ 像素世界空间量。

这个抖动的世界坐标被直接用于阴影贴图采样：

```
lsp = shadowMatrix × worldPos
```

当 ½ 像素世界偏移量接近或超过阴影贴图一个 texel 的覆盖范围时，阴影采样结果在 shadow / lit 之间来回翻转。

**为什么远处墙面更明显：**
- 远处（10–30 m）1 个屏幕像素覆盖的世界空间更大，½ 像素 jitter ≈ 0.016–0.05 m
- Cascade 2 阴影贴图（1024×1024，覆盖 ≥50 m）每 texel ≈ 0.05 m
- jitter 偏移量可达 texel 尺寸的 30–100%，5×5 PCF 无法完全平滑

---

### 缺陷 2：TAA 邻域 clamp 过于激进，放大了阴影闪烁

**文件：** `shaders/resolve.hlsl`

原实现用 3×3 邻域的 min/max 包围盒来 clamp history：

```hlsl
float3 minC = min(n0, n1, n2, ...);
float3 maxC = max(n0, n1, n2, ...);
historySample = clamp(historySample, minC, maxC);
```

对**均匀阴影区域**（9 个邻域像素同处于阴影中），`minC ≈ maxC ≈ 当前帧暗值`，包围盒极窄。上一帧处于亮态的 history 被强制 clamp 到暗值，temporal blending 完全失效，TAA 无法对阴影闪烁做任何时域平滑。

结果是：
- 第 N 帧像素在阴影 → history（亮）被 clamp → 输出暗
- 第 N+1 帧像素在亮光 → history（暗）被 clamp → 输出亮
- 阴影以完整幅度逐帧来回闪烁

---

## 修复方案

### Fix 1：上传 jittered 逆矩阵（`GpuScene.cpp`）

**位置：** `Src/GpuScene.cpp` ~line 3418

**修改前：**
```cpp
// Use clean (un-jittered) inverse VP for world position reconstruction
memcpy(data1, transpose(cleanInvViewProj).value_ptr(), (size_t)sizeof(mat4));
```

**修改后：**
```cpp
// Use jittered inverse VP so world position reconstruction is consistent
// with the depth buffer (which was rendered with jitteredProj). Using the
// clean invVP with jittered depth gives a per-frame-varying world position
// for angled surfaces, which propagates to the shadow UV and causes flicker.
mat4 jitteredInvViewProj = inverse(cleanView * jitteredProj);
memcpy(data1, transpose(jitteredInvViewProj).value_ptr(), (size_t)sizeof(mat4));
```

`worldPositionForTexcoord` 还原世界坐标时使用的矩阵现在与深度缓冲的渲染矩阵完全一致，消除了矩阵不匹配引入的每帧抖动。

> **注意：** 本项目 `mat4` 的 `A * B` 运算符实际计算 `B * A`（见 `Camera.cpp:74`），因此 `cleanView * jitteredProj` 在数学上等价于 `jitteredProj * cleanView`，取逆后得到正确的 jittered invVP。

---

### Fix 2：方差 clamp 替换 min/max clamp（`resolve.hlsl`）

**位置：** `shaders/resolve.hlsl` ~line 112

**修改前：**
```hlsl
// Compute neighborhood bounding box in tone-mapped space
float3 minC = min(min(min(n0, n1), min(n2, n3)), min(min(center, n5), min(n6, min(n7, n8))));
float3 maxC = max(max(max(n0, n1), max(n2, n3)), max(max(center, n5), max(n6, max(n7, n8))));
minC = ToneMapACES(minC * frameConstants.exposure);
maxC = ToneMapACES(maxC * frameConstants.exposure);
// ...
historySample = clamp(historySample, minC, maxC);
```

**修改后：**
```hlsl
// Compute 3x3 neighborhood mean and variance in tone-mapped space.
// Variance clamping avoids the tight min/max box that kills history in
// uniform shadow regions (where all 9 taps have nearly the same dark
// value and any history from a lit frame gets hard-clamped to shadow).
float3 taps[9] = {
    ToneMapACES(n0 * frameConstants.exposure),
    ToneMapACES(n1 * frameConstants.exposure),
    ToneMapACES(n2 * frameConstants.exposure),
    ToneMapACES(n3 * frameConstants.exposure),
    ToneMapACES(center * frameConstants.exposure),
    ToneMapACES(n5 * frameConstants.exposure),
    ToneMapACES(n6 * frameConstants.exposure),
    ToneMapACES(n7 * frameConstants.exposure),
    ToneMapACES(n8 * frameConstants.exposure)
};
float3 m1 = 0.0f, m2 = 0.0f;
[unroll] for (int i = 0; i < 9; i++) { m1 += taps[i]; m2 += taps[i] * taps[i]; }
m1 /= 9.0f;
m2 /= 9.0f;
float3 sigma = sqrt(max(m2 - m1 * m1, 0.0f));
float k = 1.25f;
// ...
// Clamp history to variance neighbourhood (mean ± k*sigma).
historySample = clamp(historySample, m1 - k * sigma, m1 + k * sigma);
```

方差 clamp 的行为：
- **均匀区域**（sigma ≈ 0）：clamp 范围约为 `mean ± 0`，但允许 history 在统计意义上的自然噪声范围内保留，不会强制置零
- **边缘区域**（sigma 较大）：clamp 范围自适应打开，仍可防止明显 ghost 伪影
- 参数 `k = 1.25` 提供比严格 min/max 更宽的容忍范围，可根据 ghost 与稳定性的取舍小幅调整

---

## 修复效果

| | 修复前 | 修复后 |
|---|---|---|
| 远处倾斜墙面阴影 | 每帧 shadow/lit 来回闪烁 | 阴影稳定 |
| 均匀阴影区域 TAA history | 被 clamp 丢弃，无时域平滑 | history 正常积累，时域稳定 |
| 阴影边缘 ghost | 受控（min/max 过于激进会间接产生边缘跳变） | 受控（方差 clamp 同样有效抑制 ghost） |

---

## 涉及文件

| 文件 | 改动 |
|---|---|
| `Src/GpuScene.cpp` | `invViewProjectionMatrix` 改为 jittered 逆矩阵 |
| `shaders/resolve.hlsl` | 邻域 clamp 从 min/max 改为方差 clamp |
| `shaders/resolve.ps.spv` | resolve.hlsl 重新编译产物 |
