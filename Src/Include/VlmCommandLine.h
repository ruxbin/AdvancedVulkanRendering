#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// VLM 命令行(Window.cpp main 解析,传入 GpuScene ctor)。
// 体积语法:
//   --vlm-bake-volume camera,sx,sy,sz      以 scene.scene 的 camera_position 为中心、尺寸 sx×sy×sz
//   --vlm-bake-volume x0,y0,z0,x1,y1,z1    显式 AABB
struct VlmCommandLine {
  bool bakeRequested = false;        // --vlm-bake <path>
  std::string bakeOutPath;
  std::string bakeVolume;            // --vlm-bake-volume <spec>(必需)
  float bakeSpacing = 1.0f;          // --vlm-bake-spacing <m>
  uint32_t bakeSamples = 2048;       // --vlm-bake-samples <N>
  uint32_t bakeBatchSamples = 32;    // --vlm-bake-batch-samples <N>(单次 dispatch 每 probe 样本数)
  uint32_t bakeMaxBounces = 6;       // --vlm-bake-max-bounces <N>
  bool bakeSkyOnly = false;          // --vlm-bake-sky-only(无几何,纯环境投影)
  bool bakeConstEnv = false;         // --vlm-bake-const-env r,g,b
  float constEnvRGB[3] = {1, 1, 1};
  bool sunIsEnvironment = false;     // --vlm-sun-mode environment(默认 analytic)
  float sunScale = 1.0f;             // --vlm-sun-scale
  float envScale = 1.0f;             // --vlm-env-scale
  float localLightScale = 1.0f;      // --vlm-local-light-scale
  bool exitAfterBake = false;        // --vlm-exit-after-bake
  std::string loadPath;              // --vlm <path>(启动时加载资产)
};

// 只负责识别 VLM 参数并前移消费索引;缺值的 flag 返回 ""(调用方判空)。
inline bool ParseVlmCommandLine(int argc, char** argv, VlmCommandLine& out) {
  auto parse3f = [](const char* s, float v[3]) {
    return std::sscanf(s, "%f,%f,%f", &v[0], &v[1], &v[2]) == 3;
  };
  for (int i = 1; i < argc; ++i) {
    auto next = [&](const char*) -> const char* {
      return (i + 1 < argc) ? argv[++i] : "";
    };
    if (!std::strcmp(argv[i], "--vlm-bake")) { out.bakeRequested = true; out.bakeOutPath = next("--vlm-bake"); }
    else if (!std::strcmp(argv[i], "--vlm")) out.loadPath = next("--vlm");
    else if (!std::strcmp(argv[i], "--vlm-bake-volume")) out.bakeVolume = next("--vlm-bake-volume");
    else if (!std::strcmp(argv[i], "--vlm-bake-spacing")) out.bakeSpacing = (float)atof(next("--vlm-bake-spacing"));
    else if (!std::strcmp(argv[i], "--vlm-bake-samples")) out.bakeSamples = (uint32_t)atoi(next("--vlm-bake-samples"));
    else if (!std::strcmp(argv[i], "--vlm-bake-batch-samples")) out.bakeBatchSamples = (uint32_t)atoi(next("--vlm-bake-batch-samples"));
    else if (!std::strcmp(argv[i], "--vlm-bake-max-bounces")) out.bakeMaxBounces = (uint32_t)atoi(next("--vlm-bake-max-bounces"));
    else if (!std::strcmp(argv[i], "--vlm-bake-sky-only")) out.bakeSkyOnly = true;
    else if (!std::strcmp(argv[i], "--vlm-bake-const-env")) { out.bakeConstEnv = true; parse3f(next("--vlm-bake-const-env"), out.constEnvRGB); }
    else if (!std::strcmp(argv[i], "--vlm-sun-mode")) { const char* m = next("--vlm-sun-mode"); out.sunIsEnvironment = !std::strcmp(m, "environment"); }
    else if (!std::strcmp(argv[i], "--vlm-sun-scale")) out.sunScale = (float)atof(next("--vlm-sun-scale"));
    else if (!std::strcmp(argv[i], "--vlm-env-scale")) out.envScale = (float)atof(next("--vlm-env-scale"));
    else if (!std::strcmp(argv[i], "--vlm-local-light-scale")) out.localLightScale = (float)atof(next("--vlm-local-light-scale"));
    else if (!std::strcmp(argv[i], "--vlm-exit-after-bake")) out.exitAfterBake = true;
  }
  return true;
}
