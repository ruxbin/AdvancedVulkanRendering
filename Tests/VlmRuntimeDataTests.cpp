#include "VlmRuntimeData.h"
#include "VlmSh.h"
#include <cmath>
#include <cstdio>
int main() {
  VlmAssetData a;
  a.cells[0]=a.cells[1]=a.cells[2]=1;
  a.step[0]=a.step[1]=a.step[2]=2;
  a.bandWidth=1; a.samplesPerProbe=64;
  a.validity.assign(8,1); a.validity[3]=0;
  a.shFp16.assign(8*28,0);
  a.shFp16[0]=VlmFloatToHalf(-2.0f); a.shFp16[26]=VlmFloatToHalf(3.0f);
  VlmRuntimeData data;
  if (!VlmPrepareRuntimeData(a,data) || data.sh.size()!=8*28 || data.params.probeTotal!=8 ||
      data.sh[0]!=-2 || data.sh[26]!=3 || data.sh[27]!=1 || data.sh[3*28+27]!=0 || data.params.invStep[0]!=.5f) return 1;
  a.shFp16[1]=0x7c00;
  if (VlmPrepareRuntimeData(a,data)) return 1;
  std::puts("runtime packing, validity and non-finite rejection passed");
}
