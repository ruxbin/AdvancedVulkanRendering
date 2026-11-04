# PBRT Exporter 功能审查与问题清单

审查日期：2026-09-25

审查对象：当前工作区的 `Src/PbrtExporter.cpp`、`Src/PbrtExporter.h` 及相关场景、材质和 shader 代码。

状态：本文前半部分保留原始审查记录；修复与验证进度见末尾的修复状态及 2026-09-26 接续复核。

## 结论

Exporter 已具备基础的场景导出流程，能组织几何、贴图、材质、相机和灯光资源。但当前实现存在法线解码、透明裁剪、材质参数转换和错误处理等问题，尚不适合作为实时渲染效果的可靠对照。

本次记录 7 项问题：3 项 P1、4 项 P2。P1 表示应优先修复的加载阻断或明显画面错误；P2 表示影响保真度、特定输入正确性或导出可靠性的问题。

## 当前功能范围

| 模块 | 当前行为 |
| --- | --- |
| 入口 | Vulkan `GpuScene` 的 ImGui `Export PBRT` 按钮，同步调用导出，结果写入日志 |
| 输出位置 | 场景根目录的 `scene.pbrt`，PLY 与 BMP 资源保存在同目录，使用相对文件名引用 |
| 几何 | 每个非空子网格输出一个 binary little-endian PLY；包含位置、可用的法线和 UV，V 坐标翻转；包含 16/32 位索引处理分支 |
| 贴图 | 从 CPU 侧场景数据解压贴图，支持 BC1 sRGB、BC3 sRGB、BC5 UNORM；导出 BaseColor、粗糙度 G 通道和重建 Z 的法线图 |
| 材质 | 根据常量 metallic 是否大于 0.5，选择 `conductor` 或 `coateddiffuse`；输出反射率、粗糙度和可用法线图 |
| 相机 | 使用当前相机位置、方向和 FOV；up 固定为 `(0,1,0)` |
| 灯光 | 导出太阳方向光、点光源、聚光灯；局部灯光使用 `localLightIntensity` |
| 渲染配置 | `path` integrator、maxdepth 8、`zsobol` 256 samples、Gaussian filter；宽高各减半，输出 GBuffer `output.exr` |

以上为代码所实现的行为，不代表所有分支均已通过端到端验证。透明度、自发光的缺失详见下文；体积、decal、动画等不在原设计范围内，本次未作为缺陷列入。

## 问题清单

### 1. [P1] 法线 BMP 被 PBRT 按 sRGB 解码

- **位置**：`Src/PbrtExporter.cpp:691–694`，以及 `ExportTextureData` / `WriteBMP`。
- **触发条件**：材质使用导出的 `_nm.bmp` 法线图。
- **原因**：Exporter 将线性编码的法线分量直接写入 BMP。PBRT 加载 normal map 时虽然要求 `ColorEncoding::Linear`，但其 BMP 所走的通用图片读取分支固定创建 sRGB 图像，忽略该参数。本机 PBRT 源码与核对的上游源码均存在此行为。
- **影响**：中性法线也会偏斜。例如 `(128,128,255)` 经该 sRGB 解码和归一化后约为 `(-0.443,-0.443,0.780)`，不再接近 `(0,0,1)`，会系统性改变明暗与高光。
- **建议修复**：法线图改用 PBRT 能按线性数据读取的格式，例如 PNG，并通过实际加载结果验证；不能仅增加一个 linear 参数而继续依赖当前 BMP 读取分支。切线空间方向是否一致需要另行验证。
- **建议验收**：平坦法线贴图与不使用法线贴图的结果基本一致；再用分别沿切线 X/Y 扰动的贴图检查方向。

