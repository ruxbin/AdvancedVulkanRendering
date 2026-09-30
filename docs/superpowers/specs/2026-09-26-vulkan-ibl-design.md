# Vulkan IBL(基于图像的照明)设计

日期:2026-09-26
状态:已实现；2026-09-30 完成 Apple KTX 数值对齐，验证记录见下文
范围:**仅 Vulkan 后端**;deferred lighting pass + forward transparent pass。DX12 后端与 RT 路径不在本次范围内,但不得被本改动破坏。

## 背景与目标

本项目是 Apple《Modern Rendering with Metal》(D:\ModernRenderingWithMetal)的 Vulkan/DX12 移植。Metal 版的 IBL 由以下部分组成:

- 环境 cube(`san_giuseppe_bridge_4k_ibl.ktx`,256×256,9 mip,RGBA8,RGBM 打包 `6.0*rgb*a` 解码,mip 链为离线 GGX 预过滤,`lod = roughness * 8.0`)
- DFG LUT(`DFGLUT.ktx`,256×256 RG16F,按 `(NdotV, roughness)` 采样,`specColor = F0*lut.x + lut.y`)
- 漫反射:硬编码 9 个 SH 系数 `evaluateShCoefficients(-N) * albedo`(AAPLLightingCommon.h:64-90)
- 缩放:`iblScale`(整体)、`iblSpecularScale`(仅高光,默认 4.0);天空仍是纯色 `skyColor`,与 IBL 解耦

**本次决策(用户已确认):**

1. IBL 资产**运行时生成**,不加载 Apple 烘焙的 KTX:从 `.hdr` equirect 全景图出发,用 compute shader 生成预过滤 cube 与 DFG LUT,CPU 计算 SH9 系数。
2. 第一版同时覆盖 **deferred + forward** 两个 pass。

当前 port 现状(实现前):

- 无 cube 纹理、无 KTX/HDR 加载;`stb_image.h` 已 vendored 且含 `stbi_loadf`(HDR)。
- deferred set 1 binding 0–17 全部占用(GpuScene.cpp:1217-1348),下一可用 18+。
- forward pass 使用 `applSetLayout`(set 1:0=materials SSBO,1=sampler,2=bindless textures[],3=meshChunks,4=chunkIndex;GpuScene.cpp:1661-1739),binding 5+ 空闲。
- deferred PS 在 `deferredlighting.hlsl:141`:`result = lightingShader(...) * shadow * ao`;Metal 的顺序是 `(sun*shadow + IBL) * ao`。
- forward PS(`drawcluster.hlsl` 的 `RenderSceneForwardPS` / `RenderSceneForwardPSIndirect`,行 290/374)只调 `lightingShader(...)`,无 AO/shadow。
- `FrameConstants`(Common.h:72-103 / commonstruct.hlsl:74-94)正好 128 字节打满,有 static_assert 钉住偏移。
- 一次性命令辅助已存在:`device.beginSingleTimeCommands()` / `endSingleTimeCommands()`(GpuScene.cpp:5190、5669 等处使用)。
- ImGui 调试窗口在 GpuScene.cpp:8556 起("Culling Stats")。

## 总体架构

```
textures/san_giuseppe_bridge_2k.hdr (新增资产, Polyhaven CC0)
   │  stbi_loadf → float4 像素 (CPU)
   ├─► Src/SphericalHarmonics.cpp: CPU 积分 SH9(纯函数)
   │        └─► 9×vec4 小 UBO(一次性创建, host visible)
   └─► equirect 纹理 RGBA32F (GPU, SAMPLED)
            │
            ▼  Src/IBLGenerator.cpp(一次性, init 阶段)
   CS1 ibl_equirect_to_cube.cs   → envCube mip0
   CS2 ibl_prefilter_specular.cs → envCube mip1..8(每 mip 一次 dispatch, roughness=mip/8)
   CS1 再次执行                → 将线性 mip0 转换为最终 Apple shader 显示值
   CS3 ibl_dfg_lut.cs            → dfgLut 256×256 RG16F
            │
            ▼  全部 transition → SHADER_READ_ONLY_OPTIMAL
   deferred PS:set1 +binding 18=dfgLut, 19=envCube, 20=iblSampler, 21=SH-UBO
   forward  PS:set1 +binding  5=dfgLut,  6=envCube,  7=iblSampler,  8=SH-UBO
```

### 关键资源

