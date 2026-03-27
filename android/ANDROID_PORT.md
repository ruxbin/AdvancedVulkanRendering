# Android Port - AdvancedVulkanRendering

## 概述

将 Vulkan 1.3 渲染器（Windows/Linux）移植到 Android 平台。使用 Android Studio + Gradle + NDK 构建，基于 SDL2 的 NativeActivity 后端，支持触控操作。

**核心挑战**：Android NDK 同时定义 `__gnu_linux__` 和 `__ANDROID__`，因此所有平台判断必须让 `#ifdef __ANDROID__` 在 `#ifdef __gnu_linux__` 之前。资源加载必须使用 Android AssetManager 替代文件系统 I/O。

---

## 新增文件

### 1. Android 项目结构 (`android/`)

| 文件 | 用途 |
|------|------|
| `android/settings.gradle.kts` | Gradle 项目设置，包含 `:app` 模块 |
| `android/build.gradle.kts` | 项目级 Gradle 配置，AGP 8.2.0 |
| `android/gradle.properties` | NDK 版本 `27.2.12479018`，JVM 堆 4G |
| `android/local.properties` | 本地 Android SDK 路径 |
| `android/app/build.gradle.kts` | App 模块配置（详见下方） |
| `android/app/src/main/AndroidManifest.xml` | 应用清单 |
| `android/app/src/main/jni/CMakeLists.txt` | NDK CMake 构建脚本 |
| `android/app/src/main/java/org/libsdl/app/*.java` | SDL2 Android Java 源码（9 个文件，从 ThirdParty/SDL 复制） |
| `android/gradle/wrapper/*` | Gradle Wrapper（8.2） |

#### `app/build.gradle.kts` 关键配置

```kotlin
compileSdk = 34          // Android 14
minSdk = 28              // Android 9（Vulkan 支持起点）
targetSdk = 34
ndk { abiFilters += "arm64-v8a" }   // 仅 64 位 ARM
externalNativeBuild { cmake { path = "src/main/jni/CMakeLists.txt" } }
androidResources { noCompress += listOf("bin", "spv", "scene") }  // 大文件不压缩，避免 OOM
```

包含 Gradle Copy Task，构建前自动将 `shaders/*.spv`、`debug1.bin`、`scene.scene` 复制到 `src/main/assets/`。

#### `AndroidManifest.xml` 关键配置

```xml
<uses-feature android:name="android.hardware.vulkan.level" android:version="1" android:required="true" />
<activity android:name="org.libsdl.app.SDLActivity"
          android:screenOrientation="landscape"
          android:theme="@android:style/Theme.NoTitleBar.Fullscreen" />
```

#### `jni/CMakeLists.txt` 构建内容

- 从 ThirdParty 子模块编译 SDL2（shared）、spdlog（static）、lzfse（static，源文件直接编译）
- 编译 `Src/` 下所有 `.cpp`（含新增的 `AssetLoader.cpp`）为 `libmain.so`
- 链接 `vulkan`、`android`、`log`

---

### 2. AssetLoader — 平台无关资源加载抽象

#### `Src/Include/AssetLoader.h`

```cpp
namespace AssetLoader {
  std::vector<char> readFileAsset(const std::string &path);           // 替代 readFile()，用于 shader
  std::vector<unsigned char> loadAssetBytes(const std::string &path); // 用于 stbi_load_from_memory / json::parse
  class BinaryFileReader { ... };                                     // 替代 fopen/fread 模式

  // Android 专用
  void setAndroidAssetManager(AAssetManager *mgr);
  AAssetManager *getAndroidAssetManager();
}
```

#### `Src/AssetLoader.cpp`

| 函数 | Android 实现 | 桌面实现 |
|------|-------------|---------|
| `readFileAsset()` | `AAssetManager_open` + `AAsset_read` + `AAsset_close` | `std::ifstream` |
| `loadAssetBytes()` | 同上，返回 `unsigned char` | 同上 |
| `BinaryFileReader` 构造 | `AAssetManager_open(..., AASSET_MODE_STREAMING)` | `fopen(path, "rb")` |
| `BinaryFileReader::read()` | `AAsset_read()` | `fread()` |
| `BinaryFileReader::close()` | `AAsset_close()` | `fclose()` |

---

### 3. VulkanCompat — Vulkan 1.3/1.2 函数兼容层

#### `Src/Include/VulkanCompat.h`

仅在 `__ANDROID__` 下生效。Android `libvulkan.so`（API 28）仅导出 Vulkan 1.1 符号。