依据：[PBRT 图像读取实现](https://github.com/mmp/pbrt-v4/blob/master/src/pbrt/util/image.cpp)、[法线图加载实现](https://github.com/mmp/pbrt-v4/blob/master/src/pbrt/scene.cpp)。上游链接指向可变分支，结论对应审查时读取的代码。

### 2. [P1] Alpha mask 和透明度未导出

- **位置**：`Src/PbrtExporter.cpp:593–598`；源材质字段见 `Src/Include/GpuScene.h` 的 `AAPLMaterial`。
- **触发条件**：材质存在 `hasDiffuseMask`、贴图透明区域或小于 1 的 `opacity`。
- **原因**：几何统一输出普通 `plymesh`，没有生成 alpha 参数或透明材质映射；材质导出也未读取这些字段。
- **影响**：叶片等裁剪卡片会成为实心面，改变轮廓、可见性和阴影。半透明表面也无法保留当前外观。
- **实证**：已有 `Foliage_Leaves_BaseColor.bmp` 为 2048×2048，其中 2,878,890 个像素 alpha 为零，约占 68.6%；已有 `scene.pbrt` 的 alpha 参数数量为 0。
- **建议修复**：分别定义 alpha cutout 和半透明材质的映射。Cutout 应导出独立遮罩并匹配实时 shader 的阈值；半透明度需要明确 PBRT 材质语义，不能直接假设等同于玻璃透射。当前 PBRT BMP 读取分支还会丢弃 alpha，因此不应依赖 BaseColor BMP 自动保留遮罩。
- **建议验收**：使用叶片卡片和带遮罩阴影的最小场景检查轮廓、遮挡与阴影；单独验证 opacity 小于 1 的材质。

### 3. [P1] BaseColor 导出失败后仍引用未定义纹理

- **位置**：`Src/PbrtExporter.cpp:515–519`；纹理声明的成功判断见 `743–744` 及 `declColorTex`。
- **触发条件**：`hasBaseColorTexture` 为真，但 hash 查找、数据获取或文件写入失败，未形成有效导出记录。
- **原因**：纹理声明检查了 `exportedColorTexNames`，材质引用却只检查 `hasBaseColorTexture`。虽然 `WriteMaterials` 接收成功导出映射，但没有用它检查 BaseColor。
- **影响**：输出包含对不存在的 `color_N` 的引用，PBRT 无法正常加载；原本可降级处理的资源失败变成场景加载失败。
- **建议修复**：仅在导出映射中存在该 hash 时写入纹理引用，否则输出常量 `baseColor.rgb`，并记录可定位到材质和 hash 的警告。
- **建议验收**：构造不存在的 hash 和贴图写入失败，确认材质回退到常量，输出中没有悬空引用。

### 4. [P2] 粗糙度未转换到相同的 GGX 参数

- **位置**：`Src/PbrtExporter.cpp:520–533`；实时公式见 `shaders/lighting.hlsl:39` 和 `shaders/rt_lighting.hlsl`。
- **触发条件**：使用非端点粗糙度的材质，包括常量和纹理分支。
- **原因**：实时 shader 使用 `alpha = roughness²`。Exporter 直接将 roughness 写入 PBRT 的 `uroughness/vroughness`，未关闭默认 `remaproughness`；PBRT v4 的 remap 使用 `sqrt(roughness)`。
- **影响**：即使输入同为常量 0.5，两者 alpha 分别为 0.25 与约 0.707，导致高光宽度和材质观感不一致。这一差异独立于贴图颜色空间问题。
- **建议修复**：明确源贴图采样后的数值语义，计算与实时 shader 一致的 alpha，再设置 `remaproughness=false`。纹理分支应在正确解码之后进行平方，并考虑实时 shader 的粗糙度下限。
- **建议验收**：对常量粗糙度 0.1、0.5、0.9 及对应均匀贴图进行参数与高光对比。不要把修正 alpha 视为两种完整 BSDF 已严格等价。

依据：[PBRT RoughnessToAlpha 实现](https://github.com/mmp/pbrt-v4/blob/master/src/pbrt/util/scattering.h)、[材质参数说明](https://pbrt.org/fileformat-v4)。

### 5. [P2] BC3 颜色块误用 BC1 三色模式

- **位置**：`Src/PbrtExporter.cpp:128–130`。
- **触发条件**：BC3 颜色端点满足 `c0 <= c1`，且像素使用插值颜色索引。
- **原因**：`DecodeBC3Block` 直接复用 `DecodeBC1Block`；后者在该端点顺序下进入 BC1 三色/透明模式，而 BC3 颜色块应使用四色插值。
- **影响**：部分像素被解为黑色或错误中间色，影响导出贴图。
- **实证**：从当前源码提取原解码函数并编译运行；设置 `c0=黑色`、`c1=白色`、颜色索引为 3，期望 RGB 为 `(170,170,170)`，实际得到 `(0,0,0)`。
- **建议修复**：将颜色块解码的四色模式显式参数化，BC3 强制四色；保持 BC1 原有透明模式。
- **建议验收**：覆盖端点大小关系和四种颜色索引，并同时检查 BC1 的透明模式未被破坏。

### 6. [P2] 自发光材质未导出

- **位置**：`Src/PbrtExporter.cpp:698–700` 的贴图导出范围，以及 `WriteMaterials` / `WriteGeometry`。
- **触发条件**：场景含有 emissive 贴图或需要保留自发光属性的材质。
- **原因**：未处理 `emissiveTextureHash`、`emissiveColor` 和 `emissiveScale`，也没有输出 `AreaLightSource`。
- **影响**：招牌、灯罩等表面失去发光外观和相应的间接照明贡献。已有导出文件的面积光声明数量为 0。
- **建议修复**：导出有效 emissive 数据，在对应几何的 attribute scope 内绑定面积光，并明确亮度缩放和双面发光约定。
- **建议验收**：关闭显式灯光，仅保留发光面，确认表面可见且能够照亮附近漫反射物体；验证缩放参数生效。

### 7. [P2] 不完整的导出可能返回成功

- **位置**：`Src/PbrtExporter.cpp:588–590`、`751–756`。
- **触发条件**：PLY 写入失败、主场景写入中途失败，或缺少必要几何数据。
- **原因**：`WriteGeometry` 以 `void` 返回，失败网格被跳过，错误不会传播到 `Export`；主输出流只检查打开成功，没有显式检查最终写入、flush 和 close 状态。
- **影响**：场景缺失几何或文件被截断时，调用方仍可能得到 `true`，日志显示导出成功。
- **建议修复**：传播必要资源失败状态；在报告成功前显式完成并检查写入。区分允许回退的材质资源警告和导致导出失败的几何/主文件错误。进一步可采用临时目录或临时文件完成后再替换，避免破坏已有导出。
- **建议验收**：在隔离目录中模拟 PLY 文件打开失败和流写入失败，确认返回失败并报告具体文件，不输出成功日志。

## 已完成的验证与限制

| 验证项 | 结果 |
| --- | --- |
| 当前实现静态审查 | 检查了导出器、相关材质字段、调用入口及实时 shader |
| PBRT 行为核对 | 核对本机 `D:/SourceCode/pbrt-v4` 源码及上游图像读取、法线加载、粗糙度转换实现 |
| 已有场景资源引用 | 202 个材质、202 个网格、202 个 normalmap 引用；被检查的资源引用均存在 |
| 已有 PLY 检查 | 202 个文件，共 3,963,005 个导出顶点记录、3,868,455 个三角形；文件长度和索引范围检查未发现错误 |
| Alpha 数据检查 | 确认叶片纹理存在大量透明像素，场景没有 alpha 参数 |
| BC3 最小复现 | 原函数提取后编译运行，确认四色插值错误；临时测试文件已清理 |
| 法线数值推导 | 按 PBRT 对 BMP 的读取路径计算中性法线偏斜，未执行图像渲染对比 |

本次检查的 `scene.pbrt`、PLY 和 BMP 是工作区已有产物，并非本次重新导出。因此，上述资源检查只说明这些既有文件的结构和引用情况，不能证明当前实现全部通过验证。

本次未运行完整 Vulkan 导出或 PBRT 端到端渲染，未进行 GPU/CPU 图像一致性比较。当前 `Tests` 目录未发现 exporter 专项测试。灯光单位、聚光灯衰减、切线空间方向及金属材质映射仍需后续保真度验证，未在本文作为已复现问题追加。

## 建议处理顺序

1. 优先修复法线线性读取、alpha mask 和 BaseColor 失败回退，消除显著画面错误及加载阻断。
2. 修复 BC3 解码和错误传播，建立最小解码、资源缺失和写入失败回归验证。
3. 校准粗糙度转换并补齐自发光，用最小场景进行 PBRT 渲染对比。
4. 在上述问题解决后，再验证完整场景的材质、灯光和切线空间一致性。

代码行号以本次审查时的工作区为准，后续修改可能导致偏移。

---

## 修复状态（2026-09-25，第二 agent 接续完成）

第二 agent 已完成 7 项问题的主体修复，彼时回归套件 17/17 通过。随后复核发现错误处理及缓存边界仍有遗漏，详见下方 2026-09-26 接续记录。修复未提交（计划约束），改动在 `Src/PbrtExporter.cpp`、`Src/PbrtExporter.h` 与 `Tests/`。

| # | 状态 | 修复方式 |
| --- | --- | --- |
| 1 法线 sRGB | ✅ | 法线/粗糙度等线性数据贴图改输出 PFM（线性浮点，PBRT 原生按线性读取）；BaseColor/emissive 颜色贴图保留 BMP（PBRT 对 BMP 的 sRGB 解码对颜色语义恰好正确） |
| 2 alpha mask/透明度 | ✅ | 遮罩按 (hash, opacity, cutout) 组合导出独立线性 PFM 并绑定到 Shape 的 `texture alpha`；cutout 用实时 shader 的 `alpha * opacity >= 0.1` 阈值；非不透明材质用 alpha×opacity 的覆盖率近似 |
| 3 BaseColor 悬空引用 | ✅ | 材质仅在导出映射中存在该 hash 时写纹理引用，否则回退常量并警告（可定位材质与 hash) |
| 4 粗糙度转换 | ✅ | 贴图分支按源格式 sRGB 解码（浮点精度），clamp 到实时 shader 的 0.08 下限后平方；常量分支同规则；所有材质 `remaproughness=false` |
| 5 BC3 三色模式 | ✅ | 颜色块解码增加四色强制参数，BC3 恒四色插值；BC1 透明分支不变（有回归保护） |
| 6 自发光 | ✅ | emissive 贴图导出（BMP）并在几何 attribute scope 内生成 `AreaLightSource "diffuse"`；贴图缺失回退 `emissiveColor` 常量；`scale` 取 `frameConstants.emissiveScale` |
| 7 误报成功 | ✅ | `WriteGeometry` 返回状态：缺几何数据、索引越界（>= vertexCount)、任一 PLY 写失败均使导出失败；主输出在报告成功前显式 flush 并检查流状态 |