| 资源 | 格式 | 尺寸 | 说明 |
|---|---|---|---|
| equirect | `R32G32B32A32_SFLOAT` | 2048×1024, 1 mip | stb 直接上传,staging 后可释放(保留也无妨) |
| envCube | `R16G16B16A16_SFLOAT` | 256×256, 6 层, **9 mip** | `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`;usage = SAMPLED \| STORAGE \| TRANSFER_DST \| TRANSFER_SRC;生成期经 **2D-array storage view** 写入,采样用 cube view。最终存储 `0.375*sqrt(L)` 的 Apple shader 显示值，无 RGBM 打包；过滤输入为线性 HDR |
| dfgLut | `R16G16_SFLOAT` | 256×256, 1 mip | usage = SAMPLED \| STORAGE |
| SH UBO | uniform buffer | 9×vec4 = 144B | host-visible+coherent,init 写一次 |
| iblSampler | sampler | — | mag/min/mip 全部 LINEAR,address CLAMP_TO_EDGE |

### 为什么漫反射用 CPU SH9 而不是 irradiance cube

与 Metal shader 同形式(`evaluateShCoefficients` 可逐字移植),少一个 GPU pass、少一个 binding、少一张纹理;代价是高频环境下轻微 banding——Metal 原版即如此,行为对齐。SH 从同一张 equirect 积分,与 specular cube 天然同朝向约定。

## 组件设计

### 4.1 新文件

**`Src/Include/SphericalHarmonics.h` / `Src/SphericalHarmonics.cpp`** — 纯 CPU,无 Vulkan 依赖,可单测。

```cpp
// 从 equirect 浮点像素积分出 9 个"浓缩形式"SH 系数(与 Metal
// AAPLLightingCommon.h evaluateShCoefficients 的硬编码常数同形式:
// 基函数 1, y, z, x, yx, yz, 3z^2-1, zx, x^2-y^2,归一化常数已折入)。
// 积分权重为立体角 dω = sin(θ) dθ dφ;结果即辐照度 SH。
struct SH9 { vec3 c[9]; };
SH9 ComputeSH9FromEquirect(const float* rgba, int width, int height); // 标准物理辐照度
SH9 ComputeMetalSH9FromEquirect(const float* rgba, int width, int height); // 运行时使用
```

- equirect 方向映射必须与 CS1 的 cube 方向映射一致(见 4.2)。
- 验证锚点:对 san_giuseppe_bridge 环境,结果应与 Metal 硬编码常数量级一致(x/z 相关项符号取决于朝向约定,以本实现内部自洽为准)。

2026-09-30 数值勘误：标准物理系数不能直接替代示例的硬编码系数。兼容 API 限制输入到 `[0,256]`，除以 `π*Y_i` 并翻转 Z 相关项，按 `+N` 求值；标准 API 保持物理含义。详见验证记录中的推导与解析回归测试。

**`Src/Include/IBLGenerator.h` / `Src/IBLGenerator.cpp`** — 持有生成管线和输出资源。

```cpp
class IBLGenerator {
public:
  void init(VulkanDevice& device, const std::filesystem::path& rootPath); // 编译 3 个 CS pipeline
  // 输入:equirect imageView+尺寸;一次性命令内完成全部生成与 layout 转换。
  // 输出(所有权转移给 GpuScene):envCube image/view(cube)/ sampler 无关、dfgLut image/view、SH9。
  struct Output {
    VkImage envCubeImage; VkDeviceMemory envCubeMemory; VkImageView envCubeView; // cube
    VkImage dfgImage;     VkDeviceMemory dfgMemory;     VkImageView dfgView;
    SH9 sh;
  };
  Output generate(VulkanDevice& device, VkImageView equirectView, uint32_t eqWidth, uint32_t eqHeight);
  void shutdown();
};
```

- 生成时序(单个 `beginSingleTimeCommands` 内):
  1. envCube 全 mip 转 `GENERAL`(storage 写);equirect 已处 SHADER_READ。
  2. CS1:dispatch (256/8, 256/8, 6),写 mip0 的 2D-array storage view。
  3. barrier:mip0 STORAGE_WRITE → SHADER_READ(image 保持 GENERAL,配 access/pipeline stage barrier;或将 mip0 单独转 SHADER_READ_ONLY,mip1-8 留 GENERAL)。
  4. CS2 × 8:mip = 1..8,push constant 传 `roughness = mip/8.0` 与输出尺寸,读线性 mip0,写对应 mip 的最终显示值，各 dispatch 输入不依赖前一个输出。完成后添加 compute 读/写→写 barrier，再执行 CS1 转换 mip0。
  5. CS3:dispatch (256/8, 256/8, 1),写 dfgLut(GENERAL)。
  6. envCube 全 mip + dfgLut → `SHADER_READ_ONLY_OPTIMAL`。
- CS pipeline 各自的 set layout 为生成专用(equirect SRV + cube UAV;cube SRV + cube UAV + push constants;LUT UAV),与主渲染管线无关。

