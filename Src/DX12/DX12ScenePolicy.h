#pragma once

#include "Matrix.h"

namespace DX12ScenePolicy {

struct ScatterAvailability {
  bool scatterVolumePSO = false;
  bool accumulatePSO = false;
  bool scatterRootSignature = false;
  bool accumulateRootSignature = false;
  bool scatterVolume = false;
  bool scatterAccumVolume = false;
};

inline vec3 CameraUp(const vec3& sceneUp) {
  return sceneUp * -1.0f;
}

inline bool IsScatterReady(const ScatterAvailability& availability) {
  return availability.scatterVolumePSO &&
         availability.accumulatePSO &&
         availability.scatterRootSignature &&
         availability.accumulateRootSignature &&
         availability.scatterVolume &&
         availability.scatterAccumVolume;
}

inline float EffectiveScatterScale(float requestedScale, bool scatterReady) {
  return scatterReady ? requestedScale : 0.0f;
}

} // namespace DX12ScenePolicy
