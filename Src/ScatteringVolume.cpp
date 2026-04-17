#include "Include/ScatteringVolume.h"
#include "Include/VulkanSetup.h"
#include "Include/Common.h"       // readFile
#include "spdlog/spdlog.h"

#include <stdexcept>
#include <array>

// Push-constant block – matches scattervolume.hlsl
struct ScatterPushConstants {
    uint32_t volumeWidth;
    uint32_t volumeHeight;
    float    screenWidth;
    float    screenHeight;
};

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
VkShaderModule ScatteringVolume::loadSpirV(VulkanDevice& device, const std::string& path) {
    auto code = readFile(path);
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod;
    if (vkCreateShaderModule(device.getLogicalDevice(), &ci, nullptr, &mod) != VK_SUCCESS)
        throw std::runtime_error("ScatteringVolume: failed to load SPIR-V: " + path);
    return mod;
}

static VkImage createImage3D(VulkanDevice& device, uint32_t w, uint32_t h, uint32_t d,
                              VkFormat fmt, VkImageUsageFlags usage,
                              VkDeviceMemory& mem) {
    VkImageCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType     = VK_IMAGE_TYPE_3D;
    ci.format        = fmt;
    ci.extent        = {w, h, d};
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ci.usage         = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage img;
    vkCreateImage(device.getLogicalDevice(), &ci, nullptr, &img);

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device.getLogicalDevice(), img, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = device.findMemoryType(req.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device.getLogicalDevice(), &alloc, nullptr, &mem);
    vkBindImageMemory(device.getLogicalDevice(), img, mem, 0);
    return img;
}

static VkImageView createImageView3D(VulkanDevice& device, VkImage img, VkFormat fmt) {
    VkImageViewCreateInfo ci{};
    ci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image                           = img;
    ci.viewType                        = VK_IMAGE_VIEW_TYPE_3D;
    ci.format                          = fmt;
    ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    ci.subresourceRange.baseMipLevel   = 0;
    ci.subresourceRange.levelCount     = 1;
    ci.subresourceRange.baseArrayLayer = 0;
    ci.subresourceRange.layerCount     = 1;
    VkImageView view;
    vkCreateImageView(device.getLogicalDevice(), &ci, nullptr, &view);
    return view;
}

// ---------------------------------------------------------------------------
//  createTextures
// ---------------------------------------------------------------------------
void ScatteringVolume::createTextures(VulkanDevice& device) {
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;

    // Scatter texture: written by ScatterVolume kernel, read by AccumulateScattering
    _scatterTex  = createImage3D(device, _volumeW, _volumeH, SCATTER_VOLUME_DEPTH, fmt,
                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 _scatterMem);
    _scatterView = createImageView3D(device, _scatterTex, fmt);

    // Accum texture: written by AccumulateScattering, sampled by deferred lighting
    _accumTex  = createImage3D(device, _volumeW, _volumeH, SCATTER_VOLUME_DEPTH, fmt,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               _accumMem);
    _accumView = createImageView3D(device, _accumTex, fmt);

    spdlog::info("ScatteringVolume: textures {}×{}×{} (RGBA16F)", _volumeW, _volumeH, SCATTER_VOLUME_DEPTH);
}

