#pragma once

// 多项式形式的 SH9 系数，求值结构与 Apple evaluateShCoefficients 一致:
//   E(n) = c0 + c1*y + c2*z + c3*x + c4*yx + c5*yz + c6*(3z^2-1) + c7*zx + c8*(x^2-y^2)
// 系数的幅度/方向约定由下面的投影函数决定。
struct SH9 {
  float c[9][3];
};

// —— VLM 共享物理约定(值与 ComputeSH9FromEquirect 内部常量逐字相同,行为不变)——
// kSh9Y:实数 SH 基归一化常数;kSh9A:辐照度卷积权重(π, 2π/3×3, π/4×5)。
inline constexpr float kSh9Y[9] = {
    0.282095f,
    0.488603f, 0.488603f, 0.488603f,
    1.092548f, 1.092548f, 0.315392f, 1.092548f, 0.546274f,
};
inline constexpr float kSh9A[9] = {
    3.1415926535897932f,
    2.0943951023931953f, 2.0943951023931953f, 2.0943951023931953f,
    0.7853981633974483f, 0.7853981633974483f, 0.7853981633974483f,
    0.7853981633974483f, 0.7853981633974483f,
};
// 逐方向基求值,顺序:常量、y、z、x、yx、yz、3z²-1、zx、x²-y²(kSh9Y 已折入)。
inline void EvalSh9Basis(float dx, float dy, float dz, float out[9]) {
  out[0] = kSh9Y[0];
  out[1] = kSh9Y[1] * dy;
  out[2] = kSh9Y[2] * dz;
  out[3] = kSh9Y[3] * dx;
  out[4] = kSh9Y[4] * dy * dx;
  out[5] = kSh9Y[5] * dy * dz;
  out[6] = kSh9Y[6] * (3.0f * dz * dz - 1.0f);
  out[7] = kSh9Y[7] * dz * dx;
  out[8] = kSh9Y[8] * (dx * dx - dy * dy);
}

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