### 明确的近似与限制

- **透明度近似**:PBRT 的 Shape `alpha` 是覆盖率语义，非玻璃折射模型；半透明材质以 `texelAlpha * opacity` 遮罩近似，与实时混合结果不完全等价。
- **遮罩常量回退**：cutout 材质缺遮罩贴图时退化为 `threshold(opacity)` 常量；opacity<0.1 时全透明，否则全不透明。因此缺图时仍可能形成实心卡片，不能声称保留了原裁剪轮廓。
- **双面发光**：面积光未开 `twosided`，与实时渲染的背面剔除一致；叶片等双面几何的背面不发光。
- **PFM 体积**：线性贴图用未压缩 PFM；RGB 法线图每像素 12 字节，约为原 32-bit BMP 的三倍；灰度粗糙度和遮罩每像素 4 字节，与原 BMP 数据量接近。
- **浮点解码路径**:BMP（颜色）路径保持原字节解码不变；PFM（线性）路径用端点全精度浮点解码，二者在插值像素上可能差 ±1/255，属预期。
- **未纳入本次**：灯光单位、金属模型、切线空间方向的全面校准（审查文档原范围外）；PBRT 端到端渲染对比（本机 pbrt-v4 源码在，但未构建 pbrt.exe,smoke test 未执行）。