// ---------------------------------------------------------------------------
//  createScatterDescriptors
//  Set 0 layout for ScatterVolume kernel:
//    binding 0: RWTexture3D  (STORAGE_IMAGE)   – scatterOut
//    binding 1: UBO          (UNIFORM_BUFFER)   – camera + frame constants
//    binding 2: Texture2DArray (SAMPLED_IMAGE)  – shadow map
//    binding 3: SamplerComparisonState (SAMPLER)
// ---------------------------------------------------------------------------
void ScatteringVolume::createScatterDescriptors(
    VulkanDevice& device,
    const std::vector<VkBuffer>& uniformBuffers,
    VkImageView  shadowMapView,
    VkSampler    shadowSampler,
    uint32_t     framesInFlight)
{
    // --- layout ---
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_SAMPLER,         1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutCI.pBindings    = bindings.data();
    vkCreateDescriptorSetLayout(device.getLogicalDevice(), &layoutCI, nullptr, &_scatterSetLayout);

    // --- pool ---
    std::array<VkDescriptorPoolSize, 4> poolSizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  framesInFlight},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, framesInFlight},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  framesInFlight},
        {VK_DESCRIPTOR_TYPE_SAMPLER,        framesInFlight},
    }};
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = framesInFlight;
    poolCI.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolCI.pPoolSizes    = poolSizes.data();
    vkCreateDescriptorPool(device.getLogicalDevice(), &poolCI, nullptr, &_scatterPool);

    // --- sets (one per frame) ---
    std::vector<VkDescriptorSetLayout> layouts(framesInFlight, _scatterSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = _scatterPool;
    allocInfo.descriptorSetCount = framesInFlight;
    allocInfo.pSetLayouts        = layouts.data();
    _scatterSets.resize(framesInFlight);
    vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo, _scatterSets.data());

    // Binding 2 and 3 are the same for all frames
    VkDescriptorImageInfo shadowInfo{};
    shadowInfo.imageView   = shadowMapView;
    shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;

    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler = shadowSampler;

    // Binding 0 is also the same (scatter out texture doesn't change)
    VkDescriptorImageInfo scatterOutInfo{};
    scatterOutInfo.imageView   = _scatterView;
    scatterOutInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    for (uint32_t i = 0; i < framesInFlight; ++i) {
        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = uniformBuffers[i];
        uboInfo.offset = 0;
        uboInfo.range  = VK_WHOLE_SIZE;

        std::array<VkWriteDescriptorSet, 4> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _scatterSets[i],
                     0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   &scatterOutInfo, nullptr, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _scatterSets[i],
                     1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  nullptr, &uboInfo, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _scatterSets[i],
                     2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   &shadowInfo, nullptr, nullptr};
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _scatterSets[i],
                     3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,         &samplerInfo, nullptr, nullptr};
        vkUpdateDescriptorSets(device.getLogicalDevice(),
                               static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

// ---------------------------------------------------------------------------
//  createAccumDescriptors
//  Set 0 layout for AccumulateScattering kernel:
//    binding 0: Texture3D   (SAMPLED_IMAGE)  – scatterIn
//    binding 1: RWTexture3D (STORAGE_IMAGE)  – accumOut
// ---------------------------------------------------------------------------
void ScatteringVolume::createAccumDescriptors(VulkanDevice& device) {
    // --- layout ---
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutCI.pBindings    = bindings.data();
    vkCreateDescriptorSetLayout(device.getLogicalDevice(), &layoutCI, nullptr, &_accumSetLayout);

    // --- pool (1 set) ---
    std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
    }};
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolCI.pPoolSizes    = poolSizes.data();
    vkCreateDescriptorPool(device.getLogicalDevice(), &poolCI, nullptr, &_accumPool);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = _accumPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &_accumSetLayout;
    vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo, &_accumSet);

    VkDescriptorImageInfo scatterReadInfo{};
    scatterReadInfo.imageView   = _scatterView;
    scatterReadInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo accumWriteInfo{};
    accumWriteInfo.imageView   = _accumView;
    accumWriteInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _accumSet,
                 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &scatterReadInfo, nullptr, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _accumSet,
                 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &accumWriteInfo, nullptr, nullptr};
    vkUpdateDescriptorSets(device.getLogicalDevice(),
                           static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

// ---------------------------------------------------------------------------
//  createPipelines
// ---------------------------------------------------------------------------
void ScatteringVolume::createPipelines(VulkanDevice& device, const std::filesystem::path& rootPath) {
    // Push-constant range (shared by both pipelines)
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(ScatterPushConstants);

    // ---- ScatterVolume pipeline ----
    {
        VkPipelineLayoutCreateInfo layoutCI{};
        layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutCI.setLayoutCount         = 1;
        layoutCI.pSetLayouts            = &_scatterSetLayout;
        layoutCI.pushConstantRangeCount = 1;
        layoutCI.pPushConstantRanges    = &pcRange;
        vkCreatePipelineLayout(device.getLogicalDevice(), &layoutCI, nullptr, &_scatterLayout);

        auto spv = loadSpirV(device, (rootPath / "shaders/scattervolume.cs.spv").generic_string());
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = spv;
        stage.pName  = "ScatterVolume";

        VkComputePipelineCreateInfo pipeCI{};
        pipeCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeCI.layout = _scatterLayout;
        pipeCI.stage  = stage;
        vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &pipeCI, nullptr, &_scatterPipe);
        vkDestroyShaderModule(device.getLogicalDevice(), spv, nullptr);
    }

    // ---- AccumulateScattering pipeline ----
    {
        VkPipelineLayoutCreateInfo layoutCI{};
        layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutCI.setLayoutCount         = 1;
        layoutCI.pSetLayouts            = &_accumSetLayout;
        layoutCI.pushConstantRangeCount = 1;
        layoutCI.pPushConstantRanges    = &pcRange;
        vkCreatePipelineLayout(device.getLogicalDevice(), &layoutCI, nullptr, &_accumLayout);

        auto spv = loadSpirV(device, (rootPath / "shaders/accumscatter.cs.spv").generic_string());
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = spv;
        stage.pName  = "AccumulateScattering";

        VkComputePipelineCreateInfo pipeCI{};
        pipeCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeCI.layout = _accumLayout;
        pipeCI.stage  = stage;
        vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &pipeCI, nullptr, &_accumPipe);
        vkDestroyShaderModule(device.getLogicalDevice(), spv, nullptr);
    }

    spdlog::info("ScatteringVolume: pipelines created");
}

