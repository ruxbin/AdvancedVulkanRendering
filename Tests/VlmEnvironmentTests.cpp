#include "VlmEnvironment.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

int main() {
  int failures=0;
  auto expect=[&](bool value,const char* what) { if(!value) { std::printf("FAIL: %s\n",what); ++failures; } };
  std::vector<float> image(8*4*4,1);
  VlmEnvironmentDistribution distribution;
  expect(VlmBuildEnvironmentDistribution(image.data(),8,4,distribution),"constant environment");
  expect(distribution.cdf.size()==32 && distribution.cdf.back()==1,"normalized distribution");
  double rowMass[4]{};
  for(unsigned i=0;i<32;++i) {
    double p=distribution.cdf[i]-(i?distribution.cdf[i-1]:0);
    expect(p>=0,"monotonic CDF"); rowMass[i/8]+=p;
  }
  expect(std::abs(rowMass[0]-(1-std::cos(3.141592653589793/4))*.5)<1e-6,"polar row has correct solid angle");
  expect(std::abs(rowMass[0]-rowMass[3])<1e-6 && std::abs(rowMass[1]-rowMass[2])<1e-6,"spherical symmetry");
  for(float& f:image) f=0;
  expect(VlmBuildEnvironmentDistribution(image.data(),8,4,distribution),"black sky falls back to uniform sphere");
  image[0]=std::numeric_limits<float>::infinity();
  expect(!VlmBuildEnvironmentDistribution(image.data(),8,4,distribution),"invalid radiance rejected");
  expect(!VlmBuildEnvironmentDistribution(nullptr,8,4,distribution),"missing pixels rejected");
  return failures ? 1 : 0;
}
