#include "VlmEnvironment.h"
#include <algorithm>
#include <cmath>

bool VlmBuildEnvironmentDistribution(const float* rgba,uint32_t width,uint32_t height,
                                     VlmEnvironmentDistribution& out) {
  out={};
  const uint64_t count=uint64_t(width)*height;
  if(!rgba || !width || !height || count>16777216) return false;
  std::vector<double> luminance(count),mass(count);
  bool black=true;
  for(size_t i=0;i<count;++i) {
    for(int c=0;c<3;++c) if(!std::isfinite(rgba[i*4+c]) || rgba[i*4+c]<0) return false;
    luminance[i]=.2126*rgba[i*4]+.7152*rgba[i*4+1]+.0722*rgba[i*4+2];
    black &= luminance[i]==0;
  }
  constexpr double pi=3.14159265358979323846;
  constexpr double kernel[3]={.125,.75,.125};
  double total=0;
  for(uint32_t y=0;y<height;++y) {
    // Stable polar-cell area (avoids subtracting nearly equal cosines).
    const double area=4*pi/width*std::sin(pi*(y+.5)/height)*std::sin(pi/(2*height));
    for(uint32_t x=0;x<width;++x) {
      double value=black ? 1 : 0;
      if(!black) for(int j=-1;j<=1;++j) for(int k=-1;k<=1;++k) {
        auto yy=std::clamp(int(y)+j,0,int(height)-1);
        auto xx=(int(x)+k+int(width))%int(width);
        value+=kernel[j+1]*kernel[k+1]*luminance[size_t(yy)*width+xx];
      }
      total+=(mass[size_t(y)*width+x]=value*area);
    }
  }
  if(!(total>0) || !std::isfinite(total)) return false;
  out.width=width; out.height=height; out.cdf.resize(count);
  double prefix=0;
  for(size_t i=0;i<count;++i) out.cdf[i]=float((prefix+=mass[i])/total);
  out.cdf.back()=1; // exact normalization for binary search and PDF differences
  return true;
}
