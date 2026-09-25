#include "DX12TextureStreamingPolicy.h"

#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

void expectU64(uint64_t actual, uint64_t expected, const char* message) {
  if (actual != expected) {
    std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
    ++failures;
  }
}

void expectU32(uint32_t actual, uint32_t expected, const char* message) {
  expectU64(actual, expected, message);
}

void expectSize(size_t actual, size_t expected, const char* message) {
  expectU64((uint64_t)actual, (uint64_t)expected, message);
}

void expectNear(float actual, float expected, float eps, const char* message) {
  if (std::fabs(actual - expected) > eps) {
    std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
    ++failures;
  }
}

void expectTrue(bool actual, const char* message) {
  if (!actual) {
    std::cerr << message << ": expected true\n";
    ++failures;
  }
}

} // namespace

using namespace DX12TextureStreamingPolicy;

int main() {
  // --- GetPixelFormatBlockDesc（镜像 GpuScene.cpp::getPixelFormatBlockDesc 含默认分支)---
  uint64_t bs = 0, bpb = 0;
  GetPixelFormatBlockDesc(130, bs, bpb); // BC1_RGBA
  expectU64(bs, 4, "BC1 block size");
  expectU64(bpb, 8, "BC1 bytes per block");
  GetPixelFormatBlockDesc(134, bs, bpb); // BC3_RGBA
  expectU64(bpb, 16, "BC3 bytes per block");
  GetPixelFormatBlockDesc(140, bs, bpb); // BC4_RUnorm
  expectU64(bpb, 8, "BC4 bytes per block");
  GetPixelFormatBlockDesc(142, bs, bpb); // BC5_RGUnorm
  expectU64(bpb, 16, "BC5 bytes per block");
  GetPixelFormatBlockDesc(152, bs, bpb); // BC7 — 走默认分支
  expectU64(bpb, 16, "BC7 bytes per block");

  // --- CalculateMipSizeInBlocks（floor 口径,与 Vulkan 一致)---
  expectU64(CalculateMipSizeInBlocks(4096, 4, 0), 1024, "4096 mip0 blocks");
  expectU64(CalculateMipSizeInBlocks(4096, 4, 6), 16, "4096 mip6 blocks");
  expectU64(CalculateMipSizeInBlocks(4, 4, 10), 1, "tiny size clamps to 1 block");

  // --- CalculateMinMip ---
  expectU32(CalculateMinMip(4096, 4096, 13, 4096), 0, "4096 tex top mip");
  expectU32(CalculateMinMip(4096, 4096, 13, 64), 6, "4096 tex permanent mip");
  expectU32(CalculateMinMip(8192, 8192, 14, 4096), 1, "8192 tex top mip");
  expectU32(CalculateMinMip(8192, 8192, 14, 64), 7, "8192 tex permanent mip");
  expectU32(CalculateMinMip(2048, 1024, 12, 64), 5, "2048x1024 uses largest dimension");
  expectU32(CalculateMinMip(32, 32, 6, 64), 0, "small texture clamps ratio to 1");
  expectU32(CalculateMinMip(128, 128, 8, 64), 1, "128 tex permanent mip");

  // --- CalculateRequiredMip ---
  expectU32(CalculateRequiredMip(4096, 4096, 13, 0.0f), 6, "no coverage -> bot mip");
  expectU32(CalculateRequiredMip(4096, 4096, 13, 4096.0f * 4096.0f), 0,
            "full coverage -> top mip");
  expectU32(CalculateRequiredMip(4096, 4096, 13, 1024.0f * 1024.0f), 2,
            "1/16 area -> mip 2");
  expectU32(CalculateRequiredMip(4096, 4096, 13, 1.0f), 6, "tiny area clamps to bot mip");
  expectU32(CalculateRequiredMip(8192, 8192, 14, 8192.0f * 8192.0f), 1,
            "huge texture clamps to top mip (MAX_TEXTURE_SIZE)");

  // --- ComputeScreenArea ---
  // 轴上远球:focal=1,1000x1000,center(0,0,100),r=1
  // area = pi/9999 * 1000*1000*0.25 ≈ 78.555
  float area = ComputeScreenArea(0.0f, 0.0f, 100.0f, 1.0f, 1.0f, 1000.0f, 1000.0f);
  expectNear(area, 78.555f, 0.01f, "far on-axis sphere area");
  // 球心距 <= 半径:全覆盖
  expectNear(ComputeScreenArea(0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 1280.0f, 720.0f),
             1280.0f * 720.0f, 1e-3f, "enclosing sphere covers screen");

  // --- BuildStreamingCopyPlan: 升级 6 -> 4,4096 BC7(16 B/block)---
  {
    auto plan = BuildStreamingCopyPlan(4096, 4096, 13, 152, 4, 6);
    expectU32(plan.newMipCount, 9, "upgrade: new texture mip count");
    expectU32(plan.oldMipCount, 7, "upgrade: old texture mip count");
    expectSize(plan.bufferCopies.size(), 2, "upgrade: mips 4,5 uploaded");
    // mip 4:256x256 texels = 64x64 blocks * 16 B = 1024 B 行(已 256 对齐)
    expectU32(plan.bufferCopies[0].width, 256, "mip4 width");
    expectU32(plan.bufferCopies[0].height, 256, "mip4 height");
    expectU32(plan.bufferCopies[0].srcRowPitch, 1024, "mip4 tight pitch");
    expectU32(plan.bufferCopies[0].rowPitch, 1024, "mip4 aligned pitch");
    expectU32(plan.bufferCopies[0].blockRows, 64, "mip4 block rows");
    expectU32(plan.bufferCopies[0].dstSubresource, 0, "mip4 -> new sub 0");
    expectU64(plan.bufferCopies[0].stagingOffset, 0, "mip4 at staging start");
    // mip 5:128x128 = 32x32 blocks * 16 = 512 B 行
    expectU32(plan.bufferCopies[1].dstSubresource, 1, "mip5 -> new sub 1");
    expectU64(plan.bufferCopies[1].stagingOffset, 64ull * 1024, "mip5 staging offset");
    expectU64(plan.stagingSize, 64ull * 1024 + 32ull * 512, "upgrade staging total");
    // 共享:old 7 mips 落到 new subresources [2..8]
    expectSize(plan.imageCopies.size(), 7, "upgrade: all old mips copied");
    expectU32(plan.imageCopies[0].srcSubresource, 0, "upgrade: old sub 0");
    expectU32(plan.imageCopies[0].dstSubresource, 2, "upgrade: -> new sub (6-4)=2");
    expectU32(plan.imageCopies[0].width, 64, "upgrade: shared mip size");
    expectU32(plan.imageCopies[6].dstSubresource, 8, "upgrade: last old mip -> new sub 8");
  }

  // --- BuildStreamingCopyPlan: 驱逐 2 -> 6,4096 BC1(8 B/block)---
  {
    auto plan = BuildStreamingCopyPlan(4096, 4096, 13, 130, 6, 2);
    expectU32(plan.newMipCount, 7, "evict: new mip count");
    expectU32(plan.oldMipCount, 11, "evict: old mip count");
    expectTrue(plan.bufferCopies.empty(), "evict: no staging uploads");
    expectU64(plan.stagingSize, 0, "evict: staging size zero");
    expectSize(plan.imageCopies.size(), 7, "evict: new mips come from old");
    expectU32(plan.imageCopies[0].srcSubresource, 4, "evict: old sub (6-2)=4 -> new 0");
    expectU32(plan.imageCopies[0].dstSubresource, 0, "evict: dst 0");
    expectU32(plan.imageCopies[0].width, 64, "evict: mip6 width");
  }

  // --- BuildStreamingCopyPlan: 退化输入 ---
  {
    auto same = BuildStreamingCopyPlan(1024, 1024, 11, 134, 3, 3);
    expectTrue(same.bufferCopies.empty() && same.imageCopies.empty() &&
               same.stagingSize == 0, "same mip -> empty plan");
  }

  // --- 小节距对齐:升级 7 -> 6,4096 BC1,mip6 = 64 tex = 16 blocks * 8 B = 128 B -> 256 ---
  {
    auto plan = BuildStreamingCopyPlan(4096, 4096, 13, 130, 6, 7);
    expectSize(plan.bufferCopies.size(), 1, "one new mip");
    expectU32(plan.bufferCopies[0].srcRowPitch, 128, "tight pitch 128");
    expectU32(plan.bufferCopies[0].rowPitch, 256, "pitch aligns up to 256");
    expectU32(plan.bufferCopies[0].blockRows, 16, "16 block rows");
    expectU64(plan.stagingSize, 256ull * 16, "staging uses aligned pitch");
    // 共享:old 6 mips 落到 new subs [1..6]
    expectSize(plan.imageCopies.size(), 6, "old mips copied");
    expectU32(plan.imageCopies[0].dstSubresource, 1, "old sub 0 -> new sub 1");
  }

  // --- 512 对齐回归(pin D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT 修复)---
  // 升级 12 -> 10,4096 BC7:mip 10、11 均为 1x1 块(bw=bh=1),
  // rowPitch 16->256,segment = 256*1 = 256(非 512 倍数)。
  // mip11 的 stagingOffset 必须进位到 512,而不是 256。
  {
    auto plan = BuildStreamingCopyPlan(4096, 4096, 13, 152, 10, 12);
    expectSize(plan.bufferCopies.size(), 2, "512-align: mips 10,11 uploaded");
    expectU32(plan.bufferCopies[0].dstSubresource, 0, "512-align: mip10 -> new sub 0");
    expectU64(plan.bufferCopies[0].stagingOffset, 0, "512-align: mip10 at 0");
    expectU32(plan.bufferCopies[0].blockRows, 1, "512-align: mip10 single block row");
    expectU32(plan.bufferCopies[0].rowPitch, 256, "512-align: mip10 pitch 256");
    expectU64(plan.bufferCopies[1].stagingOffset, 512, "512-align: mip11 offset rounds up to 512");
    expectU64(plan.stagingSize, 1024, "512-align: total rounds up to 1024");
  }

  return failures == 0 ? 0 : 1;
}
