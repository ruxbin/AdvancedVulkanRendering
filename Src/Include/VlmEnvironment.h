#pragma once
#include <cstdint>
#include <vector>

// Probability CDF over equirectangular cells. Within a cell sample solid angle,
// not UV area. The shader evaluates PDF from these exact rounded CDF differences.
struct VlmEnvironmentDistribution {
  uint32_t width=0, height=0;
  std::vector<float> cdf;
};
bool VlmBuildEnvironmentDistribution(const float* rgba, uint32_t width, uint32_t height,
                                     VlmEnvironmentDistribution& out);