// ---------------------------------------------------------------------------
//  create  – public entry point
// ---------------------------------------------------------------------------
void ScatteringVolume::create(
    VulkanDevice&                   device,
    const std::filesystem::path&    rootPath,
    uint32_t                        screenW,
    uint32_t                        screenH,
    uint32_t                        framesInFlight,
    const std::vector<VkBuffer>&    uniformBuffers,
    VkImageView                     shadowMapView,
    VkSampler                       shadowSampler)
{
    _volumeW = (screenW + SCATTER_TILE_SIZE - 1) / SCATTER_TILE_SIZE;
    _volumeH = (screenH + SCATTER_TILE_SIZE - 1) / SCATTER_TILE_SIZE;

    createTextures(device);
    createScatterDescriptors(device, uniformBuffers, shadowMapView, shadowSampler, framesInFlight);
    createAccumDescriptors(device);
    createPipelines(device, rootPath);
}

// ---------------------------------------------------------------------------
//  dispatch  – called each frame before the deferred lighting pass
// ---------------------------------------------------------------------------
void ScatteringVolume::dispatch(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (_scatterPipe == VK_NULL_HANDLE) return;

    ScatterPushConstants pc{
        _volumeW, _volumeH,
        static_cast<float>(_volumeW * SCATTER_TILE_SIZE),
        static_cast<float>(_volumeH * SCATTER_TILE_SIZE)
    };

    // ---- Transition scatter texture → GENERAL for compute write ----
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = _scatterTex;
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ---- Transition accum texture → GENERAL for compute write ----
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = _accumTex;
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ======================== Pass 1: ScatterVolume ========================
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _scatterPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _scatterLayout,
                            0, 1, &_scatterSets[frameIndex], 0, nullptr);
    vkCmdPushConstants(cmd, _scatterLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);
    // Dispatch: 4×4×4 threads per group
    vkCmdDispatch(cmd,
        (_volumeW  + 3) / 4,
        (_volumeH  + 3) / 4,
        (SCATTER_VOLUME_DEPTH + 3) / 4);

    // ---- Barrier: scatter write → scatter read by accum kernel ----
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = _scatterTex;
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ====================== Pass 2: AccumulateScattering ==================
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _accumPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _accumLayout,
                            0, 1, &_accumSet, 0, nullptr);
    vkCmdPushConstants(cmd, _accumLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);
    // Dispatch: 8×8×1 threads per group
    vkCmdDispatch(cmd,
        (_volumeW + 7) / 8,
        (_volumeH + 7) / 8,
        1);

    // ---- Barrier: accum write → fragment shader read ----
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = _accumTex;
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }
}

// ---------------------------------------------------------------------------
//  destroy
// ---------------------------------------------------------------------------
void ScatteringVolume::destroy(VulkanDevice& device) {
    VkDevice dev = device.getLogicalDevice();
    vkDestroyPipeline(dev, _scatterPipe, nullptr);
    vkDestroyPipeline(dev, _accumPipe,   nullptr);
    vkDestroyPipelineLayout(dev, _scatterLayout, nullptr);
    vkDestroyPipelineLayout(dev, _accumLayout,   nullptr);
    vkDestroyDescriptorPool(dev, _scatterPool, nullptr);
    vkDestroyDescriptorPool(dev, _accumPool,   nullptr);
    vkDestroyDescriptorSetLayout(dev, _scatterSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(dev, _accumSetLayout,   nullptr);
    vkDestroyImageView(dev, _scatterView, nullptr);
    vkDestroyImageView(dev, _accumView,   nullptr);
    vkDestroyImage(dev, _scatterTex, nullptr);
    vkDestroyImage(dev, _accumTex,   nullptr);
    vkFreeMemory(dev, _scatterMem, nullptr);
    vkFreeMemory(dev, _accumMem,   nullptr);
}