| 原函数（Vulkan 1.3/1.2） | 兼容实现（Vulkan 1.0） |
|--------------------------|----------------------|
| `vkCmdPipelineBarrier2(cmd, &depInfo)` | 将 `VkMemoryBarrier2` / `VkImageMemoryBarrier2` 转换为 `VkMemoryBarrier` / `VkImageMemoryBarrier`，调用 `vkCmdPipelineBarrier()` |
| `vkCmdDrawIndexedIndirectCount(cmd, buf, off, countBuf, countOff, max, stride)` | 忽略 countBuffer，直接调用 `vkCmdDrawIndexedIndirect(cmd, buf, off, max, stride)` |

---

## 修改文件

### 1. `Src/Include/Common.h`

**平台宏定义顺序**（第 3-9 行）

```cpp
// 之前：独立的 #ifdef，Android 上 __gnu_linux__ 也会命中
#ifdef _WIN32 ...
#ifdef __gnu_linux__ ...

// 之后：有序链式判断，__ANDROID__ 优先
#ifdef __ANDROID__
  #define VK_USE_PLATFORM_ANDROID_KHR
#elif defined(_WIN32)
  #define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__gnu_linux__)
  #define VK_USE_PLATFORM_XLIB_KHR
#endif
```

**窗口尺寸运行时可配置**（第 17-23 行）

```cpp
#ifdef __ANDROID__
extern int WINDOW_WIDTH;   // 运行时由 SDL_GetCurrentDisplayMode 填充
extern int WINDOW_HEIGHT;
#else
const int WINDOW_WIDTH = 1224;
const int WINDOW_HEIGHT = 691;
#endif
```

---

### 2. `Src/Include/VulkanSetup.h`

**平台宏定义**（第 2-9 行）：同 Common.h 的有序链式判断。

**Vulkan 平台头文件**（第 17-23 行）

```cpp
#ifdef _WIN32
  #include "vulkan/vulkan_win32.h"
#endif
#ifdef __ANDROID__
  #include "vulkan/vulkan_android.h"    // 新增
#elif defined(__gnu_linux__)
  #include "vulkan/vulkan_xlib.h"
#endif
```

**实例扩展**（第 99-111 行）

```cpp
constexpr static const char *const instaceExtensionNames[] = {
    "VK_KHR_surface",
#ifndef __ANDROID__
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,   // Android 上不需要调试扩展
#endif
#ifdef __ANDROID__
    "VK_KHR_android_surface"             // 新增
#elif defined(_WIN32)
    "VK_KHR_win32_surface"
#elif defined(__gnu_linux__)
    "VK_KHR_xlib_surface"
#endif
};
```

**设备扩展**（第 112-121 行）

```cpp
constexpr static const char *const deviceExtensionNames[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
#ifndef __ANDROID__
    VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME,          // 移动端不一定支持
    VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME,     // 移动端不一定支持
    VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME         // 移动端不一定支持
#endif
};
```

**验证层**（第 122-130 行）

```cpp
#ifndef __ANDROID__    // Android 上完全禁用验证层
constexpr static const char *const validationLayers[] = { ... };
#endif
```

**`constexpr` 移除**（第 70 行）

```cpp
// 之前：constexpr std::vector<std::string_view> getRequiredExtensions();
// 之后（NDK clang 不支持 constexpr std::vector 返回类型）：
std::vector<std::string_view> getRequiredExtensions();
```

---

### 3. `Src/VulkanSetup.cpp`

**头文件**（第 1-5 行）

```cpp
#include "VulkanSetup.h"
#ifndef __ANDROID__
#include "SDL_syswm.h"      // Android 不需要 SysWM，直接用 SDL_Vulkan_CreateSurface
#endif
#include "SDL_vulkan.h"      // 新增：SDL Vulkan surface 创建
```

**验证层开关**（第 10-14 行）

```cpp
#ifdef __ANDROID__
static constexpr bool enableValidationLayers = false;  // Android 禁用
#else
static constexpr bool enableValidationLayers = true;
#endif
```

**Vulkan API 版本**（第 72-79 行）

```cpp
#ifdef __ANDROID__
appInfo.apiVersion = VK_API_VERSION_1_1;   // 移动端更广泛的兼容性
#else
appInfo.apiVersion = VK_API_VERSION_1_3;   // 桌面端需要 bindless 等特性
#endif
```

**验证层设置守卫**（第 96-100 行，第 328-332 行）

```cpp
if (enableValidationLayers) {
#ifndef __ANDROID__                       // validationLayers 数组在 Android 上不存在
    createInfo.enabledLayerCount = ...;
    createInfo.ppEnabledLayerNames = validationLayers;
#endif
    ...
}
```

**表面创建**（第 115-159 行）