### 测试命令

```powershell
# 离线回归(g++ + python,无需 Vulkan 设备;fixture 直接编译真实 PbrtExporter.cpp)
python Tests/test_pbrt_exporter.py
# 应用集成编译
cmake --build build-msvc --config Debug --target AdvancedVulkanRendering
```

回归套件 17 条覆盖：BC1 透明/BC3 四色、法线线性与非 4 倍数裁剪、粗糙度常量与贴图（sRGB 解码后平方）、BaseColor 缺失回退、遮罩绑定/覆盖率/共享图不同 opacity/缺遮罩回退、emissive 常量/贴图/缺失回退、PLY 失败/缺几何/主文件失败的错误传播。

### 后续独立评审

最终独立 review 的结论记录于计划文件 `docs/superpowers/plans/2026-09-25-pbrt-exporter-fixes.md` 的执行记录。

## 2026-09-26 接续复核与补充修复

在保留第二 agent 改动的基础上，先复跑 17 条原有测试，再增加针对未覆盖边界的实际导出测试。新测试在修复前出现 9 个失败和 1 个空 mesh 访问异常，确认以下遗漏并继续修复：

| 遗漏 | 接续修复 |
| --- | --- |
| 索引范围在扫描前没有验证，尾部不足一个三角形仍被接受 | 先检查 indexBegin/indexCount 是否落在缓冲区内、是否为完整三角形，并拒绝未知索引类型；已有顶点索引上界检查保留 |
| 空 mesh 会解引用，材质数组缺失时可能输出未定义 NamedMaterial | 在入口返回失败，避免访问和不完整的材质声明 |
| BMP/PFM/PLY 只检查当时的流状态；主文件未检查 close | 所有输出统一在 flush、close 后检查状态，再返回成功或发布资源引用 |
| opacity 转字符串作为 cache key 会丢失精度 | 使用 opacity 的完整浮点位模式；0.0999999 与 0.1000001 不再错误共享 cutout 遮罩 |
| 不同目录内同名纹理导出时相互覆盖 | 资源文件名以 texture hash 为身份，加用途后缀；颜色和线性贴图均适用 |
| 不支持的纹理格式、零尺寸或无效 streamingEntryMap 索引 | 在访问/解码前拒绝，走常量或无 normalmap 的降级路径，避免黑图、无效图像和越界 |
| 原注释声称 PLY 已输出显式 tangent | 删除该不实说明及失去调用方的 flipGreen 参数；TBN 校准仍明确列为后续工作 |

最新验证：

- `python Tests/test_pbrt_exporter.py`：23/23 通过（含表驱动子用例）。新增测试同时覆盖有效 16 位索引和发光作用域隔离。
- `cmake --build build-msvc --config Debug --target AdvancedVulkanRendering`：成功。首次沙箱构建因 Windows SDK 配置目录读取权限失败，授权重试后成功；仍有 C4819 编码、第三方 fmt C4996 和 LNK4098 运行库冲突警告。
- `DX12TextureStreamingPolicyTests`、`DX12ScenePolicyTests`、`SphericalHarmonicsTests`：从当前源码独立编译并运行，全部通过。
- 未重新生成工作区已有场景资产；未执行完整 Vulkan 导出或 PBRT 渲染。单元 fixture 隔离了 LZFSE，应用编译只证明其集成能构建，不证明真实压缩数据的运行时行为。

