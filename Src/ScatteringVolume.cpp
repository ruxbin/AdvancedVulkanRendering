#pragma execution_character_set("utf-8")
#include "Include/ScatteringVolume.h"
#include "Include/Common.h"
#include "spdlog/spdlog.h"

#include <stdexcept>
#include <array>
#include <cstring>
#include <cmath>

// Push-constant block – must match scattervolume.hlsl PushConstants.
struct ScatterPushConstants {
    uint32_t volumeWidth;
    uint32_t volumeHeight;
    float    screenWidth;
    float    screenHeight;
    uint32_t resetHistory; // 1 on first frame: blend factor zeroed, history ignored
};

// ---------------------------------------------------------------------------
//  loadSpirV
// ---------------------------------------------------------------------------
VkShaderModule ScatteringVolume::loadSpirV(const VulkanDevice& device, const std::string& path) {
    auto code = readFile(path);
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod;
    if (vkCreateShaderModule(device.getLogicalDevice(), &ci, nullptr, &mod) != VK_SUCCESS)
        throw std::runtime_error("ScatteringVolume: failed to load " + path);
    return mod;
}

// ---------------------------------------------------------------------------
//  Internal helpers: create 3D / 2D images
// ---------------------------------------------------------------------------
static VkImage createImage3D(const VulkanDevice& device, uint32_t w, uint32_t h, uint32_t d,
                              VkFormat fmt, VkImageUsageFlags usage, VkDeviceMemory& mem) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = fmt;
    ci.extent = {w, h, d};
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img;
    vkCreateImage(device.getLogicalDevice(), &ci, nullptr, &img);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device.getLogicalDevice(), img, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = device.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device.getLogicalDevice(), &alloc, nullptr, &mem);
    vkBindImageMemory(device.getLogicalDevice(), img, mem, 0);
    return img;
}

static VkImage createImage2D(const VulkanDevice& device, uint32_t w, uint32_t h,
                              VkFormat fmt, VkImageUsageFlags usage, VkDeviceMemory& mem) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img;
    vkCreateImage(device.getLogicalDevice(), &ci, nullptr, &img);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device.getLogicalDevice(), img, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = device.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device.getLogicalDevice(), &alloc, nullptr, &mem);
    vkBindImageMemory(device.getLogicalDevice(), img, mem, 0);
    return img;
}

static VkImageView createView3D(const VulkanDevice& device, VkImage img, VkFormat fmt) {
    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = img; ci.viewType = VK_IMAGE_VIEW_TYPE_3D; ci.format = fmt;
    ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v; vkCreateImageView(device.getLogicalDevice(), &ci, nullptr, &v);
    return v;
}

static VkImageView createView2D(const VulkanDevice& device, VkImage img, VkFormat fmt) {
    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = img; ci.viewType = VK_IMAGE_VIEW_TYPE_2D; ci.format = fmt;
    ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v; vkCreateImageView(device.getLogicalDevice(), &ci, nullptr, &v);
    return v;
}

// Upload pixel data to a device-local image via a staging buffer.
static void uploadToImage(const VulkanDevice& device, VkImage img,
                          uint32_t w, uint32_t h, uint32_t d,
                          VkFormat fmt, const void* data, VkDeviceSize dataBytes,
                          VkImageLayout finalLayout) {
    VkBuffer stageBuf; VkDeviceMemory stageMem;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = dataBytes; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(device.getLogicalDevice(), &bi, nullptr, &stageBuf);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(), stageBuf, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = device.findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device.getLogicalDevice(), &ai, nullptr, &stageMem);
    vkBindBufferMemory(device.getLogicalDevice(), stageBuf, stageMem, 0);
    void* mapped; vkMapMemory(device.getLogicalDevice(), stageMem, 0, dataBytes, 0, &mapped);
    memcpy(mapped, data, dataBytes);
    vkUnmapMemory(device.getLogicalDevice(), stageMem);

    // Single-shot command buffer for upload
    VkCommandBufferAllocateInfo cmdAI{};
    cmdAI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAI.commandPool = device.getCommandPool();
    cmdAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAI.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device.getLogicalDevice(), &cmdAI, &cmd);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = img;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toTransfer.srcAccessMask = 0; toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, d};
    vkCmdCopyBufferToImage(cmd, stageBuf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toFinal = toTransfer;
    toFinal.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toFinal.newLayout = finalLayout;
    toFinal.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toFinal.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toFinal);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(device.getGraphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(device.getGraphicsQueue());
    vkFreeCommandBuffers(device.getLogicalDevice(), device.getCommandPool(), 1, &cmd);
    vkDestroyBuffer(device.getLogicalDevice(), stageBuf, nullptr);
    vkFreeMemory(device.getLogicalDevice(), stageMem, nullptr);
}