### 4.2 方向约定(Vulkan cube 规则)

CS1 对每个 face/uv 生成世界方向 `dir`,再用 `u = atan2(-dir.x, dir.z)/(2π)+0.5`、`v = acos(dir.y)/π` 采样 equirect。face→轴映射为 `+X,-X,+Y,-Y,+Z,-Z` 层序。SH 投影使用逆映射 `dir=(-sinθ sinφ, cosθ, sinθ cosφ)`，其中 `φ=(u-0.5)*2π`、`θ=v*π`。运行时以 `+N` 求值本项目 SH；Apple 的硬编码系数以 `-N` 求值，不能直接比较两套系数的符号。

2026-09-29 勘误：原设计中的 `atan2(z,x)` 以及早期实现的 `atan2(x,z)` 均被上述映射替代。方向回归测试使用独立定义的 +X/+Z/-Z 半球环境。2026-09-30 进一步修正了 Cube、SH 和 DFG 的数值约定；DFG 采用相关 Smith，可见性 alpha=roughness、采样 NDF alpha=roughness²，属于参考资产兼容。对拍指标、已知限制和复现方式见 [KTX 对拍验证记录](../../ibl-ktx-validation.md)。

### 4.3 Shader 改动

**`shaders/ibl_common.hlsl`(新增)** — 被 deferred/forward 包含,后端中立:

```hlsl
// 与 Metal AAPLLightingCommon.h 同数学。
// SH 系数以值参数组传入(HLSL 不能把 cbuffer 当参数;各 pass 自己声明绑定,传全局数组进来)。
float3 evaluateShCoefficients(float3 n, float4 sh[9]);
float3 IBL(AAPLPixelSurfaceData surface,
           TextureCube envMap, Texture2D<float2> dfgLut, SamplerState samp,
           float4 sh[9], float3 viewDir, float scale, float specularScale);
// lod = roughness * 8.0;SampleLevel;lut uv = (NdotV, roughness);
// specColor = F0*lut.x + lut.y;return (diffuse + spec*specularScale) * scale;
```

**`shaders/deferredlighting.hlsl`**:

```hlsl
#ifndef DX12_BACKEND
VK_BINDING(18,1) Texture2D<float2> dfgLutTex REGISTER_SRV(18,1);
VK_BINDING(19,1) TextureCube envMap REGISTER_SRV(19,1);
VK_BINDING(20,1) SamplerState iblSampler REGISTER_SAMPLER(20,1);
VK_BINDING(21,1) cbuffer SHCoefficients REGISTER_CBV(21,1) { float4 shCoefs[9]; };
#endif
```

调用点(现 141 行)改为:

```hlsl
half3 result = lightingShader(surfaceData, depth, worldPosition, frameConstants, cameraParams) * shadow;
#ifndef DX12_BACKEND
if (frameConstants.iblScale > 0.0f)
    result += (half3)IBL(surfaceData, envMap, dfgLutTex, iblSampler, shCoefs, viewDir,
                         frameConstants.iblScale, frameConstants.iblSpecularScale);
#endif
result *= (half)ao;
```

(viewDir 需在 PS 内由 camera 位置与 worldPosition 现算,与 lightingShader 内部一致;sky 像素分支 `depth < 0.0001` 保持原样,IBL 只加在有几何的像素上——sky 分支直接覆盖 result,天然排除。)

**`shaders/drawcluster.hlsl`**(两个 forward PS 入口):

```hlsl
#ifndef DX12_BACKEND
VK_BINDING(5,1) Texture2D<float2> dfgLutTex REGISTER_SRV(5,1);
VK_BINDING(6,1) TextureCube envMap REGISTER_SRV(6,1);
VK_BINDING(7,1) SamplerState iblSampler REGISTER_SAMPLER(7,1);
VK_BINDING(8,1) cbuffer SHCoefficients REGISTER_CBV(8,1) { float4 shCoefs[9]; };
#endif
// RenderSceneForwardPS / RenderSceneForwardPSIndirect 中:
// res += IBL(...)(无 AO;base/shadow 等其它入口不引用,编译期消掉)
```

**`shaders/lighting.hlsl`**:不动(IBL 放独立新头文件,避免 lightingShader 签名涟漪到 point/spot/RT 等所有调用方)。

**`shaders/commonstruct.hlsl` + `Src/Include/Common.h`** — `FrameConstants` 末尾追加:

```cpp
float iblScale;          // offset 128
float iblSpecularScale;  // offset 132
float _padIbl0, _padIbl1;// → sizeof 144
```