尚待验证：PBRT 端到端加载/渲染、真实场景 TBN/光度一致性，以及真实存储设备耗尽等故障注入；不能将当前通过的测试等同于这些验证已完成。

## 2026-10-07：点光源位置排查与解压问题收尾

### 点光源位置（尚未定位用户报告的错位）

- 对比当前工作区 `scene.pbrt` 与 `scene.scene`：两者均有 100 个 point light，逐项 XYZ 最大差值约 4.97e-7，符合六位小数的导出精度。
- 本机 PBRT-v4 `src/pbrt/lights.cpp` 的 `PointLight::Create` 读取 `point3 from` 并合成平移；`scene.cpp` 的 `WorldBegin` 重置场景变换。因此现有参数本身有效，相机 LookAt 不会叠加到点光源世界坐标。
- 新增使用两个非对称、带正负分量的位置以及非原点相机的实际导出回归，通过；同时检查 WorldBegin 后没有泄漏的物体变换。
- **尚未修改灯光位置或强度**。等待确认用户实际使用的渲染器/导入工具、对应导出文件及错位表现；上述检查不能证明用户看到的画面正确，也不能据此声称问题已修复。

### 上轮独立复核发现的两项 P1 已修复

- exporter 原来调用的通用 `uncompressData` 会在坏头部时退出进程，且忽略 LZFSE 实际解码长度。现在使用专用受检解码：验证 24-byte 头部、模式、压缩和解压尺寸，以及 codec 返回值；损坏的 mip 尝试下一级，全部失败则回退材质。
- mip 和纹理偏移原来未与实际 blob 大小校验。`AAPLMeshData` 记录实际读入的纹理字节数，桌面/Android 读取路径均填写；exporter 在构造指针前使用减法式边界检查。
- 测试改为链接真实 LZFSE 解码器，移除旧测试桩。上轮 RED 为 2 failures + 3 errors；现覆盖截断头部、坏码流、尺寸不符、错误压缩模式、偏移越界、解码超长，以及坏 mip 回退至有效粗 mip。
- `python Tests/test_pbrt_exporter.py`：**26/26 通过**。PBRT 端到端渲染仍未执行。
- 主应用 MSVC Debug 构建通过（exit 0）；沙箱无法读取 Windows SDK 配置，授权重试成功。仍存在 C4819、第三方 C4996、LNK4098 警告。`git diff --check` 通过。

### 用户补充：灯泡几何包围 point light

用户确认部分 point light 位于发光 shape 内部。该反馈指向灯具表示与遮挡问题，坐标相等的回归不能覆盖它：不透明灯泡表面会遮挡通向内部 point light 的可见性射线，附加 AreaLightSource 不会使表面自动透光。

另外，本机 PBRT-v4 `scene.cpp` 为网格展开的每个 shape 创建面积光，`DiffuseAreaLight::Create` 每次读取 filename 图像。因此发光三角形数量可放大图像内存，exporter 按 hash 去重输出文件不能解决 PBRT 的这部分重复加载。

建议方案（尚未实施）：

- 同一灯具明确选择一个照明表示。离线物理渲染优先保留灯泡外表面的面积光，使用常量发光色或低分辨率图，并移除明确配对的内部 point light；亮度需要重新校准，不能直接把 I 赋给 L。
- 对照实时点光源时保留 point light，移除明确配对的灯泡发光壳体及其面积光，保留灯罩/支架。代价是灯泡不再可见；若需要可见发光但不遮挡/不参与照明，需额外的渲染器可见性支持。
- 配对必须基于显式规则或经确认的几何关联；不能删除所有 emissive 网格或仅凭整个 submesh 包围盒包含点光源就自动删除。
- 招牌等独立纹理发光面继续保留；降低发光贴图分辨率，必要时在 PBRT 中共享不可变图像数据，避免逐三角形重复载入。
- 仅禁用 AreaLightSource 不能消除壳体遮挡；twosided 也不改变透明性。不将普通玻璃材质作为当前 path 积分器下点光源直射可见性的通用修复。

## 2026-10-08：实现实时点光源对照策略

用户选择保留 point light、移除与其配对的发光灯泡壳体。该策略现已作为 exporter 默认行为实现；灯的位置、颜色、强度保持原导出规则。

### 实现

- 纹理导出前先验证几何并分析发光面。跨材质、跨 submesh 汇总，按焊接后的共享边划分部件；仅共享一个顶点的装饰片不会被并入灯泡。
- AABB 只用于快速排除。实际采用有向立体角判定：部件须覆盖 point light 周围至少 90% 的球面，且无非流形边，才作为配对壳体移除。Bistro 灯泡有小开口，最初要求完全封闭的实现未识别到任何壳体，故改为此保守阈值。
- 仅移除命中的部件，保留同材质的未匹配几何及不发光灯罩/支架；剩余面映射回原 submesh，保留原顶点属性和 UV。
- 重新计算材质使用情况：整块壳体移除后，不再声明其独占材质、不导出其独占贴图、不创建其面积光。共享资源仍按剩余几何所需保留。