```cpp
#ifdef __ANDROID__
// Android：SDL 处理一切（ANativeWindow → VkSurface）
if (SDL_Vulkan_CreateSurface(sdl_window, vkInstance, &wsiSurface) == SDL_FALSE) {
    throw std::runtime_error("failed to create Android Vulkan surface via SDL!");
}
#else
// 桌面端：保留原有的 Win32/Xlib 平台特定表面创建
SDL_SysWMinfo wmInfo;
...
#ifdef _WIN32
vkCreateWin32SurfaceKHR(...)
#endif
#ifdef __gnu_linux__
vkCreateXlibSurfaceKHR(...)
#endif
#endif // !__ANDROID__
```

**`getRequiredExtensions()` 扩展列表**（第 186-192 行）

```cpp
// 新增 Android surface 扩展分支
#ifdef __ANDROID__
    "VK_KHR_android_surface"
#elif defined(__gnu_linux__)
    "VK_KHR_xlib_surface"
#elif defined(_WIN32)
    "VK_KHR_win32_surface"
#endif
```

---

### 4. `Src/Include/GpuScene.h`

**新增头文件**（第 4 行）

```cpp
#include "AssetLoader.h"
```

**AAPLTextureData 新增构造函数**（第 34-36 行）

```cpp
#ifdef __ANDROID__
AAPLTextureData(AssetLoader::BinaryFileReader &reader);   // 从 AssetManager 读取纹理元数据
#endif
```

---

### 5. `Src/GpuScene.cpp`

**新增头文件**（第 3、8 行）

```cpp
#include "AssetLoader.h"
#include "VulkanCompat.h"
```

**`readFile()` 替换**（第 26-28 行）

```cpp
// 之前：std::ifstream 读取文件
// 之后：委托给 AssetLoader，Android 自动走 AAssetManager
std::vector<char> readFile(const std::string &filename) {
    return AssetLoader::readFileAsset(filename);
}
```

**`AAPLMeshData` 构造函数**（第 3988-4131 行）

```cpp
#ifdef __ANDROID__
// 用 BinaryFileReader 替代 FILE*/fread
AssetLoader::BinaryFileReader reader(filepath);
if (reader.isOpen()) {
    reader.read(&_vertexCount, ...);
    // ... 所有 fread 调用替换为 reader.read
    for (int i = 0; i < texture_count; ++i) {
        _textures.push_back(AAPLTextureData(reader));  // 使用 reader 版构造函数
    }
    reader.close();
}
#else
FILE *rawFile = fopen(filepath, "rb");
// ... 保留原有 fopen/fread 逻辑
#endif
```

**`AAPLTextureData` 新增 Android 构造函数**（第 3952-3981 行）

```cpp
#ifdef __ANDROID__
AAPLTextureData::AAPLTextureData(AssetLoader::BinaryFileReader &reader) {
    reader.read(&path_length, ...);
    reader.read(&_pathHash, ...);
    // ... 所有 fread 替换为 reader.read
}
#endif
```

**`createTexture()` 纹理加载**（第 4250-4258 行）

```cpp
#ifdef __ANDROID__
// 先整体加载到内存，再用 stbi_load_from_memory 解码
auto texData = AssetLoader::loadAssetBytes(path);
stbi_uc *pixels = stbi_load_from_memory(texData.data(), (int)texData.size(),
                                        &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
#else
stbi_uc *pixels = stbi_load(path.c_str(), ...);   // 桌面端直接从文件路径加载
#endif
```

**`GpuScene` 构造函数 — 资源路径**（第 2179-2196 行）

```cpp
// Mesh 数据
applMesh =
#ifdef __ANDROID__
    new AAPLMeshData("debug1.bin");                        // 相对路径，由 AssetManager 解析
#else
    new AAPLMeshData((_rootPath / "debug1.bin").generic_string().c_str());
#endif

// 场景文件
#ifdef __ANDROID__
auto sceneBytes = AssetLoader::loadAssetBytes("scene.scene");
std::string sceneStr(sceneBytes.begin(), sceneBytes.end());
sceneFile = nlohmann::json::parse(sceneStr);
#else
std::ifstream f(root / "scene.scene");
sceneFile = nlohmann::json::parse(f);
#endif
```

---

### 6. `Src/Window.cpp`

**头文件与全局变量**（第 1-21 行）

```cpp
#ifndef __ANDROID__
#include <filesystem>          // Android 上不需要文件系统遍历
#endif

#ifdef __ANDROID__
#include "AssetLoader.h"
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

int WINDOW_WIDTH = 0;          // extern 声明的定义，运行时赋值
int WINDOW_HEIGHT = 0;
#endif
```

**SDL 初始化与 Android 特化**（第 28-60 行）