同步更新两处 static_assert(sizeof==144、新增 offsetof 断言)。DX12 的 `DX12GpuScene.cpp` 初始化处给两个字段赋默认值(1.0/4.0;DX12 shader 不含 IBL 代码,行为不变)。

**`shaders/compile_shaders.bat`**:追加 3 个 CS 的编译行(与现有条目同风格,`dxc -spirv -T cs_6_2`)。`compile_shaders_dx12.bat` **不改**(ifndef 保证 DX12 编译产物不变)。

### 4.4 Vulkan C++ 改动(GpuScene.cpp)

1. **init 阶段**(纹理/场景加载完成后、首帧前):
   - `stbi_loadf` 加载 hdr → staging → equirect RGBA32F 纹理 + view(复用 createTexture 的上传模式,但格式/每像素字节数不同,写专用小函数);
   - `IBLGenerator::generate(...)` 得 envCube/dfgLut/SH;
   - 创建 SH UBO(host-visible+coherent,memcpy 一次);
   - 创建 iblSampler(三线性 + clamp)。
2. **deferred set layout**(GpuScene.cpp:1217-1348 区域):追加 binding 18(SAMPLED_IMAGE)、19(SAMPLED_IMAGE)、20(SAMPLER)、21(UNIFORM_BUFFER),全部 FRAGMENT 阶段;池尺寸:`SAMPLED_IMAGE 14→16*N`、`SAMPLER 4→5*N`、新增 `UNIFORM_BUFFER 1*N`。每个 frame-in-flight 的 deferred set 写 4 条新 descriptor(静态内容,init 写一次即可,但写操作放在现有 per-frame 写区域旁保持一致)。
3. **appl set layout**(GpuScene.cpp:1661-1739):追加 binding 5(SAMPLED_IMAGE)、6(SAMPLED_IMAGE)、7(SAMPLER)、8(UNIFORM_BUFFER);池扩容;appl 集合每个 frame 写 4 条。注意该 set 也被 base/G-buffer pass 共用——新增 binding 写入有效描述符后,base pass 不采样即无影响。
4. **DX12GpuScene.cpp**:仅两行新字段默认值,其余不动。
5. **ImGui**(GpuScene.cpp:8556 起的窗口):新增 "IBL" 分组,`SliderFloat("IBL Scale", 0..2)`、`SliderFloat("IBL Specular Scale", 0..8)`,写入 `_frameConstants.iblScale / iblSpecularScale`(默认 1.0 / 4.0)。

### 4.5 资产

- 下载 Polyhaven `san_giuseppe_bridge` 2K HDR(CC0,与 Metal 环境同源)→ `textures/san_giuseppe_bridge_2k.hdr`。若下载失败则提示用户手动放置,加载失败时优雅降级:`iblScale` 强制 0 并 log,不崩溃。

## 错误处理

- hdr 文件缺失/解析失败:跳过 IBL 生成,绑定 1×1 黑色 cube/LUT 占位(或 iblScale=0 + shader 早退),应用照常运行。
- SH 积分输入为空:同上。
- 生成 pass 任一 pipeline 创建失败:fatal 与现有 init 风格一致(log + abort)。

## 验证方案

1. **单元级**:`SphericalHarmonics` 纯函数测试(参照 Tests/ 现有 gtest 风格):
   - 恒定辐射环境(全 1.0)→ L00 ≈ 常数、其余系数 ≈ 0;
   - 对 san_giuseppe hdr,与 Metal 硬编码 9 常数量级对比(容差内,符号按约定说明)。
2. **数值级**:生成的 DFG LUT 与 Apple `DFGLUT.ktx`(开发期解析,不入库)逐像素对比，MAE < 0.003；Cube 各 mip 原始相对误差 < 5%，SH 求值原始相对误差 < 2%。自动检查使用 `Tests/validate_ibl_log.py`。
3. **渲染级**:编译运行 bistro 场景,ImGui 开关/拉条截屏对比;RenderDoc 抓帧确认:envCube 内容与 equirect 一致、9 mip 逐级变糊、binding 18-21 / 5-8 全部绑定。
4. **回归**:DX12 后端编译运行画面与改动前一致;现有 Vulkan 路径(TAA/scatter/RT 开关)无回归。

## 不做的事(YAGNI)

- DX12 后端的 IBL(后续单独移植;ifdef 已预留)。
- RT 路径(rt_lighting.hlsl)miss shader 换环境贴图天空。
- 环境图运行时切换/多环境插值(Metal 的 day/evening/night 体系)。
- 以 KTX 替代运行时生成的 IBL 资产。开发期对拍使用独立的最小 KTX1 解析器，仅在指定参考目录时读取外部资产。
- 天空盒渲染环境图(Metal 原版天空即纯色,保持解耦)。
