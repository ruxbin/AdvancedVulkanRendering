#pragma once

// KtxTexture — 最小 KTX1 解析器(只读,无 Vulkan 依赖)。
//
// 用途:开发期校验。把 Apple《Modern Rendering with Metal》烘焙的
// san_giuseppe_bridge_4k_ibl.ktx(cube, RGBA8, RGBM 打包)读进来,
// 与本项目 IBLGenerator 运行时生成的预过滤 cube 逐 mip 对比;
// 同时解析其 "sh" 元数据(9 行 RGB 球谐系数)。
//
// KTX1 布局(spec 4.3/4.4):
//   64B 头(12B identifier + 13×uint32)
//   bytesOfKeyValueData:{uint32 size; key\0value(pad4)} × N
//   per mip: {uint32 imageSize; per face: imageSize 字节数据 + cubePadding(pad4)}
//   (cube 的 imageSize 是「单面」大小;非 cube 即整个 mip)

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct Ktx1Header {
  uint32_t glType = 0;
  uint32_t glTypeSize = 0;
  uint32_t glFormat = 0;
  uint32_t glInternalFormat = 0;
  uint32_t glBaseInternalFormat = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
  uint32_t arrayElements = 0;
  uint32_t faces = 0;
  uint32_t mipLevels = 0;
  uint32_t bytesOfKeyValueData = 0;
};

class KtxTexture {
public:
  // 支持 little-endian、非数组的 2D/cube。失败返回 false、清空纹理并填 err。
  // 成功清空 err;不自动生成 numberOfMipmapLevels=0 请求的其余 mip。
  bool load(const std::filesystem::path& path, std::string& err);

  const Ktx1Header& header() const { return _header; }

  // 取 key/value 对(value 以文本形式返回,如 "sh" 的 9 行 RGB)。
  bool findKeyValue(const std::string& key, std::string& valueOut) const;

  struct FaceData {
    const uint8_t* data = nullptr; // 指向文件缓冲内
    uint32_t size = 0;             // 字节数(= imageSize,未含 padding)
  };
  // mip/face 越界返回空 FaceData。faces==1 的文件传 face=0。
  FaceData faceImage(uint32_t mip, uint32_t face) const;

  // 便捷:解析 "sh" 元数据为 9 个 RGB 三元组(不足 27 个数返回 false)。
  bool parseShCoefficients(float out[9][3]) const;

  // RGBM 解码(Apple 的 6.0*rgb*a 打包):faceImage 的 RGBA8 → 每像素 float3。
  // 输出 out 大小为 w*h*3;w/h 为该 mip 尺寸。非 RGBA8 或越界返回 false。
  bool decodeFaceRGBM(uint32_t mip, uint32_t face, std::vector<float>& out,
                      uint32_t& w, uint32_t& h) const;

private:
  bool loadFile(const std::filesystem::path& path, std::string& err);
  Ktx1Header _header{};
  std::vector<char> _bytes;
  // 每个 mip:imageSize + 各 face 在 _bytes 中的偏移
  struct MipInfo {
    uint32_t imageSize = 0;
    std::vector<size_t> faceOffsets;
  };
  std::vector<MipInfo> _mips;
  std::vector<std::pair<std::string, std::string>> _keyValues;
};
