#include "DX12ScenePolicy.h"

#include <cmath>
#include <iostream>

namespace {

int failures = 0;

void expectNear(float actual, float expected, const char* message) {
  if (std::fabs(actual - expected) > 1e-6f) {
    std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
    ++failures;
  }
}

void expectTrue(bool actual, const char* message) {
  if (!actual) {
    std::cerr << message << ": expected true\n";
    ++failures;
  }
}

void expectFalse(bool actual, const char* message) {
  if (actual) {
    std::cerr << message << ": expected false\n";
    ++failures;
  }
}

} // namespace

int main() {
  const vec3 sceneUp(-0.008532071f, 0.999956667f, 0.003697905f);
  const vec3 cameraUp = DX12ScenePolicy::CameraUp(sceneUp);
  expectNear(cameraUp.x, 0.008532071f, "camera up x must match Vulkan conversion");
  expectNear(cameraUp.y, -0.999956667f, "camera up y must match Vulkan conversion");
  expectNear(cameraUp.z, -0.003697905f, "camera up z must match Vulkan conversion");

  DX12ScenePolicy::ScatterAvailability availability{};
  expectFalse(DX12ScenePolicy::IsScatterReady(availability),
              "scatter must be unavailable when resources are absent");

  availability.scatterVolumePSO = true;
  availability.accumulatePSO = true;
  availability.scatterRootSignature = true;
  availability.accumulateRootSignature = true;
  availability.scatterVolume = true;
  availability.scatterAccumVolume = true;
  expectTrue(DX12ScenePolicy::IsScatterReady(availability),
             "scatter must be ready only when every dependency exists");

  struct ScatterDependency {
    const char* name;
    bool DX12ScenePolicy::ScatterAvailability::*member;
  };
  const ScatterDependency dependencies[] = {
      {"scatter volume PSO", &DX12ScenePolicy::ScatterAvailability::scatterVolumePSO},
      {"accumulate PSO", &DX12ScenePolicy::ScatterAvailability::accumulatePSO},
      {"scatter root signature", &DX12ScenePolicy::ScatterAvailability::scatterRootSignature},
      {"accumulate root signature", &DX12ScenePolicy::ScatterAvailability::accumulateRootSignature},
      {"scatter volume", &DX12ScenePolicy::ScatterAvailability::scatterVolume},
      {"scatter accumulation volume", &DX12ScenePolicy::ScatterAvailability::scatterAccumVolume},
  };
  for (const auto& dependency : dependencies) {
    availability.*(dependency.member) = false;
    if (DX12ScenePolicy::IsScatterReady(availability)) {
      std::cerr << "missing " << dependency.name << " must disable scatter\n";
      ++failures;
    }
    availability.*(dependency.member) = true;
  }

  expectNear(DX12ScenePolicy::EffectiveScatterScale(0.75f, false), 0.0f,
             "invalid scatter resources must upload a disabled scale");
  expectNear(DX12ScenePolicy::EffectiveScatterScale(0.75f, true), 0.75f,
             "valid scatter resources must preserve the requested scale");

  return failures == 0 ? 0 : 1;
}