### 实际验证

- `python Tests/test_pbrt_exporter.py`：**32/32 通过**。覆盖闭合壳体、开口灯颈、同材质多个部件、包围盒内但壳体外的 point、普通非发光几何、UV 接缝顶点、16 位索引、资源全部裁剪、共享资源保留、只共顶点的装饰片、跨材质拆分壳体。
- 新用例均先在未修复实现上确认失败，再验证修复后通过。独立复核提出的“顶点连接误删”和“跨 submesh 漏删”两项均已修正并复核。
- `cmake --build build-msvc --config Debug --target AdvancedVulkanRendering`：成功，exit 0。修复了初版辅助函数访问 GpuScene 私有字段导致的集成编译错误；采用从 exporter 显式传入所需数据。仍有既有编码/第三方弃用警告。
- 直接解码本地 `bistro.dxt.bin` 的顶点、索引、材质和网格数据，配合 `scene.scene` 的 100 个 point light，对最终过滤函数进行离线检查：**移除 62 个壳体、16,712 个三角形**。其中路灯 5 个壳体，串灯 57 个；发光三角形从 64,563 减少至 47,851。书店、药店招牌面保留。该计数是壳体数量，不等同于被处理的灯光数量。

### 当前限制

- 本轮未执行完整 Vulkan 场景重新导出或 PBRT 渲染，也未测量 PBRT 峰值内存。须使用新构建重新导出才能应用本策略。
- **并非所有面积光都被删除**。未匹配灯具、室内灯和招牌继续保留；部分材质仍有剩余部件，所以仍需要其发光贴图。当前修复减少重复加载的面积光数量，不代表全部面积光内存问题已消失。
- 判定为几何启发式，要求准确重合的接缝位置和较一致的绕序；较大开口、非流形壳体、跨不发光面的壳体等可能保守地保留。不自动移动点光源或删除未确认的灯罩。
- 重复导出到旧目录时不主动删除历史 PLY/纹理文件；新 scene 不再引用已移除资源，因此这些残留不会被新场景加载。需要干净资源包时导出到新目录。
- 改动未提交，保留在当前工作区。

## 2026-10-14：街道路灯仍挡光的补充修复

用户实测上一轮仍存在街道路灯遮挡。真实数据复查确认：`Emissive_StreetLight` 的开口灯罩分量多为 24/48 个三角形，对 point light 的立体角覆盖约 72%–88%，未达到此前 90% 的壳体阈值；不发光金属和支撑件也不在此前发光壳体过滤范围内。

### 修复范围

- 保留通用的 90% 发光壳体规则，追加一个 **Bistro 专用规则**，精确匹配源 BaseColor 贴图 basename：`Emissive_StreetLight_BaseColor.dds`、`Streetlight_Metal_BaseColor.dds`、`Streetlight_Support_Bulb_BaseColor.dds`。
- 对这三类表面，移除与任一点光周围 **0.3 米球体相交的三角面**。采用点到三角面的最短距离，包含面内部、边、顶点、退化三角形；不能只检查顶点，因为大面可能顶点都在球外却横穿灯光附近。
- 该规则不要求 point 位于面的 AABB 内，不要求材质发光，也不要求组成封闭壳体；不扩展到其他材质。保留远处灯杆、链条和场景几何。
- 按保留面重建索引和资源使用关系；point 的位置、强度不改。三角面按整面移除而非裁剪，因此相交面超出球体的部分也会消失；这是实时点光对照模式下的几何近似。

### 验证结果

- 新增近距离开口金属/发光灯罩回归：旧实现两项失败，修复后通过；远处同材质面、近处无关材质面保持不变。完整套件 **34/34 通过**。
- MSVC Debug 主应用构建 exit 0，独立只读复核未发现重要缺陷。
- 直接对原始 Bistro 路灯数据应用新增局部规则（以下为该规则独立计数）：

| 网格 | 原始三角形 | 保留三角形 |
| --- | ---: | ---: |
| 72 / Emissive_StreetLight | 5,704 | 8 |
| 100 / Streetlight_Metal | 185,880 | 152,870 |
| 101 / Streetlight_Chains（不在匹配名单中） | 288 | 288 |
| 103 / Streetlight_Support_Bulb | 2,128 | 0 |

- 独立射线相交检查：100 个 point light，每个 128 个球面方向，共 12,800 条射线。对原始路灯网格与应用局部过滤后的网格比较，0.3 米内的命中从 **4,196 降为 0**；1 米内从 4,204 降为 217，说明更远的结构遮挡仍保留。此测试使用真实网格及独立的射线/三角面求交，不是 PBRT 渲染结果。
- 未运行完整 PBRT 渲染或内存峰值测量；使用新构建重新导出后才能生效。旧导出资产未覆盖。
- 文件名匹配与 0.3 米范围均针对本仓库 Bistro 的尺度；其他资产需要显式对应策略，不应把该规则推广为全场景删面。匹配材质在范围内的其他三角面也会被移除。

