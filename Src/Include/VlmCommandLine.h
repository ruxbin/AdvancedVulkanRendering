#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cmath>
#include <cerrno>
#include <limits>

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
  bool sunIsEnvironment = true;      // bundled HDR has a sun disk; analytic requires a disk-free sky
  float sunScale = 1.0f;             // --vlm-sun-scale
  float envScale = 1.0f;             // --vlm-env-scale
  float localLightScale = 1.0f;      // --vlm-local-light-scale
  bool exitAfterBake = false;        // --vlm-exit-after-bake
  uint32_t validationFrames = 0;     // --vlm-validation-frames N: bounded renderer smoke run
  std::string loadPath;              // --vlm <path>(启动时加载资产)
  std::string error;
};

// 只负责识别 VLM 参数并前移消费索引;缺值的 flag 返回 ""(调用方判空)。
inline bool ParseVlmCommandLine(int argc, char** argv, VlmCommandLine& out) {
  bool valid = true;
  auto parse3f = [&](const char* s, float v[3]) {
    int consumed = 0;
    if (std::sscanf(s, "%f,%f,%f%n", &v[0], &v[1], &v[2], &consumed) != 3 ||
        s[consumed] != '\0') valid = false;
    for (int c = 0; c < 3; ++c) if (!std::isfinite(v[c]) || v[c] < 0) valid = false;
  };
  auto count = [&](const char* s, uint32_t maximum) {
    char* end = nullptr; errno = 0;
    const auto n = std::strtoull(s, &end, 10);
    if (*s < '0' || *s > '9' || end == s || *end || errno || n == 0 || n > maximum) valid = false;
    return static_cast<uint32_t>(n);
  };
  auto number = [&](const char* s, bool positive) {
    char* end = nullptr; errno = 0;
    float n = std::strtof(s, &end);
    if (end == s || *end || errno || !std::isfinite(n) || n < 0 || (positive && n == 0)) valid = false;
    return n;
  };
  for (int i = 1; i < argc; ++i) {
    auto next = [&](const char*) -> const char* {
      if (i + 1 >= argc || std::strncmp(argv[i + 1], "--", 2) == 0 || !*argv[i + 1]) {
        valid = false; return "";
      }
      return argv[++i];
    };
    if (!std::strcmp(argv[i], "--vlm-bake")) { out.bakeRequested = true; out.bakeOutPath = next("--vlm-bake"); }
    else if (!std::strcmp(argv[i], "--vlm")) out.loadPath = next("--vlm");
    else if (!std::strcmp(argv[i], "--vlm-bake-volume")) out.bakeVolume = next("--vlm-bake-volume");
    else if (!std::strcmp(argv[i], "--vlm-bake-spacing")) out.bakeSpacing = number(next("spacing"), true);
    else if (!std::strcmp(argv[i], "--vlm-bake-samples")) out.bakeSamples = count(next("samples"), 16777216u);
    else if (!std::strcmp(argv[i], "--vlm-bake-batch-samples")) out.bakeBatchSamples = count(next("batch"), 4096u);
    else if (!std::strcmp(argv[i], "--vlm-bake-max-bounces")) out.bakeMaxBounces = count(next("bounces"), 32u);
    else if (!std::strcmp(argv[i], "--vlm-bake-sky-only")) out.bakeSkyOnly = true;
    else if (!std::strcmp(argv[i], "--vlm-bake-const-env")) { out.bakeConstEnv = true; parse3f(next("--vlm-bake-const-env"), out.constEnvRGB); }
    else if (!std::strcmp(argv[i], "--vlm-sun-mode")) { const char* m = next("sun-mode"); out.sunIsEnvironment = !std::strcmp(m, "environment"); if (std::strcmp(m, "analytic") && !out.sunIsEnvironment) valid = false; }
    else if (!std::strcmp(argv[i], "--vlm-sun-scale")) out.sunScale = number(next("sun-scale"), false);
    else if (!std::strcmp(argv[i], "--vlm-env-scale")) out.envScale = number(next("env-scale"), false);
    else if (!std::strcmp(argv[i], "--vlm-local-light-scale")) out.localLightScale = number(next("local-scale"), false);
    else if (!std::strcmp(argv[i], "--vlm-exit-after-bake")) out.exitAfterBake = true;
    else if (!std::strcmp(argv[i], "--vlm-validation-frames")) out.validationFrames = count(next("validation-frames"),10000u);
    else if (!std::strncmp(argv[i], "--vlm", 5)) valid = false;
    if (!valid) { out.error = "Invalid or missing VLM argument near " + std::string(argv[i]); return false; }
  }
  if (out.bakeRequested && (out.bakeOutPath.empty() || out.bakeVolume.empty())) {
    out.error = "--vlm-bake requires an output path and --vlm-bake-volume"; return false;
  }
  return true;
}
