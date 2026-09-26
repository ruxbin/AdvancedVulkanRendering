#pragma once

// 从 equirectangular HDR 像素积分出 9 个"浓缩形式"球谐系数,
// 形式与 Apple AAPLLightingCommon.h evaluateShCoefficients 的硬编码常数一致:
//   E(n) = c0 + c1*y + c2*z + c3*x + c4*yx + c5*yz + c6*(3z^2-1) + c7*zx + c8*(x^2-y^2)
// 即 A_l(带权重 π, 2π/3, 2π/3, 2π/3, π/4×5)与 Y_lm 归一化常数已折入 c[i]。
struct SH9 {
  float c[9][3];
};

// rgba: width*height*4 的 float 像素(stbi_loadf 的 STBI_rgb_alpha 输出)。
// 方向约定(必须与 shaders/ibl.hlsl 的 DirectionToEquirectUV 逐字一致):
//   u = atan2(dir.x, dir.z) / (2π) + 0.5
//   v = acos(dir.y) / π            (v=0 端 = +Y)
// 逆映射:φ=(u-0.5)*2π,θ=v*π;dir=(sinθ sinφ, cosθ, sinθ cosφ)
SH9 ComputeSH9FromEquirect(const float* rgba, int width, int height);