## 2026-11-04：以当前硬件 Path Tracing 为基准的材质复核

本轮基准是 `shaders/rt_path_common.hlsl`、`shaders/rt_lighting.hlsl` 和实际 GPU 材质上传路径，而非光栅 PBR 或 CPU 材质结构的设计含义。结论是：当前差异确实不只是 IBL，金属度、法线方向和光照单位都会改变材质观感。以下结论修正历史记录中“常量粗糙度/alpha 已匹配实时”的表述；历史测试通过只证明旧策略得到实现，并不证明与当前 RT 等价。

### 已确认并修复

| 优先级 | 问题与可见影响 | 本轮处理 |
| --- | --- | --- |
| P1 | RT 用 MR 贴图 B 通道插值金属度；exporter 却用 CPU 常量 x 和 0.5 阈值选择整块材质，丢失金属区域和过渡 | 导出线性 B 通道 PFM，用 `mix` 混合 `coateddiffuse` / `conductor`；0 对应介质，1 对应金属 |
| P1 | RT 没有 MR 贴图时初始化 `materialData=0`，最终 roughness=0.08；exporter 用 CPU 常量 y，导致高光宽度不同 | 无 MR 贴图时使用非金属、alpha=0.0064；贴图导出失败也采用这一确定性回退，不留下缺失纹理引用 |
| P1 | RT 直射光颜色乘 π；旧 exporter 乘 1/π，PBRT-v4 创建光源还会再除以光度归一化因子 | 点光、聚光、太阳 scale 改为 π×106.856895；面积光为 emissiveScale×106.856895；显式声明 sRGB |
| P2 | RT miss 有天空渐变，PBRT 没有环境光，暗部和金属反射缺少环境贡献 | 生成 128×128 线性 RGB `rt_sky.pfm` 并声明 `infinite` 光源，scale=106.856895 |

代码位置：[材质输出](</F:/AdvancedVulkanRendering/Src/PbrtExporter.cpp:948>)、[光源输出](</F:/AdvancedVulkanRendering/Src/PbrtExporter.cpp:1182>)、[天空输出](</F:/AdvancedVulkanRendering/Src/PbrtExporter.cpp:1232>)。

金属度与粗糙度沿用实际资源颜色空间：本项目 MR 所用 BC1/BC3 格式是 sRGB，GPU 会先对 RGB 解码，再读取 G/B；导出同样先解码，不按一般 glTF 的线性 MR 假设处理。粗糙度仍采用 `max(G,0.08)²` 且 `remaproughness=false`。介质显式 eta=1.5，对应法向 F0=0.04。

光度系数来自本机 PBRT-v4 `SpectrumToPhotometric` 的实际实现：对 RGB 光源只积分其标准 illuminant；sRGB illuminant 归一化后积分为 `CIE_Y_integral=106.856895`。点光、聚光、distant、面积光和图像 infinite 创建时均除以这一项，所以导出显式抵消它。此为代码级单位核对，尚未做灰卡渲染测量；不要再叠加旧导出为补偿暗图而人工设置的曝光倍率。

### IBL 选择

当前 RT miss 使用 `skyColor * (0.6 + 0.4 * saturate(dir.y*0.5+0.5)^2)`，没有采样光栅渲染的 HDR 环境图。因此本轮添加与 RT 同源的天空环境，而不是换成另一张 HDR。PBRT 环境同时参与可见背景、反射和间接照明；采样方式与 RT 不同。

