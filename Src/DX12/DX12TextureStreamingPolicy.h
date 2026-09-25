#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

// DX12 texture streaming 的纯逻辑内核:不依赖 D3D12,可独立单测。
// 公式逐行镜像 Vulkan 路径(Src/GpuScene.cpp 的 streaming 段,本身镜像
// Metal AAPLTextureManager)。与 Vulkan 的唯一刻意差异:staging 行距按
// D3D12_TEXTURE_DATA_PITCH_ALIGNMENT(256)对齐——PLACED_FOOTPRINT 要求,
// Vulkan 的 tightly-packed staging 无此约束。
namespace DX12TextureStreamingPolicy {

inline constexpr unsigned int PERMANENT_TEXTURE_SIZE = 64;
inline constexpr unsigned int MAX_TEXTURE_SIZE = 4096;

// 资源中出现的 MTLPixelFormat 数值(与 GpuScene.cpp enum / MapMTLToDXGI 同值)。
inline constexpr uint32_t MTLBC1_RGBA = 130;
inline constexpr uint32_t MTLBC1_RGBA_sRGB = 131;
inline constexpr uint32_t MTLBC4_RUnorm = 140;
inline constexpr uint32_t MTLBC4_RSnorm = 141;

// BC 块边长(texels)与块字节数。镜像 GpuScene.cpp::getPixelFormatBlockDesc
// 及其默认分支:非 BC1/BC4 一律 (4,16)——对 BC2/BC3/BC5/BC6H/BC7 均正确。
inline void GetPixelFormatBlockDesc(uint32_t pixelFormat,
                                    uint64_t& blockSize,
                                    uint64_t& bytesPerBlock) {
  blockSize = 4;
  bytesPerBlock = (pixelFormat == MTLBC1_RGBA || pixelFormat == MTLBC1_RGBA_sRGB ||
                   pixelFormat == MTLBC4_RUnorm || pixelFormat == MTLBC4_RSnorm)
                      ? 8 : 16;
}

// 镜像 GpuScene.cpp::calculateMipSizeInBlocks(floor 口径,至少 1 块)。
// 注意:DX12 CreateTextures 现有上传用 ceil((dim+3)/4);4 的倍数尺寸两者一致,
// streaming 路径必须与 Vulkan 的 floor 口径逐字节一致(源数据按此布局)。
inline uint64_t CalculateMipSizeInBlocks(uint64_t size, uint64_t blockSize, uint32_t mip) {
  uint64_t blocksWide = (size / blockSize) > 1 ? (size / blockSize) : 1;
  return (blocksWide >> mip) > 1 ? (blocksWide >> mip) : 1;
}

// 镜像 GpuScene.cpp::calculateMinMip(整数比、截断 log2、clamp 到已有 mip)。
inline uint32_t CalculateMinMip(uint64_t width, uint64_t height, uint64_t mipCount,
                                unsigned int maxSize) {
  uint64_t texSize = width > height ? width : height;
  uint64_t ratio = texSize / maxSize;
  if (ratio < 1) ratio = 1;
  int minMip = static_cast<int>(std::log2(static_cast<float>(ratio)));
  if (minMip >= static_cast<int>(mipCount))
    minMip = static_cast<int>(mipCount) - 1;
  return static_cast<uint32_t>(minMip);
}

// 镜像 GpuScene.cpp::calculateRequiredMip(clamp [topMip, botMip])。
inline uint32_t CalculateRequiredMip(uint64_t width, uint64_t height, uint64_t mipCount,
                                     float screenArea) {
  float topMipTexelArea = static_cast<float>(width * height);
  uint32_t topMip = CalculateMinMip(width, height, mipCount, MAX_TEXTURE_SIZE);
  uint32_t botMip = CalculateMinMip(width, height, mipCount, PERMANENT_TEXTURE_SIZE);

  if (screenArea <= 0.0f) return botMip;
  int mipLevel = static_cast<int>(0.5f * std::log2(topMipTexelArea / screenArea));
  if (mipLevel < static_cast<int>(topMip)) mipLevel = static_cast<int>(topMip);
  if (mipLevel > static_cast<int>(botMip)) mipLevel = static_cast<int>(botMip);
  return static_cast<uint32_t>(mipLevel);
}

// Metal 包围球屏幕面积投影(GpuScene.cpp:5806-5817)。
// viewX/viewY/viewZ: 球心 view 空间坐标;radius: 球半径。
// 只用 x 缩放与 view z,与 NDC Y / 负高度 viewport 无关。
inline float ComputeScreenArea(float viewX, float viewY, float viewZ, float radius,
                               float focalLengthSquared, float viewW, float viewH) {
  if (viewZ <= radius) return viewW * viewH;
  float radiusSquared = radius * radius;
  float z2 = viewZ * viewZ;
  float l2 = viewX * viewX + viewY * viewY + viewZ * viewZ;
  float area = -3.1415926535897932f * focalLengthSquared * radiusSquared *
               std::sqrt(std::fabs((l2 - radiusSquared) / (radiusSquared - z2))) /
               (radiusSquared - z2);
  return area * viewW * viewH * 0.25f;
}

// staging→texture 单条 copy(新 mip),D3D12 PLACED_FOOTPRINT 口径。
struct StreamingBufferCopy {
  uint64_t stagingOffset;  // work item upload buffer 内字节偏移
  uint32_t width, height;  // mip 尺寸(texel)
  uint32_t rowPitch;       // 256 对齐后的行距(staging 内布局)
  uint32_t srcRowPitch;    // 解压数据的紧密行距(拷贝源)
  uint32_t blockRows;      // 块行数
  uint32_t dstSubresource; // 新纹理的 subresource
};

// texture→texture 单条 copy(共享 mip),D3D12 SUBRESOURCE_INDEX 口径。
struct StreamingImageCopy {
  uint32_t srcSubresource; // 旧纹理 subresource
  uint32_t dstSubresource; // 新纹理 subresource
  uint32_t width, height;  // mip 尺寸(texel)
};

struct StreamingCopyPlan {
  std::vector<StreamingBufferCopy> bufferCopies;
  std::vector<StreamingImageCopy> imageCopies;
  uint64_t stagingSize = 0;
  uint32_t newMipCount = 0;
  uint32_t oldMipCount = 0;
};

// 常驻从 currentMip 迁移到 targetMip 的 copy 计划。
// 镜像 GpuScene.cpp::blitThreadFunc 的两个 region 分支(GpuScene.cpp:5584-5649):
// 升级(target<current):上传 [target, current) 段 + old sub m -> new sub m+(current-target)
// 驱逐(target>current):无上传 + old sub m+(target-current) -> new sub m
inline StreamingCopyPlan BuildStreamingCopyPlan(uint64_t width, uint64_t height,
                                                uint64_t mipCount, uint32_t pixelFormat,
                                                uint32_t targetMip, uint32_t currentMip) {
  StreamingCopyPlan plan;
  if (targetMip == currentMip || targetMip >= mipCount || currentMip >= mipCount)
    return plan;

  uint64_t blockSize, bytesPerBlock;
  GetPixelFormatBlockDesc(pixelFormat, blockSize, bytesPerBlock);

  plan.newMipCount = static_cast<uint32_t>(mipCount) - targetMip;
  plan.oldMipCount = static_cast<uint32_t>(mipCount) - currentMip;
  const bool addingMips = targetMip < currentMip;

  if (addingMips) {
    uint64_t offset = 0;
    for (uint32_t m = targetMip; m < currentMip; ++m) {
      uint64_t bw = CalculateMipSizeInBlocks(width, blockSize, m);
      uint64_t bh = CalculateMipSizeInBlocks(height, blockSize, m);
      uint32_t mipW = static_cast<uint32_t>((width >> m) > 0 ? (width >> m) : 1);
      uint32_t mipH = static_cast<uint32_t>((height >> m) > 0 ? (height >> m) : 1);
      uint32_t srcRowPitch = static_cast<uint32_t>(bw * bytesPerBlock);
      uint32_t rowPitch = (srcRowPitch + 255u) & ~255u; // D3D12 pitch alignment
      plan.bufferCopies.push_back({offset, mipW, mipH, rowPitch, srcRowPitch,
                                   static_cast<uint32_t>(bh), m - targetMip});
      offset += static_cast<uint64_t>(rowPitch) * bh;
      // PLACED_FOOTPRINT.Offset 必须是 D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
      // (512) 的倍数;rowPitch 只保证 256,小 mip 需要补 padding。
      offset = (offset + 511ull) & ~511ull;
    }
    plan.stagingSize = offset;

    uint32_t sharedOffset = currentMip - targetMip;
    for (uint32_t m = 0; m < plan.oldMipCount; ++m) {
      uint32_t srcMip = m + currentMip;
      plan.imageCopies.push_back({m, m + sharedOffset,
          static_cast<uint32_t>((width >> srcMip) > 0 ? (width >> srcMip) : 1),
          static_cast<uint32_t>((height >> srcMip) > 0 ? (height >> srcMip) : 1)});
    }
  } else {
    uint32_t dstOffset = targetMip - currentMip;
    for (uint32_t m = 0; m < plan.newMipCount; ++m) {
      uint32_t srcMip = m + targetMip;
      plan.imageCopies.push_back({m + dstOffset, m,
          static_cast<uint32_t>((width >> srcMip) > 0 ? (width >> srcMip) : 1),
          static_cast<uint32_t>((height >> srcMip) > 0 ? (height >> srcMip) : 1)});
    }
  }
  return plan;
}

} // namespace DX12TextureStreamingPolicy
