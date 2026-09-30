#pragma once

// 多项式形式的 SH9 系数，求值结构与 Apple evaluateShCoefficients 一致:
//   E(n) = c0 + c1*y + c2*z + c3*x + c4*yx + c5*yz + c6*(3z^2-1) + c7*zx + c8*(x^2-y^2)
// 系数的幅度/方向约定由下面的投影函数决定。
struct SH9 {
  float c[9][3];
};

// rgba: width*height*4 的 float 像素(stbi_loadf 的 STBI_rgb_alpha 输出)。
// 标准物理辐照度：A_l(π, 2π/3×3, π/4×5)与 Y_lm 常数折入 c[i]。
// 方向约定(必须与 shaders/ibl.hlsl 的 DirectionToEquirectUV 逐字一致):
//   u = atan2(-dir.x, dir.z) / (2π) + 0.5
//   v = acos(dir.y) / π            (v=0 端 = +Y)
// 逆映射:φ=(u-0.5)*2π,θ=v*π;dir=(-sinθ sinφ, cosθ, sinθ cosφ)
// -dir.x 的方位角镜像是对 Apple 烘焙 KTX(san_giuseppe_bridge_4k_ibl.ktx)的
// 实证校准:镜像后环境太阳与 bistro scene.scene 的 sun_direction 同侧(-X)。
SH9 ComputeSH9FromEquirect(const float* rgba, int width, int height);

// Apple sample compatibility: bound source radiance to [0,256], divide the
// physical polynomial coefficients by pi*Y_i, then reflect Z (terms 2/5/7).
// The resulting polynomial is evaluated at +N; it reproduces the sample's
// coefficient/evaluation convention, not standard physical irradiance.
// Keep the standard physical irradiance API above unchanged.
SH9 ComputeMetalSH9FromEquirect(const float* rgba, int width, int height);
