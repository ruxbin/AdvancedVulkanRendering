#include "IBLGenerator.h"

#include "Common.h"       // readFile
#include "KtxTexture.h"   // KTX1 解析(Apple 烘焙 cube 对拍)
#include "IBLComparison.h"
#include "SphericalHarmonics.h" // SH9 计算(SH E(n) 对拍)
#include "VulkanSetup.h"  // VulkanDevice

#include <spdlog/spdlog.h>

// STB_IMAGE_IMPLEMENTATION 已在 GpuScene.cpp 定义;此处仅声明。
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {

VkShaderModule loadModule(VkDevice dev, const std::filesystem::path& path) {
  std::vector<char> code = readFile(path.generic_string());
  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = code.size();
  ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
  VkShaderModule m;
  if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create shader module");
  return m;
}

void createImage2D(const VulkanDevice& device, VkFormat format, uint32_t w, uint32_t h,
                   uint32_t mips, uint32_t layers, VkImageCreateFlags flags,
                   VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory) {
  VkImageCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.extent = {w, h, 1};
  ci.mipLevels = mips;
  ci.arrayLayers = layers;
  ci.format = format;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  ci.usage = usage;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.flags = flags;
  if (vkCreateImage(device.getLogicalDevice(), &ci, nullptr, &image) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create image");
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(device.getLogicalDevice(), image, &mr);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = device.findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(device.getLogicalDevice(), &ai, nullptr, &memory) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to allocate image memory");
  vkBindImageMemory(device.getLogicalDevice(), image, memory, 0);
}

void transitionRange(VkCommandBuffer cmd, VkImage image,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     uint32_t baseMip, uint32_t mipCount, uint32_t layerCount,
                     VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                     VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.oldLayout = oldLayout;
  b.newLayout = newLayout;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, layerCount};
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

float halfToFloat(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t mant = h & 0x3FF;
  uint32_t f;
  if (exp == 0) {
    // 次正规:value = mant/1024 * 2^-14
    float v = (float)mant / 1024.0f * 6.103515625e-05f;
    std::memcpy(&f, &v, 4);
    f |= sign;
  } else if (exp == 31) {
    f = sign | 0x7F800000u | (mant << 13);
  } else {
    f = sign | ((exp + 112) << 23) | (mant << 13);
  }
  float out;
  std::memcpy(&out, &f, 4);
  return out;
}

void createHostBuffer(const VulkanDevice& device, VkDeviceSize size, VkBufferUsageFlags usage,
                      VkBuffer& buffer, VkDeviceMemory& memory) {
  VkBufferCreateInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bi.size = size;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device.getLogicalDevice(), &bi, nullptr, &buffer) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create buffer");
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(device.getLogicalDevice(), buffer, &mr);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = device.findMemoryType(
      mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (vkAllocateMemory(device.getLogicalDevice(), &ai, nullptr, &memory) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to allocate buffer memory");
  vkBindBufferMemory(device.getLogicalDevice(), buffer, memory, 0);
}

// ---- CPU-vs-GPU cube chain validation(Ruling 9/10:RenderDoc 替代)----

struct Vec3 { float x, y, z; };

Vec3 normalizeVec3(Vec3 v) {
  const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  const float inv = len > 0.0f ? 1.0f / len : 0.0f;
  return {v.x * inv, v.y * inv, v.z * inv};
}

constexpr float kPiF = 3.1415926535897932f;

// 与 shaders/ibl.hlsl CubeFaceDirection 逐字一致(含 normalize)。
Vec3 cubeFaceDirectionCpu(uint32_t face, float u, float v) {
  float sc = u * 2.0f - 1.0f;
  float tc = v * 2.0f - 1.0f;
  Vec3 d;
  switch (face) {
    case 0:  d = { 1.0f, -tc, -sc }; break;
    case 1:  d = { -1.0f, -tc,  sc }; break;
    case 2:  d = {  sc, 1.0f,  tc }; break;
    case 3:  d = {  sc, -1.0f, -tc }; break;
    case 4:  d = {  sc, -tc, 1.0f }; break;
    default: d = { -sc, -tc, -1.0f }; break;
  }
  float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  return {d.x / len, d.y / len, d.z / len};
}

// CubeFaceDirection 的逆映射:主轴定面(Vulkan/GL 约定),面内坐标反解 sc/tc。
uint32_t faceForDirection(Vec3 d, float& sc, float& tc) {
  float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
  if (ax >= ay && ax >= az) {
    if (d.x > 0.0f) { sc = -d.z / ax; tc = -d.y / ax; return 0; }
    sc = d.z / ax; tc = -d.y / ax; return 1;
  }
  if (ay >= ax && ay >= az) {
    if (d.y > 0.0f) { sc = d.x / ay; tc = d.z / ay; return 2; }
    sc = d.x / ay; tc = -d.z / ay; return 3;
  }
  if (d.z > 0.0f) { sc = d.x / az; tc = -d.y / az; return 4; }
  sc = -d.x / az; tc = -d.y / az; return 5;
}

// 与 DirectionToEquirectUV 逐字一致:u = atan2(-x,z)/(2π)+0.5,v = acos(y)/π。
// (方位角镜像 = 对 Apple 烘焙 KTX 的实证校准,见 SphericalHarmonics.h 约定注释)
void directionToEquirectUV(Vec3 d, float& u, float& v) {
  float phi = std::atan2(-d.x, d.z);
  float cy = d.y < -1.0f ? -1.0f : (d.y > 1.0f ? 1.0f : d.y);
  float theta = std::acos(cy);
  u = phi / (2.0f * kPiF) + 0.5f;
  v = theta / kPiF;
}

// 复刻 CS1 采样器行为:bilinear + CLAMP_TO_EDGE(坐标先 clamp 到 [0.5, n-0.5] texel)。
Vec3 sampleEquirectBilinear(const float* px, int w, int h, float u, float v) {
  auto texelCoord = [](float t, int n, int& i0, int& i1, float& f) {
    float c = t * (float)n;
    c = c < 0.5f ? 0.5f : (c > (float)n - 0.5f ? (float)n - 0.5f : c);
    c -= 0.5f;
    i0 = (int)std::floor(c);
    f = c - (float)i0;
    i1 = i0 + 1 < n ? i0 + 1 : n - 1;
  };
  int x0, x1, y0, y1;
  float fx, fy;
  texelCoord(u, w, x0, x1, fx);
  texelCoord(v, h, y0, y1, fy);
  const float* p00 = px + ((size_t)y0 * w + x0) * 4;
  const float* p01 = px + ((size_t)y0 * w + x1) * 4;
  const float* p10 = px + ((size_t)y1 * w + x0) * 4;
  const float* p11 = px + ((size_t)y1 * w + x1) * 4;
  Vec3 out;
  float* o = &out.x;
  for (int c = 0; c < 3; ++c) {
    float top = p00[c] + (p01[c] - p00[c]) * fx;
    float bot = p10[c] + (p11[c] - p10[c]) * fx;
    o[c] = top + (bot - top) * fy;
  }
  return out;
}

// CPU-vs-GPU cube 链校验:
// 1) stb 重载 equirect;2) ~50 个确定性方向(fibonacci 球 + 6 轴)上,
//    CPU 按 shader 同公式 bilinear 采样,GPU mip0 点采样同 texel,比相对误差;
// 3) mip8(1×1×6)有限性 + 均值与 mip0 均值 ±10% 对比。超差 warn(不 throw)。
void validateCubeChainAgainstCpu(const VulkanDevice& device, VkImage envCube,
                                 const std::filesystem::path& hdrPath) {
  VkDevice dev = device.getLogicalDevice();
  constexpr uint32_t kSize = IBLGenerator::kEnvMapSize;          // 256
  constexpr uint32_t kLastMip = IBLGenerator::kEnvMipCount - 1;  // 8
  constexpr VkDeviceSize kMip0Bytes = (VkDeviceSize)kSize * kSize * 6 * 8; // RGBA16F
  constexpr VkDeviceSize kMip8Bytes = 6 * 8;
  constexpr VkDeviceSize kTotalBytes = kMip0Bytes + kMip8Bytes;

  // 1) CPU 侧 equirect
  int w = 0, h = 0, ch = 0;
  float* pixels = stbi_loadf(hdrPath.generic_string().c_str(), &w, &h, &ch, STBI_rgb_alpha);
  if (!pixels || w <= 0 || h <= 0) {
    spdlog::warn("IBL: cube-chain validation skipped, cannot reload {}", hdrPath.generic_string());
    if (pixels) stbi_image_free(pixels);
    return;
  }

  // 2) GPU 回读 mip0 + mip8(SHADER_READ_ONLY → TRANSFER_SRC → copy → 转回)
  VkBuffer staging; VkDeviceMemory stagingMem;
  createHostBuffer(device, kTotalBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, staging, stagingMem);
  VkCommandBuffer cmd = device.beginSingleTimeCommands();
  transitionRange(cmd, envCube, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, IBLGenerator::kEnvMipCount, 6,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy regions[2] = {};
  regions[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6};
  regions[0].imageExtent = {kSize, kSize, 1};
  regions[1].bufferOffset = kMip0Bytes; // 3145728,4 字节对齐满足
  regions[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, kLastMip, 0, 6};
  regions[1].imageExtent = {1, 1, 1};
  vkCmdCopyImageToBuffer(cmd, envCube, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 2, regions);
  transitionRange(cmd, envCube, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, IBLGenerator::kEnvMipCount, 6,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  device.endSingleTimeCommands(cmd);

  const uint16_t* gpu = nullptr;
  vkMapMemory(dev, stagingMem, 0, kTotalBytes, 0, (void**)&gpu);

  auto gpuTexel = [&](uint32_t mipBase, uint32_t face, uint32_t x, uint32_t y, uint32_t size,
                      float out[3]) {
    size_t idx = (size_t)mipBase + (((size_t)face * size + y) * size + x) * 4;
    for (int c = 0; c < 3; ++c) out[c] = halfToFloat(gpu[idx + c]);
  };

  // mip0 均值(供 mip8 对比)
  double mean0[3] = {};
  for (uint32_t f = 0; f < 6; ++f)
    for (uint32_t y = 0; y < kSize; ++y)
      for (uint32_t x = 0; x < kSize; ++x) {
        float t[3];
        gpuTexel(0, f, x, y, kSize, t);
        for (int c = 0; c < 3; ++c) mean0[c] += t[c];
      }
  for (int c = 0; c < 3; ++c) mean0[c] /= (double)(kSize * kSize * 6);

  // 3) ~50 个确定性方向:48 fibonacci 球 + 6 个轴方向(保证每面覆盖)
  std::vector<Vec3> dirs;
  constexpr uint32_t kFibCount = 48;
  for (uint32_t i = 0; i < kFibCount; ++i) {
    float t = ((float)i + 0.5f) / (float)kFibCount;
    float y = 1.0f - 2.0f * t;
    float r = std::sqrt(1.0f - y * y > 0.0f ? 1.0f - y * y : 0.0f);
    float phi = 2.39996323f * (float)i; // 黄金角
    dirs.push_back({r * std::cos(phi), y, r * std::sin(phi)});
  }
  dirs.push_back({1, 0, 0}); dirs.push_back({-1, 0, 0});
  dirs.push_back({0, 1, 0}); dirs.push_back({0, -1, 0});
  dirs.push_back({0, 0, 1}); dirs.push_back({0, 0, -1});

  double maxRelErr = 0.0;
  uint32_t worstFace = 0, worstX = 0, worstY = 0;
  for (const Vec3& d : dirs) {
    float sc, tc;
    uint32_t face = faceForDirection(d, sc, tc);
    float u01 = (sc + 1.0f) * 0.5f, v01 = (tc + 1.0f) * 0.5f;
    uint32_t x = (uint32_t)(u01 * kSize); if (x >= kSize) x = kSize - 1;
    uint32_t y = (uint32_t)(v01 * kSize); if (y >= kSize) y = kSize - 1;
    // 半 texel 内的梯度噪声不属于实现错误:用落点 texel 中心方向做 CPU 期望。
    Vec3 dc = cubeFaceDirectionCpu(face, ((float)x + 0.5f) / kSize, ((float)y + 0.5f) / kSize);
    float eu, ev;
    directionToEquirectUV(dc, eu, ev);
    Vec3 cpu = sampleEquirectBilinear(pixels, w, h, eu, ev);
    float gpuV[3];
    gpuTexel(0, face, x, y, kSize, gpuV);
    const float* cpuf = &cpu.x;
    for (int c = 0; c < 3; ++c) {
      float expect = 0.375f * std::sqrt((std::max)(0.0f, (std::min)(cpuf[c], 256.0f)));
      float denom = std::fabs(expect) > 1e-3f ? std::fabs(expect) : 1e-3f;
      double rel = std::fabs((double)gpuV[c] - expect) / denom;
      if (rel > maxRelErr) { maxRelErr = rel; worstFace = face; worstX = x; worstY = y; }
    }
  }
  spdlog::info("IBL: cube-chain validation mip0 max rel error = {:.4f}% ({} dirs, worst face {} texel ({},{}))",
               maxRelErr * 100.0, dirs.size(), worstFace, worstX, worstY);
  if (maxRelErr > 0.05)
    spdlog::warn("IBL: mip0 CPU-vs-GPU mismatch > 5% - check CubeFaceDirection/DirectionToEquirectUV conventions, sampler clamp, fp16 quantization");

  // 4) mip8 有限性 + 均值对比
  double mean8[3] = {};
  bool mip8Finite = true;
  spdlog::info("IBL: mip8 per-face values:");
  for (uint32_t f = 0; f < 6; ++f) {
    float t[3];
    gpuTexel((uint32_t)(kMip0Bytes / 2), f, 0, 0, 1, t);
    for (int c = 0; c < 3; ++c) {
      if (!std::isfinite(t[c])) mip8Finite = false;
      mean8[c] += t[c];
    }
    spdlog::info("IBL:   face {} = ({:.4f}, {:.4f}, {:.4f})", f, t[0], t[1], t[2]);
  }
  for (int c = 0; c < 3; ++c) mean8[c] /= 6.0;
  double maxMeanDev = 0.0;
  for (int c = 0; c < 3; ++c) {
    double denom = mean0[c] > 1e-3 ? mean0[c] : 1e-3;
    double dev = std::fabs(mean8[c] - mean0[c]) / denom;
    if (dev > maxMeanDev) maxMeanDev = dev;
  }
  spdlog::info("IBL: cube-chain validation mip8 mean = ({:.4f}, {:.4f}, {:.4f}) vs mip0 mean = ({:.4f}, {:.4f}, {:.4f}), finite={}, max dev = {:.2f}%",
               mean8[0], mean8[1], mean8[2], mean0[0], mean0[1], mean0[2], mip8Finite, maxMeanDev * 100.0);
  // These means use different direction sets after a nonlinear output transform;
  // equality is not an invariant. Numerical parity is checked per mip below.
  if (!mip8Finite)
    spdlog::warn("IBL: mip8 has non-finite values - check GGX importance sampling (Hammersley/ImportanceSampleGGX), roughness push constant, NaN guard");

  vkUnmapMemory(dev, stagingMem);
  vkDestroyBuffer(dev, staging, nullptr);
  vkFreeMemory(dev, stagingMem, nullptr);
  stbi_image_free(pixels);
}

// 把 GPU 生成的 LUT 读回,与 Apple 烘焙的 DFGLUT.ktx 逐 texel 对比(Review Focus #3)。
void validateDfgAgainstReference(const VulkanDevice& device, VkImage dfgImage,
                                 const std::filesystem::path& refPath) {
  VkDevice dev = device.getLogicalDevice();
  constexpr uint32_t kSize = 256;
  constexpr VkDeviceSize kBytes = (VkDeviceSize)kSize * kSize * 4; // RG16F = 4B/texel

  KtxTexture ktx;
  std::string error;
  if (!ktx.load(refPath, error)) {
    spdlog::warn("IBL: DFG validation skipped: {}", error);
    return;
  }
  const auto& header = ktx.header();
  const auto reference = ktx.faceImage(0, 0);
  if (header.glType != 0x140B || header.glTypeSize != 2 || header.glFormat != 0x8227 ||
      header.glInternalFormat != 0x822F || header.width != kSize || header.height != kSize ||
      header.faces != 1 || header.mipLevels != 1 || reference.size != kBytes) {
    spdlog::warn("IBL: DFG reference must be a 256x256 RG16F 2D KTX, skip validation");
    return;
  }

  // 1) readback:SHADER_READ_ONLY → TRANSFER_SRC,copy → staging,转回
  VkBuffer staging; VkDeviceMemory stagingMem;
  createHostBuffer(device, kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, staging, stagingMem);
  VkCommandBuffer cmd = device.beginSingleTimeCommands();
  transitionRange(cmd, dfgImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 1,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {kSize, kSize, 1}; // bufferOffset/rowLength 默认 0 = 紧密(1024B 行距,满足对齐)
  vkCmdCopyImageToBuffer(cmd, dfgImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
  transitionRange(cmd, dfgImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 1,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  device.endSingleTimeCommands(cmd);

  uint16_t* gpu = nullptr;
  if (vkMapMemory(dev, stagingMem, 0, kBytes, 0, (void**)&gpu) != VK_SUCCESS) {
    vkDestroyBuffer(dev, staging, nullptr);
    vkFreeMemory(dev, stagingMem, nullptr);
    spdlog::warn("IBL: DFG readback mapping failed, skip validation");
    return;
  }

  // Reference payload was validated before allocating GPU readback resources.
  {
    double mae = 0.0;
    uint32_t nanCount = 0;
    for (uint32_t i = 0; i < kSize * kSize * 2; ++i) {
      float a = halfToFloat(gpu[i]);
      uint16_t bits;
      std::memcpy(&bits, reference.data + i * sizeof(bits), sizeof(bits));
      float b = halfToFloat(bits);
      if (!std::isfinite(a) || !std::isfinite(b)) { ++nanCount; continue; }
      mae += std::fabs(a - b);
    }
    mae /= (double)(kSize * kSize * 2);
    spdlog::info("IBL: DFG LUT vs Apple reference MAE={:.5f}, non-finite count={}", mae, nanCount);
    // The reference matches correlated Smith with alpha=r in visibility and
    // alpha=r^2 in the sampling NDF. Do not infer this LUT from direct lighting.
    if (mae >= 0.003 || nanCount > 0)
      spdlog::warn("IBL: DFG LUT MAE exceeds 0.003 or contains non-finite data; check Metal visibility convention and sampling");
  }

  vkUnmapMemory(dev, stagingMem);
  vkDestroyBuffer(dev, staging, nullptr);
  vkFreeMemory(dev, stagingMem, nullptr);
}

} // namespace

namespace {

// 与 Apple 烘焙的预过滤 cube(san_giuseppe_bridge_4k_ibl.ktx,RGBA8 RGBM 打包)
// 逐 mip 对比,并与 Metal 硬编码 SH 常数做「求值后辐照度」E(n) 对比(约定无关)。
// RGBM 输出范围为 [0,6],本项目保留 HDR。排除任一侧 >=5.5 的颜色分量,
// 同时报绝对误差与统一曝光归一后的结构误差。参考烘焙源/曝光/预过滤流程未知,
// 因此该对拍只用于排查朝向,不能作为逐像素等价性判定。
void validateEnvCubeAgainstAppleKtx(const VulkanDevice& device, VkImage envCube,
                                    const std::filesystem::path& refPath,
                                    const std::filesystem::path& hdrPath) {
  KtxTexture ktx;
  std::string kerr;
  if (!ktx.load(refPath, kerr)) {
    spdlog::warn("IBL: Apple KTX validation skipped: {}", kerr);
    return;
  }
  if (ktx.header().width != IBLGenerator::kEnvMapSize ||
      ktx.header().height != IBLGenerator::kEnvMapSize || ktx.header().faces != 6 ||
      ktx.header().mipLevels != IBLGenerator::kEnvMipCount) {
    spdlog::warn("IBL: Apple KTX layout unexpected, skip validation");
    return;
  }

  // ---- 1) 全 mip 回读我们的 cube ----
  constexpr uint32_t kMips = IBLGenerator::kEnvMipCount;
  std::array<std::array<std::vector<float>, 6>, kMips> reference;
  for (uint32_t m = 0; m < kMips; ++m)
    for (uint32_t f = 0; f < 6; ++f) {
      uint32_t w = 0, h = 0;
      const uint32_t size = IBLGenerator::kEnvMapSize >> m;
      if (!ktx.decodeFaceRGBM(m, f, reference[m][f], w, h) || w != size || h != size) {
        spdlog::warn("IBL: invalid RGBM reference at mip {} face {}, skip validation", m, f);
        return;
      }
    }
  VkDeviceSize offsets[kMips] = {};
  VkDeviceSize total = 0;
  for (uint32_t m = 0; m < kMips; ++m) {
    offsets[m] = total;
    const uint32_t s = IBLGenerator::kEnvMapSize >> m;
    total += (VkDeviceSize)s * s * 6 * 8; // RGBA16F
  }
  VkDevice dev = device.getLogicalDevice();
  VkBuffer staging; VkDeviceMemory stagingMem;
  createHostBuffer(device, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, staging, stagingMem);
  VkCommandBuffer cmd = device.beginSingleTimeCommands();
  transitionRange(cmd, envCube, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, kMips, 6,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy regions[kMips] = {};
  for (uint32_t m = 0; m < kMips; ++m) {
    const uint32_t s = IBLGenerator::kEnvMapSize >> m;
    regions[m].bufferOffset = offsets[m];
    regions[m].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 6};
    regions[m].imageExtent = {s, s, 1};
  }
  vkCmdCopyImageToBuffer(cmd, envCube, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, kMips, regions);
  transitionRange(cmd, envCube, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, kMips, 6,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  device.endSingleTimeCommands(cmd);
  const uint16_t* gpu = nullptr;
  if (vkMapMemory(dev, stagingMem, 0, total, 0, (void**)&gpu) != VK_SUCCESS) {
    vkDestroyBuffer(dev, staging, nullptr);
    vkFreeMemory(dev, stagingMem, nullptr);
    spdlog::warn("IBL: env reference readback mapping failed, skip validation");
    return;
  }

  // ---- 2) 逐 mip 图像对比 ----
  spdlog::info("IBL: env cube vs Apple KTX (all finite RGB components included; sat counts components >=5.5 on either side):");
  for (uint32_t m = 0; m < kMips; ++m) {
    const uint32_t s = IBLGenerator::kEnvMapSize >> m;
    IBLComparison raw;
    double worstFaceErr = -1.0;
    uint32_t worstFace = 0;
    for (uint32_t f = 0; f < 6; ++f) {
      const auto& apple = reference[m][f];
      const uint16_t* base = gpu + offsets[m] / 2; // uint16 元素
      for (uint32_t y = 0; y < s; ++y)
        for (uint32_t x = 0; x < s; ++x) {
          const size_t ti = (size_t)(y * s + x);
          for (int c = 0; c < 3; ++c) {
            const float a = apple[ti * 3 + c];
            const float o = halfToFloat(base[(((size_t)f * s + y) * s + x) * 4 + c]);
            raw.add(o, a);
          }
        }
    }
    const double meanRel = raw.meanRelativeError();
    const double meanOurs = raw.meanActual();
    const double meanApple = raw.meanReference();
    // 第二遍:所有面共用同一个曝光比例,单面和整体采用相同统计口径。
    double sumPattern = 0.0;
    if (raw.count && meanOurs > 1e-3) {
      const double k = meanApple / meanOurs;
      for (uint32_t f = 0; f < 6; ++f) {
        const auto& apple = reference[m][f];
        IBLComparison facePattern;
        const uint16_t* base = gpu + offsets[m] / 2;
        for (uint32_t y = 0; y < s; ++y)
          for (uint32_t x = 0; x < s; ++x) {
            const size_t ti = (size_t)(y * s + x);
            for (int c = 0; c < 3; ++c) {
              const float a = apple[ti * 3 + c];
              const float o = halfToFloat(base[(((size_t)f * s + y) * s + x) * 4 + c]);
              facePattern.add(o * k, a);
            }
          }
        sumPattern += facePattern.sumRelativeError;
        if (facePattern.count && facePattern.meanRelativeError() > worstFaceErr) {
          worstFaceErr = facePattern.meanRelativeError();
          worstFace = f;
        }
      }
    }
    const double patternRel = raw.count ? sumPattern / (double)raw.count : 0.0;
    spdlog::info("IBL:   mip{} ({}x{}): meanRelErr={:.2f}%  patternErr={:.2f}%  meanOurs={:.3f}  meanApple={:.3f}  sat={}  nonFinite={}  worstPatternFace={}({:.2f}%)",
                 m, s, s, meanRel * 100.0, patternRel * 100.0, meanOurs, meanApple,
                 raw.saturated, raw.nonFinite, worstFace, worstFaceErr * 100.0);
    if (raw.nonFinite || !raw.count || meanOurs <= 1e-3)
      spdlog::warn("IBL: mip{} comparison invalid: nonFinite={}, usable={}, meanOurs={}",
                   m, raw.nonFinite, raw.count, meanOurs);
    if (meanRel >= 0.05)
      spdlog::warn("IBL: mip{} raw relative error {:.2f}% exceeds 5% vs Apple reference", m, meanRel * 100.0);
  }

  // ---- 3) SH 辐照度 E(n) 对比(约定无关:直接比求值结果)----
  // Apple 运行时数据 = AAPLLightingCommon.h 硬编码常数,求值 E(n)=Σ M_i·f_i(-n)。
  // (KTX 的 "sh" 元数据与 Metal 常数存在逐带符号/量级差异——band1 符号相反,
  //  不用作基准;Metal 硬编码常数才是其运行时真值。)
  static const float kAppleSh[9][3] = {
      { 1.614944507896493f,  1.541036092763475f,  1.571013589299304f},
      {-0.253877086046911f, -0.429470071197213f, -0.690516354135927f},
      { 0.169490208844630f,  0.354603612695152f,  0.470313910537248f},
      { 0.097116881286676f,  0.266256657319848f,  0.359295544072626f},
      {-0.068539142976241f, -0.113442880787374f, -0.144920974765986f},
      {-0.155764013783923f, -0.197141784218826f, -0.219866180869429f},
      { 0.048072946052602f,  0.047616845245505f,  0.028245382387344f},
      { 0.222550351872431f,  0.198626269418641f,  0.175229058057126f},
      { 0.025198626854623f, -0.020106073808714f, -0.063087948829664f},
  };
  int w = 0, h = 0, ch = 0;
  float* pixels = stbi_loadf(hdrPath.generic_string().c_str(), &w, &h, &ch, STBI_rgb_alpha);
  if (pixels) {
    // Physical and clamp-to-6 variants are diagnostics only. The application
    // uploads ComputeMetalSH9FromEquirect, which uses the RGBM source bound 256.
    std::vector<float> clamped((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h * 4; ++i)
      clamped[i] = pixels[i] < 6.0f ? pixels[i] : 6.0f;
    const SH9 physicalSh = ComputeSH9FromEquirect(pixels, w, h);
    const SH9 metalSh = ComputeMetalSH9FromEquirect(pixels, w, h);
    const SH9 clampedSh = ComputeSH9FromEquirect(clamped.data(), w, h);
    stbi_image_free(pixels);
    auto evalSh = [](const float c[9][3], float nx, float ny, float nz, float out[3]) {
      const float b[9] = {1.0f, ny, nz, nx, ny * nx, ny * nz,
                          3.0f * nz * nz - 1.0f, nz * nx, nx * nx - ny * ny};
      for (int ch2 = 0; ch2 < 3; ++ch2) {
        float v = 0.0f;
        for (int i = 0; i < 9; ++i) v += c[i][ch2] * b[i];
        out[ch2] = v;
      }
    };
    // 26 个确定方向:6 轴 + 8 角 + 12 棱中点
    std::vector<Vec3> dirs = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (int i = -1; i <= 1; i += 2)
      for (int j = -1; j <= 1; j += 2)
        for (int k = -1; k <= 1; k += 2)
          dirs.push_back(normalizeVec3({(float)i, (float)j, (float)k}));
    const int edges[12][3] = {{1,1,0},{1,-1,0},{-1,1,0},{-1,-1,0},
                              {1,0,1},{1,0,-1},{-1,0,1},{-1,0,-1},
                              {0,1,1},{0,1,-1},{0,-1,1},{0,-1,-1}};
    for (auto& e : edges) dirs.push_back(normalizeVec3({(float)e[0], (float)e[1], (float)e[2]}));

    for (int variant = 0; variant < 3; ++variant) {
      const SH9& sh = variant == 0 ? physicalSh : (variant == 1 ? clampedSh : metalSh);
      const char* label = variant == 0 ? "physical HDR diagnostic" :
                          (variant == 1 ? "clamp-to-6 diagnostic" : "runtime Metal-compatible");
      double sumRel = 0.0, sumRelNorm = 0.0, meanOurs = 0.0, meanApple = 0.0;
      std::vector<std::array<float,3>> eOurs, eApple;
      for (const Vec3& d : dirs) {
        float eo[3], ea[3];
        evalSh(sh.c, d.x, d.y, d.z, eo);
        evalSh(kAppleSh, -d.x, -d.y, -d.z, ea);
        eOurs.push_back({eo[0], eo[1], eo[2]});
        eApple.push_back({ea[0], ea[1], ea[2]});
        for (int c = 0; c < 3; ++c) { meanOurs += eo[c]; meanApple += ea[c]; }
      }
      meanOurs /= (dirs.size() * 3); meanApple /= (dirs.size() * 3);
      for (size_t i = 0; i < dirs.size(); ++i)
        for (int c = 0; c < 3; ++c) {
          const double a = eApple[i][c];
          sumRel += std::fabs(eOurs[i][c] - a) / (std::fabs(a) > 0.05 ? std::fabs(a) : 0.05);
          const double an = a / meanApple, on = eOurs[i][c] / meanOurs;
          sumRelNorm += std::fabs(on - an) / (std::fabs(an) > 0.05 ? std::fabs(an) : 0.05);
        }
      const double n = (double)(dirs.size() * 3);
      spdlog::info("IBL: SH E(n), {}, vs Apple constants ({} dirs): meanRelErr={:.2f}%  pattern(normalized)={:.2f}%  meanOurs={:.3f} meanApple={:.3f}",
                   label, dirs.size(), sumRel / n * 100.0, sumRelNorm / n * 100.0, meanOurs, meanApple);
      if (variant == 2 && (!std::isfinite(sumRel) || sumRel / n >= 0.02))
        spdlog::warn("IBL: runtime SH relative error exceeds 2% vs Apple reference");
    }
  } else {
    spdlog::warn("IBL: SH-vs-Apple validation skipped, cannot reload {}", hdrPath.generic_string());
  }

  vkUnmapMemory(dev, stagingMem);
  vkDestroyBuffer(dev, staging, nullptr);
  vkFreeMemory(dev, stagingMem, nullptr);
}

} // namespace

IBLResources IBLGenerator::generate(const VulkanDevice& device, VkImageView equirectView,
                                    const std::filesystem::path& rootPath) {
  VkDevice dev = device.getLogicalDevice();
  IBLResources out;

  // --- sampler(生成与光照共用:三线性 + clamp) ---
  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = VK_FILTER_LINEAR;
  si.minFilter = VK_FILTER_LINEAR;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.maxLod = (float)kEnvMipCount;
  if (vkCreateSampler(dev, &si, nullptr, &out.sampler) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create sampler");

  // --- 目标资源 ---
  createImage2D(device, VK_FORMAT_R16G16B16A16_SFLOAT, kEnvMapSize, kEnvMapSize,
                kEnvMipCount, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, // TRANSFER_SRC:校验回读
                out.envCube, out.envCubeMemory);

  // mip0 的 2D-array storage view(CS1 输出)
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = out.envCube;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
  VkImageView mip0StorageView;
  if (vkCreateImageView(dev, &vi, nullptr, &mip0StorageView) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create env cube mip0 storage view");

  // cube 采样 view(全 mip;Task 4 预过滤读 mip0、光照 SampleLevel 用)
  vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kEnvMipCount, 0, 6};
  if (vkCreateImageView(dev, &vi, nullptr, &out.envCubeView) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create env cube view");

  // --- DFG LUT(Task 5:Karis split-sum 第二步;256x256 RG16F) ---
  createImage2D(device, VK_FORMAT_R16G16_SFLOAT, 256, 256, 1, 1, 0,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, // TRANSFER_SRC:校验回读
                out.dfgLut, out.dfgLutMemory);

  VkImageViewCreateInfo lvi{};
  lvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  lvi.image = out.dfgLut;
  lvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  lvi.format = VK_FORMAT_R16G16_SFLOAT;
  lvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(dev, &lvi, nullptr, &out.dfgLutView) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create DFG LUT view");

  // --- CS1 pipeline ---
  VkDescriptorSetLayoutBinding cs1Bindings[3] = {};
  cs1Bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  cs1Bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  cs1Bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo slci{};
  slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  slci.bindingCount = 3;
  slci.pBindings = cs1Bindings;
  VkDescriptorSetLayout cs1SetLayout;
  if (vkCreateDescriptorSetLayout(dev, &slci, nullptr, &cs1SetLayout) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create CS1 descriptor set layout");

  VkPushConstantRange pcRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &cs1SetLayout;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcRange;
  VkPipelineLayout cs1Layout;
  if (vkCreatePipelineLayout(dev, &plci, nullptr, &cs1Layout) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create CS1 pipeline layout");

  VkShaderModule cs1Module = loadModule(dev, rootPath / "shaders" / "ibl_equirect.cs.spv");
  VkComputePipelineCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_COMPUTE_BIT, cs1Module, "EquirectToCubeCS", nullptr};
  cpci.layout = cs1Layout;
  VkPipeline cs1Pipeline;
  if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &cs1Pipeline) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create equirect-to-cube pipeline");

  // --- CS2 pipeline(GGX 预过滤;只引用 binding 1/2/3,无 binding 0) ---
  VkDescriptorSetLayoutBinding cs2Bindings[3] = {};
  cs2Bindings[0] = {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  cs2Bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  cs2Bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo slci2{};
  slci2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  slci2.bindingCount = 3;
  slci2.pBindings = cs2Bindings;
  VkDescriptorSetLayout cs2SetLayout;
  if (vkCreateDescriptorSetLayout(dev, &slci2, nullptr, &cs2SetLayout) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create CS2 descriptor set layout");

  VkPipelineLayoutCreateInfo plci2{};
  plci2.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci2.setLayoutCount = 1;
  plci2.pSetLayouts = &cs2SetLayout;
  plci2.pushConstantRangeCount = 1;
  plci2.pPushConstantRanges = &pcRange;
  VkPipelineLayout cs2Layout;
  if (vkCreatePipelineLayout(dev, &plci2, nullptr, &cs2Layout) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create CS2 pipeline layout");

  VkShaderModule cs2Module = loadModule(dev, rootPath / "shaders" / "ibl_prefilter.cs.spv");
  VkComputePipelineCreateInfo cpci2{};
  cpci2.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cpci2.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_COMPUTE_BIT, cs2Module, "PrefilterSpecularCS", nullptr};
  cpci2.layout = cs2Layout;
  VkPipeline cs2Pipeline;
  if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci2, nullptr, &cs2Pipeline) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create prefilter pipeline");

  // --- CS3 pipeline(DFG LUT;set layout 单 binding 0 = STORAGE_IMAGE,无 push constants) ---
  VkDescriptorSetLayoutBinding cs3Binding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                          VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo slci3{};
  slci3.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  slci3.bindingCount = 1;
  slci3.pBindings = &cs3Binding;
  VkDescriptorSetLayout cs3SetLayout;
  if (vkCreateDescriptorSetLayout(dev, &slci3, nullptr, &cs3SetLayout) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create CS3 descriptor set layout");

  VkPipelineLayoutCreateInfo plci3{};
  plci3.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci3.setLayoutCount = 1;
  plci3.pSetLayouts = &cs3SetLayout;
  plci3.pushConstantRangeCount = 0; // CS3 无 push constants(dxc 单 push block 属 CS1/CS2)
  plci3.pPushConstantRanges = nullptr;
  VkPipelineLayout cs3Layout;
  if (vkCreatePipelineLayout(dev, &plci3, nullptr, &cs3Layout) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create CS3 pipeline layout");

  VkShaderModule cs3Module = loadModule(dev, rootPath / "shaders" / "ibl_dfglut.cs.spv");
  VkComputePipelineCreateInfo cpci3{};
  cpci3.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cpci3.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_COMPUTE_BIT, cs3Module, "DfgLutCS", nullptr};
  cpci3.layout = cs3Layout;
  VkPipeline cs3Pipeline;
  if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci3, nullptr, &cs3Pipeline) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create DFG LUT pipeline");

  // --- descriptor pool/set(生成专用,一次性) ---
  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 12}, // CS1 equirect + CS2 × 8 mip 各 1
      {VK_DESCRIPTOR_TYPE_SAMPLER, 12},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 13}, // 9 个 mip view(CS1 mip0 + CS2 mip1-8)+ CS3 LUT
  };
  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.maxSets = 13;
  dpci.poolSizeCount = 3;
  dpci.pPoolSizes = poolSizes;
  VkDescriptorPool pool;
  if (vkCreateDescriptorPool(dev, &dpci, nullptr, &pool) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create descriptor pool");

  VkDescriptorSetAllocateInfo dsai{};
  dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsai.descriptorPool = pool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &cs1SetLayout;
  VkDescriptorSet cs1Set;
  if (vkAllocateDescriptorSets(dev, &dsai, &cs1Set) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to allocate CS1 descriptor set");

  // 注意:VkDescriptorImageInfo 字段序为 (sampler, imageView, imageLayout),
  // 与 GpuScene.cpp:1383 一致用成员赋值,避免 brace 顺序错误。
  VkDescriptorImageInfo eqInfo{};
  eqInfo.imageView = equirectView;
  eqInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo sampInfo{};
  sampInfo.sampler = out.sampler;
  VkDescriptorImageInfo outInfo{};
  outInfo.imageView = mip0StorageView;
  outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkWriteDescriptorSet writes[3] = {};
  writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cs1Set, 0, 0, 1,
               VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &eqInfo, nullptr, nullptr};
  writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cs1Set, 1, 0, 1,
               VK_DESCRIPTOR_TYPE_SAMPLER, &sampInfo, nullptr, nullptr};
  writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cs1Set, 2, 0, 1,
               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outInfo, nullptr, nullptr};
  vkUpdateDescriptorSets(dev, 3, writes, 0, nullptr);

  // CS3 descriptor set(单 binding 0 = LUT storage image)
  dsai.pSetLayouts = &cs3SetLayout;
  VkDescriptorSet cs3Set;
  if (vkAllocateDescriptorSets(dev, &dsai, &cs3Set) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to allocate CS3 descriptor set");
  VkDescriptorImageInfo lutInfo{};
  lutInfo.imageView = out.dfgLutView;
  lutInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkWriteDescriptorSet w3{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cs3Set, 0, 0, 1,
                          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &lutInfo, nullptr, nullptr};
  vkUpdateDescriptorSets(dev, 1, &w3, 0, nullptr);

  // --- 录制 one-shot 命令 ---
  VkCommandBuffer cmd = device.beginSingleTimeCommands();

  // 全 mip → GENERAL(生成期统一;Task 4 写 mip1-8 无需再转)
  transitionRange(cmd, out.envCube, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  0, kEnvMipCount, 6,
                  0, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs1Pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs1Layout, 0, 1, &cs1Set, 0, nullptr);
  uint32_t pc[4] = {kEnvMapSize, 0, 0, 0};
  vkCmdPushConstants(cmd, cs1Layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
  vkCmdDispatch(cmd, kEnvMapSize / 8, kEnvMapSize / 8, 6);

  // mip0: STORAGE_WRITE → SHADER_READ(Task 4 的预过滤从 mip0 采样)
  transitionRange(cmd, out.envCube, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                  0, 1, 6,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  // === Task 4:PrefilterSpecularCS 对 mip1-8 做 GGX 重要性采样预过滤 ===
  // 生命周期规则:mipView/descriptor set 的销毁必须在 endSingleTimeCommands 之后
  // (命令录制引用的资源在执行完成前不得销毁),mipView 先存 vector 统一收尾。
  std::vector<VkImageView> mipViews;
  VkDescriptorImageInfo cubeInfo{};
  cubeInfo.imageView = out.envCubeView;
  cubeInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL; // 生成期 GENERAL 下采样合法
  VkDescriptorImageInfo sampInfo2{};
  sampInfo2.sampler = out.sampler;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs2Pipeline);
  for (uint32_t mip = 1; mip < kEnvMipCount; ++mip) {
    const uint32_t mipSize = kEnvMapSize >> mip;

    VkImageViewCreateInfo mvi{};
    mvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    mvi.image = out.envCube;
    mvi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    mvi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    mvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6};
    VkImageView mipView;
    if (vkCreateImageView(dev, &mvi, nullptr, &mipView) != VK_SUCCESS)
      throw std::runtime_error("IBL: failed to create env cube mip view");
    mipViews.push_back(mipView);

    VkDescriptorSetAllocateInfo dsai2{};
    dsai2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai2.descriptorPool = pool;
    dsai2.descriptorSetCount = 1;
    dsai2.pSetLayouts = &cs2SetLayout;
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(dev, &dsai2, &set) != VK_SUCCESS)
      throw std::runtime_error("IBL: failed to allocate CS2 descriptor set");

    VkDescriptorImageInfo outInfo2{};
    outInfo2.imageView = mipView;
    outInfo2.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w2[3] = {};
    w2[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 3, 0, 1,
             VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &cubeInfo, nullptr, nullptr};
    w2[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1,
             VK_DESCRIPTOR_TYPE_SAMPLER, &sampInfo2, nullptr, nullptr};
    w2[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outInfo2, nullptr, nullptr};
    vkUpdateDescriptorSets(dev, 3, w2, 0, nullptr);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs2Layout, 0, 1, &set, 0, nullptr);
    // push constant 布局与 HLSL IblPushConstants 一致(mipSize@0, roughness@4)
    struct { uint32_t mipSize; float roughness; uint32_t p0, p1; } pc2 = {
        mipSize, (float)mip / (float)(kEnvMipCount - 1), 0, 0};
    vkCmdPushConstants(cmd, cs2Layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &pc2);
    vkCmdDispatch(cmd, (mipSize + 7) / 8, (mipSize + 7) / 8, 6);

    // 各 mip 只写一次、输入恒为 mip0,dispatch 间无需 barrier
  }

  // All prefilters have consumed linear mip0. Now replace mip0 with the
  // Apple shader-visible value, matching the already encoded mip1..8.
  transitionRange(cmd, out.envCube, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                  0, 1, 6, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs1Pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs1Layout, 0, 1, &cs1Set, 0, nullptr);
  pc[2] = 1;
  vkCmdPushConstants(cmd, cs1Layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
  vkCmdDispatch(cmd, kEnvMapSize / 8, kEnvMapSize / 8, 6);

  // === Task 5:DfgLutCS 生成 Karis split-sum DFG LUT(256x256 RG16F,无 push constants)===
  transitionRange(cmd, out.dfgLut, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  0, 1, 1,
                  0, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs3Pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs3Layout, 0, 1, &cs3Set, 0, nullptr);
  vkCmdDispatch(cmd, 32, 32, 1); // 256/8
  // 与 env cube 的最终 transition 并列:GENERAL → SHADER_READ_ONLY
  transitionRange(cmd, out.dfgLut, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  0, 1, 1,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  // 最终:全 mip → SHADER_READ_ONLY
  transitionRange(cmd, out.envCube, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  0, kEnvMipCount, 6,
                  VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  device.endSingleTimeCommands(cmd);

  // 清理生成期临时对象(输出资源保留)
  vkDestroyImageView(dev, mip0StorageView, nullptr);
  for (VkImageView v : mipViews) vkDestroyImageView(dev, v, nullptr);
  vkDestroyShaderModule(dev, cs1Module, nullptr);
  vkDestroyShaderModule(dev, cs2Module, nullptr);
  vkDestroyShaderModule(dev, cs3Module, nullptr);
  vkDestroyPipeline(dev, cs1Pipeline, nullptr);
  vkDestroyPipeline(dev, cs2Pipeline, nullptr);
  vkDestroyPipeline(dev, cs3Pipeline, nullptr);
  vkDestroyPipelineLayout(dev, cs1Layout, nullptr);
  vkDestroyPipelineLayout(dev, cs2Layout, nullptr);
  vkDestroyPipelineLayout(dev, cs3Layout, nullptr);
  vkDestroyDescriptorSetLayout(dev, cs1SetLayout, nullptr);
  vkDestroyDescriptorSetLayout(dev, cs2SetLayout, nullptr);
  vkDestroyDescriptorSetLayout(dev, cs3SetLayout, nullptr);
  vkDestroyDescriptorPool(dev, pool, nullptr);

  // CPU-vs-GPU cube 链校验(Ruling 9/10;envCube 已处 SHADER_READ_ONLY)
  // 注意:此处的 hdr 路径必须与 GpuScene::initIBL 选用的环境贴图保持一致
  // (GpuScene.cpp initIBL 的 hdrPath);换资产时两处需同步修改。
  // 与 GpuScene.cpp HdrEnvPath 同源,换资产需同步。
  validateCubeChainAgainstCpu(device, out.envCube,
                              rootPath / "textures" / "san_giuseppe_bridge_2k.hdr");

  // Optional local reference data; no machine-specific path or asset distribution required.
  const char* referenceDir = std::getenv("AVR_IBL_REFERENCE_DIR");
  if (referenceDir && *referenceDir) {
    const std::filesystem::path refs(referenceDir);
    spdlog::info("IBL: using reference directory {}", refs.generic_string());
    validateDfgAgainstReference(device, out.dfgLut, refs / "DFGLUT.ktx");
    validateEnvCubeAgainstAppleKtx(device, out.envCube,
        refs / "san_giuseppe_bridge_4k_ibl.ktx",
        rootPath / "textures" / "san_giuseppe_bridge_2k.hdr");
  }

  return out;
}