```cpp
#ifdef __ANDROID__
// 1. 查询屏幕尺寸
SDL_DisplayMode displayMode;
SDL_GetCurrentDisplayMode(0, &displayMode);
WINDOW_WIDTH = displayMode.w;
WINDOW_HEIGHT = displayMode.h;

// 2. 通过 JNI 获取 AAssetManager
JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
jobject activity = (jobject)SDL_AndroidGetActivity();
// ... getAssets() → AAssetManager_fromJava() → setAndroidAssetManager()

// 3. 创建全屏 Vulkan 窗口
SDL_Window *window = SDL_CreateWindow("AdvancedVulkanRendering",
    SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
    WINDOW_WIDTH, WINDOW_HEIGHT,
    SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
#else
// 桌面端：窗口化 Vulkan 窗口（原来是 SDL_WINDOW_OPENGL，改为 SDL_WINDOW_VULKAN）
SDL_Window *window = SDL_CreateWindow("test", 100, 100,
    WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_VULKAN);
#endif
```

**资源路径**（第 66-81 行）

```cpp
#ifdef __ANDROID__
std::filesystem::path currentPath("");   // Android 用相对路径，AssetManager 解析
#else
// 保留原有的向上遍历目录查找 shaders/ 的逻辑
std::filesystem::path currentPath = std::filesystem::current_path();
while (!std::filesystem::exists(currentPath / "shaders")) { ... }
#endif
```

**触控输入**（第 166-194 行，事件循环内新增）

```cpp
#ifdef __ANDROID__
// 单指拖拽 → 摄像机旋转
else if (e.type == SDL_FINGERMOTION) {
    float dx = e.tfinger.dx * WINDOW_WIDTH;
    float dy = e.tfinger.dy * WINDOW_HEIGHT;
    if (abs(dx) > abs(dy)) {
        currentYDegree += dx * rotateSpeed * 0.5f;
        gpuScene.GetMainCamera()->RotateY(currentYDegree);
    } else {
        currentZDegree += dy * rotateSpeed * 0.5f;
        gpuScene.GetMainCamera()->RotateZ(currentZDegree);
    }
}
else if (e.type == SDL_FINGERDOWN) {
    currentZDegree = 0;
    currentYDegree = 0;
}
// 双指捏合 → 摄像机缩放
else if (e.type == SDL_MULTIGESTURE) {
    if (e.mgesture.numFingers == 2) {
        float pinchDist = e.mgesture.dDist;
        gpuScene.GetMainCamera()->MoveForward/Backward(moveSpeed * |pinchDist| * 20.0f);
    }
}
#endif
```

---

### 7. `Src/Light.cpp`

**新增头文件**（第 3 行）

```cpp
#include "VulkanCompat.h"    // 使 vkCmdPipelineBarrier2 在 Android 上可用
```

`Light.cpp` 中有 4 处 `vkCmdPipelineBarrier2` 调用（光照剔除计算着色器之间的同步），通过 `VulkanCompat.h` 中的 inline 兼容函数自动转换为 `vkCmdPipelineBarrier`。

---

### 8. `Src/CMakeLists.txt`

**新增源文件**（第 31 行）

```cmake
set(SOURCES
    ...
    ${CMAKE_CURRENT_LIST_DIR}/AssetLoader.cpp    # 新增
)
```

桌面构建不受影响，AssetLoader 在非 Android 平台走 `std::ifstream` 路径。

---

## 编译过程中发现并修复的问题

| 问题 | 原因 | 修复 |
|------|------|------|
| NDK 版本不匹配 | `gradle.properties` 中指定的版本未安装 | 改为本地已有的 `27.2.12479018` |
| Asset 压缩 OOM | `debug1.bin` 约 800MB，Gradle 默认堆不够 | JVM 堆增至 4G，`.bin/.spv/.scene` 标记为不压缩 |
| `constexpr std::vector` 编译失败 | NDK clang 不支持 `constexpr` 返回 `std::vector` | 移除 `getRequiredExtensions()` 的 `constexpr` 修饰符 |
| `vkCmdPipelineBarrier2` / `vkCmdDrawIndexedIndirectCount` 链接失败 | Android `libvulkan.so`（API 28）仅导出 Vulkan 1.1 符号 | 新建 `VulkanCompat.h` 提供 inline 兼容实现 |

---

## 构建与安装

```bash
cd android
./gradlew assembleDebug
# APK 输出: app/build/outputs/apk/debug/app-debug.apk

adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## 验证要点

1. APK 正常生成（`./gradlew assembleDebug` 成功）
2. 安装后启动，Vulkan surface 创建正常
3. 触控操作：单指拖拽旋转摄像机，双指捏合缩放
4. 桌面端构建不受影响（所有改动均在 `#ifdef __ANDROID__` 守卫内）