- 图像为 PBRT-v4 要求的方形 equal-area 球面投影，直接按世界 Y 方向生成；没有额外旋转或经纬图误用。
- 128² RGB float 像素约 192 KiB，不产生逐三角形 AreaLightSource。
- skyColor 全零时不输出环境光；文件写入失败时整个导出返回 false。
- 一般 HDR 经纬图不能直接代替该 PFM，需要先转为 PBRT-v4 equal-area 格式。格式依据：[PBRT-v4 scene format](https://www.pbrt.org/fileformat-v4)、[PBRT-v4 users guide](https://www.pbrt.org/users-guide-v4)。

### 仍未解决，按后续优先级排序

1. **P1：法线贴图的切线空间不一致。** [RT](</F:/AdvancedVulkanRendering/shaders/rt_path_common.hlsl:369>) 使用模型的顶点 T，令 B=cross(T,N)，法线为 `blue*N - green*T + red*B`。PBRT `NormalMap` 则将 R/G 对应 UV 推导的 dpdu / cross(N,dpdu)。当前 PLY 不保存源 T，原样输出贴图不能保持一般模型的世界空间法线。这会改变凹凸方向、高光位置和强度。仅翻绿通道不足以修复；即使假设 T≈dpdu，仍涉及 R/G 交换和符号，而且该假设未对资产验证。PBRT 本地 PLY 路径不保留切线 S，不能只往 PLY 加几个字段就认为生效。下一步先用已知 T、UV、单色 R/G 偏移贴图的平面做法线对照，再选择保留切线的几何表示或重新烘焙法线。
2. **P1：RT 无 BaseColor/Normal 贴图时基准未定义。** [GpuScene 上传](</F:/AdvancedVulkanRendering/Src/GpuScene.cpp:2396>) 对缺失纹理填写 `0xffffffff`，而 [ClosestHit](</F:/AdvancedVulkanRendering/shaders/rt_path_common.hlsl:351>) 无条件采样这两个索引。必须先为 RT 补齐合法默认纹理/采样分支，才能比较无贴图材质；exporter 的安全常量回退不能模拟未定义 descriptor 访问。本轮没有改 RT shader。
3. **P2：PBRT 标准 BSDF 不是当前 RT BRDF。** RT 使用 Schlick Fresnel、近似 Smith GGX 与 Lambert 项，F0 随金属度插值。PBRT 的 coateddiffuse 是分层 BSDF，conductor 使用导体 Fresnel，mix 是按权重随机选两种材质；它们不是同一条公式。中间金属度、掠射角和粗糙金属尤其可能有差异。本轮保留标准 PBRT 材质兼容性，不宣称逐像素匹配；严格一致需要专用 BSDF 或统一 RT 的 BRDF。
4. **P2：透明裁剪规则仍不同。** [RT AnyHit](</F:/AdvancedVulkanRendering/shaders/rt_path_common.hlsl:403>) 比较纹理 alpha<0.1，不乘 opacity；[导出器](</F:/AdvancedVulkanRendering/Src/PbrtExporter.cpp:1412>) 比较 alpha×opacity。opacity<1 的 cutout 材质可能出现轮廓/阴影差异，PBRT 的 fractional coverage 也不是 RT 的同一透明模型。旧 alpha 测试刻画的是既有导出策略；后续需统一语义后更新双方，而不是把测试通过当成匹配证明。
5. **P2：CPU 常量发光仍可能造成额外亮面。** RT 在无 emissive 贴图时发光为零，未上传 CPU emissiveColor；exporter 保留常量 emissiveColor 作为面积光回退。这是旧功能，当前没有切换语义，需在无发光贴图的材质上单独核对。
6. **P2：光形与积分策略不同。** RT 点光/聚光含有限半径平滑衰减并在半径外归零；PBRT 当前为无限域 inverse-square。RT 只在首个命中做局部灯 NEE，PBRT 后续反弹也计算；太阳锥角、spot 内锥过渡也未对齐。强度单位修正无法消除这些空间差异。先前用户选择的灯泡壳体/街灯局部删面策略继续保留，本身也改变几何。
7. **P2：显示与采样条件未统一。** [RT](</F:/AdvancedVulkanRendering/shaders/rt_lighting.hlsl:34>) 对 radiance/throughput 做 20 的 clamp，最终 ACES 曲线内部还乘 0.6；PBRT 输出线性 EXR。PBRT 当前固定 maxdepth=8、256 spp、半分辨率，未与 RT 的动态 bounce/spp 设置联动。纹理 mip/过滤、先插值再平方 vs 先平方再过滤也可能改变高光。直接比较截图与未经同样处理的 EXR 会误判材质。

### 验证与建议对照方法

- 新增四个核心回归先在旧实现上复现失败：金属度、无 MR 粗糙度、光照单位、天空缺失。
- 最终 `python Tests/test_pbrt_exporter.py` **40/40 通过**；含 MR 失败回退、天空文件写失败、天空方向/颜色值、面积光强度，以及既有解码、裁剪、点光位置/灯罩用例。测试使用真实 exporter 和 LZFSE，CPU scene fixture 不需要 Vulkan。
- MSVC Debug 主应用构建通过，exit 0；保留既有编译警告。独立只读复核核对了本机 PBRT 的 mix 顺序、归一化、equal-area 映射和失败路径，未发现本轮新增 P1/P2。
- **没有执行完整 PBRT 渲染对照，也没有重新导出完整 Bistro。** 上述测试证明导出内容与公式，不证明最终图像达到目标；旧 scene/贴图不会自动更新。
- 建议先用新构建导出到新目录；固定相机、分辨率和 bounce。先用无 normalmap 的灰卡/金属球，禁用局部灯与发光，只对齐太阳或天空。比较线性 RT accumulation 与 PBRT EXR，或给两者应用完全相同的曝光与 ACES；再逐项加 normalmap、局部灯和透明材质。法线问题应作为下一轮首要修复，而非继续凭观感调整 roughness 或曝光。
