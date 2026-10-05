#pragma once
#include "VlmAsset.h"
#include "VlmSh.h"
#include <cstddef>
#include <vector>

struct VlmParams {
  float boundsMin[3], bandWidth;
  float step[3], normalBias;
  float invStep[3], normalBiasMax;
  uint32_t cells[3], probeTotal;
};
static_assert(sizeof(VlmParams)==64);
static_assert(offsetof(VlmParams, cells)==48);
struct VlmRuntimeData { VlmParams params{}; std::vector<float> sh; };
inline bool VlmPrepareRuntimeData(const VlmAssetData& a, VlmRuntimeData& out) {
  if (!VlmValidateAsset(a)) return false;
  VlmRuntimeData d;
  for (int i=0;i<3;++i) { d.params.boundsMin[i]=a.bmin[i]; d.params.step[i]=a.step[i]; d.params.invStep[i]=1/a.step[i]; d.params.cells[i]=a.cells[i]; }
  d.params.probeTotal=static_cast<uint32_t>(a.ProbeCount());
  d.params.bandWidth=a.bandWidth;
  // Bias defaults to zero until scene-unit and thin-wall tests justify an offset.
  d.params.normalBias=0; d.params.normalBiasMax=0;
  d.sh.resize(a.shFp16.size());
  for(size_t i=0;i<d.sh.size();++i) d.sh[i]=i%28==27 ? float(a.validity[i/28]) : VlmHalfToFloat(a.shFp16[i]);
  out=std::move(d);
  return true;
}