// ---------------------------------------------------------------------------
//  createTextures
// ---------------------------------------------------------------------------
void ScatteringVolume::createTextures(const VulkanDevice& device) {
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkImageUsageFlags storageAndSample = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags storageAndTransfer = storageAndSample | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    _scatterTex  = createImage3D(device, _volumeW, _volumeH, SCATTER_VOLUME_DEPTH, fmt, storageAndTransfer, _scatterMem);
    _scatterView = createView3D(device, _scatterTex, fmt);

    _scatterHistoryTex  = createImage3D(device, _volumeW, _volumeH, SCATTER_VOLUME_DEPTH, fmt, storageAndTransfer, _scatterHistoryMem);
    _scatterHistoryView = createView3D(device, _scatterHistoryTex, fmt);

    _accumTex  = createImage3D(device, _volumeW, _volumeH, SCATTER_VOLUME_DEPTH, fmt, storageAndSample, _accumMem);
    _accumView = createView3D(device, _accumTex, fmt);

    spdlog::info("ScatteringVolume: {}×{}×{} RGBA16F (scatter + history + accum)", _volumeW, _volumeH, SCATTER_VOLUME_DEPTH);
}

// ---------------------------------------------------------------------------
//  uploadBlueNoise  – generate 64×64 R8 pseudo blue-noise and upload
// ---------------------------------------------------------------------------
void ScatteringVolume::uploadBlueNoise(const VulkanDevice& device) {
    constexpr uint32_t SZ = 64;
    std::vector<uint8_t> pixels(SZ * SZ);
    // Bayer-based interleaved dither scaled to look like blue noise at medium distance.
    // Pattern adapted from a 64×64 low-discrepancy sequence:
    // use two interleaved 8×8 Bayer matrices shifted in X/Y for de-correlation.
    static const uint8_t bayer8[8][8] = {
        { 0,32, 8,40, 2,34,10,42},
        {48,16,56,24,50,18,58,26},
        {12,44, 4,36,14,46, 6,38},
        {60,28,52,20,62,30,54,22},
        { 3,35,11,43, 1,33, 9,41},
        {51,19,59,27,49,17,57,25},
        {15,47, 7,39,13,45, 5,37},
        {63,31,55,23,61,29,53,21}
    };
    for (uint32_t y = 0; y < SZ; ++y)
        for (uint32_t x = 0; x < SZ; ++x)
            pixels[y * SZ + x] = static_cast<uint8_t>(bayer8[y & 7][x & 7] * 4);

    _blueNoiseTex = createImage2D(device, SZ, SZ, VK_FORMAT_R8_UNORM,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, _blueNoiseMem);
    _blueNoiseView = createView2D(device, _blueNoiseTex, VK_FORMAT_R8_UNORM);
    uploadToImage(device, _blueNoiseTex, SZ, SZ, 1, VK_FORMAT_R8_UNORM,
                  pixels.data(), pixels.size(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// ---------------------------------------------------------------------------
//  uploadPerlinNoise  – generate 32³ R8 Perlin noise and upload
// ---------------------------------------------------------------------------
// Classic Perlin noise (Ken Perlin 2002 improved version).
namespace {
    static int perm[512];
    static const int ptable[256] = {
        151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,
        8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,
        117,35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,
        134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,
        46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,
        200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,250,124,
        123,5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,
        223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,129,22,
        39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,228,251,34,242,
        193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,107,49,192,214,31,
        181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,222,
        114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
    };

    inline float fade(float t) { return t*t*t*(t*(t*6-15)+10); }
    inline float lerpf(float a, float b, float t) { return a+t*(b-a); }
    inline float grad(int h, float x, float y, float z) {
        int k = h & 15;
        float u = k < 8 ? x : y, v = k < 4 ? y : (k==12||k==14 ? x : z);
        return ((k&1) ? -u : u) + ((k&2) ? -v : v);
    }
    inline void initPerlin() {
        for (int i = 0; i < 256; ++i) perm[i] = perm[256+i] = ptable[i];
    }
    inline float perlin3(float x, float y, float z) {
        int X = (int)floorf(x) & 255, Y = (int)floorf(y) & 255, Z = (int)floorf(z) & 255;
        x -= floorf(x); y -= floorf(y); z -= floorf(z);
        float u=fade(x), v=fade(y), w=fade(z);
        int A=perm[X]+Y, AA=perm[A]+Z, AB=perm[A+1]+Z, B=perm[X+1]+Y, BA=perm[B]+Z, BB=perm[B+1]+Z;
        return lerpf(lerpf(lerpf(grad(perm[AA],x,y,z),grad(perm[BA],x-1,y,z),u),
                           lerpf(grad(perm[AB],x,y-1,z),grad(perm[BB],x-1,y-1,z),u),v),
                     lerpf(lerpf(grad(perm[AA+1],x,y,z-1),grad(perm[BA+1],x-1,y,z-1),u),
                           lerpf(grad(perm[AB+1],x,y-1,z-1),grad(perm[BB+1],x-1,y-1,z-1),u),v),w);
    }
    // 3-octave fBm
    inline float fbm(float x, float y, float z) {
        return 0.5f*perlin3(x,y,z) + 0.25f*perlin3(x*2,y*2,z*2) + 0.125f*perlin3(x*4,y*4,z*4);
    }
}

void ScatteringVolume::uploadPerlinNoise(const VulkanDevice& device) {
    constexpr uint32_t SZ = 32;
    initPerlin();
    std::vector<uint8_t> pixels(SZ * SZ * SZ);
    for (uint32_t z = 0; z < SZ; ++z)
        for (uint32_t y = 0; y < SZ; ++y)
            for (uint32_t x = 0; x < SZ; ++x) {
                float n = fbm(x / 8.0f, y / 8.0f, z / 8.0f);
                // Remap [-1,1] → [0,1] and clamp
                n = n * 0.5f + 0.5f;
                n = n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
                pixels[(z * SZ + y) * SZ + x] = static_cast<uint8_t>(n * 255.0f);
            }

    _perlinTex  = createImage3D(device, SZ, SZ, SZ, VK_FORMAT_R8_UNORM,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, _perlinMem);
    _perlinView = createView3D(device, _perlinTex, VK_FORMAT_R8_UNORM);
    uploadToImage(device, _perlinTex, SZ, SZ, SZ, VK_FORMAT_R8_UNORM,
                  pixels.data(), pixels.size(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    spdlog::info("ScatteringVolume: 32³ Perlin noise uploaded");
}

// ---------------------------------------------------------------------------
//  createSampler  – linear clamp for history + Perlin sampling
// ---------------------------------------------------------------------------
void ScatteringVolume::createSampler(const VulkanDevice& device) {
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.minLod = 0.0f; ci.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(device.getLogicalDevice(), &ci, nullptr, &_linearSampler);
}

// ---------------------------------------------------------------------------
//  createScatterDescriptors
//  Set 0 for ScatterVolume kernel (per-frame because of UBO + light index buffers):
//    binding  0: RWTexture3D  scatterOut     (STORAGE_IMAGE)
//    binding  1: cbuffer UBO                (UNIFORM_BUFFER)
//    binding  2: Texture2DArray shadowMaps   (SAMPLED_IMAGE)
//    binding  3: SamplerComparisonState      (SAMPLER)
//    binding  4: Texture3D scatterPrev       (SAMPLED_IMAGE, history)
//    binding  5: Texture2D blueNoise         (SAMPLED_IMAGE)
//    binding  6: StructuredBuffer pointData  (STORAGE_BUFFER)
//    binding  7: StructuredBuffer pointIdx   (STORAGE_BUFFER, per-frame)
//    binding  8: StructuredBuffer spotData   (STORAGE_BUFFER)
//    binding  9: StructuredBuffer spotIdx    (STORAGE_BUFFER, per-frame)
//    binding 10: Texture2DArray spotShadows  (SAMPLED_IMAGE)
//    binding 11: SamplerComparisonState      (SAMPLER, spot shadow)
//    binding 12: StructuredBuffer spotVP     (STORAGE_BUFFER)
//    binding 13: Texture3D perlinNoise       (SAMPLED_IMAGE)
//    binding 14: SamplerState linearSampler  (SAMPLER)
// ---------------------------------------------------------------------------
void ScatteringVolume::createScatterDescriptors(
    const VulkanDevice& device,
    const std::vector<VkBuffer>& uniformBuffers,
    VkImageView shadowMapView, VkSampler shadowSampler,
    const ScatterLightResources& lightRes,
    uint32_t framesInFlight)
{
    // --- layout ---
    std::array<VkDescriptorSetLayoutBinding, 15> bindings{};
    auto mkB = [](uint32_t b, VkDescriptorType t) {
        VkDescriptorSetLayoutBinding r{};
        r.binding = b; r.descriptorType = t; r.descriptorCount = 1;
        r.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; return r;
    };
    bindings[ 0] = mkB(0,  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    bindings[ 1] = mkB(1,  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    bindings[ 2] = mkB(2,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    bindings[ 3] = mkB(3,  VK_DESCRIPTOR_TYPE_SAMPLER);
    bindings[ 4] = mkB(4,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    bindings[ 5] = mkB(5,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    bindings[ 6] = mkB(6,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bindings[ 7] = mkB(7,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bindings[ 8] = mkB(8,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bindings[ 9] = mkB(9,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bindings[10] = mkB(10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    bindings[11] = mkB(11, VK_DESCRIPTOR_TYPE_SAMPLER);
    bindings[12] = mkB(12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bindings[13] = mkB(13, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    bindings[14] = mkB(14, VK_DESCRIPTOR_TYPE_SAMPLER);

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutCI.pBindings = bindings.data();
    vkCreateDescriptorSetLayout(device.getLogicalDevice(), &layoutCI, nullptr, &_scatterSetLayout);

    // --- pool ---
    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   framesInFlight},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  framesInFlight},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   5 * framesInFlight}, // 2,4,5,10,13
        {VK_DESCRIPTOR_TYPE_SAMPLER,         3 * framesInFlight}, // 3,11,14
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  5 * framesInFlight}, // 6,7,8,9,12
    };
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = framesInFlight;
    poolCI.poolSizeCount = 5; poolCI.pPoolSizes = poolSizes;
    vkCreateDescriptorPool(device.getLogicalDevice(), &poolCI, nullptr, &_scatterPool);

    // --- allocate ---
    std::vector<VkDescriptorSetLayout> layouts(framesInFlight, _scatterSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = _scatterPool;
    allocInfo.descriptorSetCount = framesInFlight;
    allocInfo.pSetLayouts = layouts.data();
    _scatterSets.resize(framesInFlight);
    vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo, _scatterSets.data());

    // Immutable image infos (same for all frames)
    VkDescriptorImageInfo scatterOutInfo{};
    scatterOutInfo.imageView = _scatterView; scatterOutInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo shadowInfo{};
    shadowInfo.imageView = shadowMapView;
    shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;

    VkDescriptorImageInfo samplerInfo{}; samplerInfo.sampler = shadowSampler;

    VkDescriptorImageInfo historyInfo{};
    historyInfo.imageView = _scatterHistoryView;
    historyInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo blueNoiseInfo{};
    blueNoiseInfo.imageView = _blueNoiseView;
    blueNoiseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo spotShadowInfo{};
    spotShadowInfo.imageView = lightRes.spotShadowMapView;
    spotShadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo spotSamplerInfo{}; spotSamplerInfo.sampler = lightRes.spotShadowSampler;

    VkDescriptorImageInfo perlinInfo{};
    perlinInfo.imageView = _perlinView;
    perlinInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo linearSamplerInfo{}; linearSamplerInfo.sampler = _linearSampler;

    VkDescriptorBufferInfo pointDataInfo{lightRes.pointLightDataBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo spotDataInfo{lightRes.spotLightDataBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo spotVPInfo{lightRes.spotViewProjBuffer, 0, VK_WHOLE_SIZE};

    for (uint32_t i = 0; i < framesInFlight; ++i) {
        VkDescriptorBufferInfo uboInfo{uniformBuffers[i], 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo pointIdxInfo{lightRes.pointLightIndexBuffers[i], 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo spotIdxInfo{lightRes.spotLightIndexBuffers[i], 0, VK_WHOLE_SIZE};

        auto wImg  = [&](uint32_t b, VkDescriptorType t, const VkDescriptorImageInfo& ii) {
            VkWriteDescriptorSet w{}; w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = _scatterSets[i]; w.dstBinding = b;
            w.descriptorCount = 1; w.descriptorType = t; w.pImageInfo = &ii; return w; };
        auto wBuf  = [&](uint32_t b, const VkDescriptorBufferInfo& bi) {
            VkWriteDescriptorSet w{}; w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = _scatterSets[i]; w.dstBinding = b;
            w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi; return w; };

        std::array<VkWriteDescriptorSet, 15> ws{
            wImg(0,  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  scatterOutInfo),
            VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                _scatterSets[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo},
            wImg(2,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  shadowInfo),
            wImg(3,  VK_DESCRIPTOR_TYPE_SAMPLER,        samplerInfo),
            wImg(4,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  historyInfo),
            wImg(5,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  blueNoiseInfo),
            wBuf(6,  pointDataInfo),
            wBuf(7,  pointIdxInfo),
            wBuf(8,  spotDataInfo),
            wBuf(9,  spotIdxInfo),
            wImg(10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  spotShadowInfo),
            wImg(11, VK_DESCRIPTOR_TYPE_SAMPLER,        spotSamplerInfo),
            wBuf(12, spotVPInfo),
            wImg(13, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  perlinInfo),
            wImg(14, VK_DESCRIPTOR_TYPE_SAMPLER,        linearSamplerInfo),
        };
        vkUpdateDescriptorSets(device.getLogicalDevice(),
                               static_cast<uint32_t>(ws.size()), ws.data(), 0, nullptr);
    }
}

// ---------------------------------------------------------------------------
//  createAccumDescriptors
// ---------------------------------------------------------------------------
void ScatteringVolume::createAccumDescriptors(const VulkanDevice& device) {
    std::array<VkDescriptorSetLayoutBinding, 2> b{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 2; layoutCI.pBindings = b.data();
    vkCreateDescriptorSetLayout(device.getLogicalDevice(), &layoutCI, nullptr, &_accumSetLayout);

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}};
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = 1; poolCI.poolSizeCount = 2; poolCI.pPoolSizes = ps;
    vkCreateDescriptorPool(device.getLogicalDevice(), &poolCI, nullptr, &_accumPool);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = _accumPool; allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &_accumSetLayout;
    vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo, &_accumSet);

    VkDescriptorImageInfo sr{};
    sr.imageView = _scatterView; sr.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo aw{};
    aw.imageView = _accumView; aw.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 2> ws{};
    ws[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _accumSet,
             0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &sr};
    ws[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _accumSet,
             1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &aw};
    vkUpdateDescriptorSets(device.getLogicalDevice(), 2, ws.data(), 0, nullptr);
}

// ---------------------------------------------------------------------------
//  createPipelines
// ---------------------------------------------------------------------------
void ScatteringVolume::createPipelines(const VulkanDevice& device, const std::filesystem::path& rootPath) {
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset = 0; pcRange.size = sizeof(ScatterPushConstants);

    {
        VkPipelineLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        li.setLayoutCount = 1; li.pSetLayouts = &_scatterSetLayout;
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pcRange;
        vkCreatePipelineLayout(device.getLogicalDevice(), &li, nullptr, &_scatterLayout);

        auto spv = loadSpirV(device, (rootPath / "shaders/scattervolume.cs.spv").generic_string());
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; stage.module = spv; stage.pName = "ScatterVolume";
        VkComputePipelineCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pci.layout = _scatterLayout; pci.stage = stage;
        vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &pci, nullptr, &_scatterPipe);
        vkDestroyShaderModule(device.getLogicalDevice(), spv, nullptr);
    }
    {
        VkPipelineLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        li.setLayoutCount = 1; li.pSetLayouts = &_accumSetLayout;
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pcRange;
        vkCreatePipelineLayout(device.getLogicalDevice(), &li, nullptr, &_accumLayout);

        auto spv = loadSpirV(device, (rootPath / "shaders/accumscatter.cs.spv").generic_string());
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; stage.module = spv; stage.pName = "AccumulateScattering";
        VkComputePipelineCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pci.layout = _accumLayout; pci.stage = stage;
        vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &pci, nullptr, &_accumPipe);
        vkDestroyShaderModule(device.getLogicalDevice(), spv, nullptr);
    }
    spdlog::info("ScatteringVolume: pipelines created");
}

// ---------------------------------------------------------------------------
//  create  – public entry point
// ---------------------------------------------------------------------------
void ScatteringVolume::create(
    const VulkanDevice&           device,
    const std::filesystem::path&  rootPath,
    uint32_t screenW, uint32_t screenH,
    uint32_t framesInFlight,
    const std::vector<VkBuffer>&  uniformBuffers,
    VkImageView shadowMapView, VkSampler shadowSampler,
    const ScatterLightResources&  lightRes)
{
    _volumeW = (screenW + SCATTER_TILE_SIZE - 1) / SCATTER_TILE_SIZE;
    _volumeH = (screenH + SCATTER_TILE_SIZE - 1) / SCATTER_TILE_SIZE;
    _framesInFlight = framesInFlight;

    createTextures(device);
    uploadBlueNoise(device);
    uploadPerlinNoise(device);
    createSampler(device);
    createScatterDescriptors(device, uniformBuffers, shadowMapView, shadowSampler, lightRes, framesInFlight);
    createAccumDescriptors(device);
    createPipelines(device, rootPath);
}

// ---------------------------------------------------------------------------
//  Helper: image memory barrier (reduces boilerplate in dispatch)
// ---------------------------------------------------------------------------
static void imgBarrier(VkCommandBuffer cmd,
                       VkImage img, VkImageLayout from, VkImageLayout to,
                       VkAccessFlags src, VkAccessFlags dst,
                       VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = src; b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ---------------------------------------------------------------------------
//  dispatch  – called each frame
// ---------------------------------------------------------------------------
void ScatteringVolume::dispatch(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (_scatterPipe == VK_NULL_HANDLE) return;

    const bool resetHistory = _firstDispatch;
    _firstDispatch = false;

    ScatterPushConstants pc{
        _volumeW, _volumeH,
        static_cast<float>(_volumeW * SCATTER_TILE_SIZE),
        static_cast<float>(_volumeH * SCATTER_TILE_SIZE),
        resetHistory ? 1u : 0u
    };

    // On first dispatch initialise history texture to SHADER_READ_ONLY_OPTIMAL
    // (the scatter shader will ignore its content due to resetHistory=1).
    if (resetHistory) {
        imgBarrier(cmd, _scatterHistoryTex,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   0, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    // Scatter texture and accum texture start undefined each frame (overwritten).
    imgBarrier(cmd, _scatterTex,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
               0, VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    imgBarrier(cmd, _accumTex,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
               0, VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // ==================== Pass 1: ScatterVolume ====================
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _scatterPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _scatterLayout,
                            0, 1, &_scatterSets[frameIndex], 0, nullptr);
    vkCmdPushConstants(cmd, _scatterLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (_volumeW+3)/4, (_volumeH+3)/4, (SCATTER_VOLUME_DEPTH+3)/4);

    // Scatter write → scatter read by accum kernel
    imgBarrier(cmd, _scatterTex,
               VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // ================== Pass 2: AccumulateScattering ==================
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _accumPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _accumLayout,
                            0, 1, &_accumSet, 0, nullptr);
    vkCmdPushConstants(cmd, _accumLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (_volumeW+7)/8, (_volumeH+7)/8, 1);

    // Accum write → fragment shader read
    imgBarrier(cmd, _accumTex,
               VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // ========== Copy scatter → history for next frame's reprojection ==========
    // Need to transition _scatterTex to TRANSFER_SRC first.
    imgBarrier(cmd, _scatterTex,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    imgBarrier(cmd, _scatterHistoryTex,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageCopy copyRegion{};
    copyRegion.srcSubresource = copyRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.extent = {_volumeW, _volumeH, SCATTER_VOLUME_DEPTH};
    vkCmdCopyImage(cmd, _scatterTex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   _scatterHistoryTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    // Restore both to SHADER_READ_ONLY_OPTIMAL ready for next frame.
    imgBarrier(cmd, _scatterHistoryTex,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    imgBarrier(cmd, _scatterTex,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

// ---------------------------------------------------------------------------
//  destroy
// ---------------------------------------------------------------------------
void ScatteringVolume::destroy(const VulkanDevice& device) {
    VkDevice dev = device.getLogicalDevice();
    auto dp = [dev](auto h, auto fn) { if (h != VK_NULL_HANDLE) fn(dev, h, nullptr); };
    dp(_scatterPipe,   vkDestroyPipeline);
    dp(_accumPipe,     vkDestroyPipeline);
    dp(_scatterLayout, vkDestroyPipelineLayout);
    dp(_accumLayout,   vkDestroyPipelineLayout);
    dp(_scatterPool,   vkDestroyDescriptorPool);
    dp(_accumPool,     vkDestroyDescriptorPool);
    dp(_scatterSetLayout, vkDestroyDescriptorSetLayout);
    dp(_accumSetLayout,   vkDestroyDescriptorSetLayout);
    dp(_scatterView,        vkDestroyImageView);
    dp(_scatterHistoryView, vkDestroyImageView);
    dp(_accumView,          vkDestroyImageView);
    dp(_blueNoiseView,      vkDestroyImageView);
    dp(_perlinView,         vkDestroyImageView);
    dp(_scatterTex,        vkDestroyImage);
    dp(_scatterHistoryTex, vkDestroyImage);
    dp(_accumTex,          vkDestroyImage);
    dp(_blueNoiseTex,      vkDestroyImage);
    dp(_perlinTex,         vkDestroyImage);
    dp(_linearSampler,     vkDestroySampler);
    vkFreeMemory(dev, _scatterMem,        nullptr);
    vkFreeMemory(dev, _scatterHistoryMem, nullptr);
    vkFreeMemory(dev, _accumMem,          nullptr);
    vkFreeMemory(dev, _blueNoiseMem,      nullptr);
    vkFreeMemory(dev, _perlinMem,         nullptr);
}
