#include "Raytracing.h"

#include "GpuScene.h"
#include "spdlog/spdlog.h"

#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {
constexpr VkDeviceSize alignUp(VkDeviceSize x, VkDeviceSize a) {
  return (x + a - 1) & ~(a - 1);
}
}  // namespace

RayTracing::RayTracing(VulkanDevice &device, GpuScene &scene)
    : _device(device), _scene(scene) {}

RayTracing::~RayTracing() {
  VkDevice dev = _device.getLogicalDevice();
  if (_tlas != VK_NULL_HANDLE && pfnDestroyAccelerationStructure) {
    pfnDestroyAccelerationStructure(dev, _tlas, nullptr);
  }
  if (_blas != VK_NULL_HANDLE && pfnDestroyAccelerationStructure) {
    pfnDestroyAccelerationStructure(dev, _blas, nullptr);
  }
  auto destroyBuf = [dev](VkBuffer &b, VkDeviceMemory &m) {
    if (b != VK_NULL_HANDLE) vkDestroyBuffer(dev, b, nullptr);
    if (m != VK_NULL_HANDLE) vkFreeMemory(dev, m, nullptr);
    b = VK_NULL_HANDLE;
    m = VK_NULL_HANDLE;
  };
  destroyBuf(_blasBuffer, _blasBufferMemory);
  destroyBuf(_tlasBuffer, _tlasBufferMemory);
  destroyBuf(_instancesBuffer, _instancesBufferMemory);
  destroyBuf(_sbtBuffer, _sbtMemory);

  for (size_t i = 0; i < _rtLitImage.size(); ++i) {
    if (_rtLitImageView[i] != VK_NULL_HANDLE)
      vkDestroyImageView(dev, _rtLitImageView[i], nullptr);
    if (_rtLitImage[i] != VK_NULL_HANDLE)
      vkDestroyImage(dev, _rtLitImage[i], nullptr);
    if (_rtLitMemory[i] != VK_NULL_HANDLE)
      vkFreeMemory(dev, _rtLitMemory[i], nullptr);
  }
  if (_rtPipeline != VK_NULL_HANDLE)
    vkDestroyPipeline(dev, _rtPipeline, nullptr);
  if (_rtPipelineLayout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(dev, _rtPipelineLayout, nullptr);
  if (_rtShaderModule != VK_NULL_HANDLE)
    vkDestroyShaderModule(dev, _rtShaderModule, nullptr);
  if (_rtDescriptorPool != VK_NULL_HANDLE)
    vkDestroyDescriptorPool(dev, _rtDescriptorPool, nullptr);
  if (_rtSetLayout != VK_NULL_HANDLE)
    vkDestroyDescriptorSetLayout(dev, _rtSetLayout, nullptr);
  for (auto fb : _rtImguiFrameBuffer)
    if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(dev, fb, nullptr);
  if (_rtImguiPass != VK_NULL_HANDLE)
    vkDestroyRenderPass(dev, _rtImguiPass, nullptr);
}

void RayTracing::Init() {
  loadFunctionPointers();
}

void RayTracing::loadFunctionPointers() {
  VkDevice dev = _device.getLogicalDevice();
#define LOAD_DEV(name) \
  pfn##name = reinterpret_cast<PFN_vk##name##KHR>(vkGetDeviceProcAddr(dev, "vk" #name "KHR")); \
  if (!pfn##name) throw std::runtime_error("RT: failed to load vk" #name "KHR");

  LOAD_DEV(GetBufferDeviceAddress)
  LOAD_DEV(CreateAccelerationStructure)
  LOAD_DEV(DestroyAccelerationStructure)
  LOAD_DEV(GetAccelerationStructureBuildSizes)
  LOAD_DEV(CmdBuildAccelerationStructures)
  LOAD_DEV(GetAccelerationStructureDeviceAddress)
  LOAD_DEV(CreateRayTracingPipelines)
  LOAD_DEV(GetRayTracingShaderGroupHandles)
  LOAD_DEV(CmdTraceRays)
#undef LOAD_DEV
  spdlog::info("RT: function pointers loaded.");
}

VkDeviceAddress RayTracing::getBufferDeviceAddress(VkBuffer buffer) const {
  VkBufferDeviceAddressInfo info{
      VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, buffer};
  return pfnGetBufferDeviceAddress(_device.getLogicalDevice(), &info);
}

void RayTracing::allocateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags memProps,
                                VkBuffer &outBuffer,
                                VkDeviceMemory &outMemory,
                                bool deviceAddress) {
  VkDevice dev = _device.getLogicalDevice();
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage | (deviceAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(dev, &bi, nullptr, &outBuffer) != VK_SUCCESS) {
    throw std::runtime_error("RT: vkCreateBuffer failed");
  }
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(dev, outBuffer, &req);
  VkMemoryAllocateFlagsInfo flagsInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flagsInfo.flags = deviceAddress ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR : 0;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.pNext = deviceAddress ? &flagsInfo : nullptr;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = _device.findMemoryType(req.memoryTypeBits, memProps);
  if (vkAllocateMemory(dev, &ai, nullptr, &outMemory) != VK_SUCCESS) {
    throw std::runtime_error("RT: vkAllocateMemory failed");
  }
  vkBindBufferMemory(dev, outBuffer, outMemory, 0);
}

void RayTracing::BuildAccelerationStructures() {
  buildBLAS();
  buildTLAS();
  spdlog::info("RT: BLAS+TLAS built. blasAddr=0x{:x}", _blasAddress);
}

void RayTracing::buildBLAS() {
  VkDevice dev = _device.getLogicalDevice();

  const uint32_t opaqueCount = static_cast<uint32_t>(_scene.applMesh->_opaqueChunkCount);
  const uint32_t alphaCount = static_cast<uint32_t>(_scene.applMesh->_alphaMaskedChunkCount);
  const uint32_t geomCount = opaqueCount + alphaCount;
  if (geomCount == 0) {
    throw std::runtime_error("RT: no geometry to build BLAS from");
  }

  const VkDeviceAddress vbAddr = getBufferDeviceAddress(_scene.applVertexBuffer);
  const VkDeviceAddress ibAddr = getBufferDeviceAddress(_scene.applIndexBuffer);
  const uint32_t vertexCount = static_cast<uint32_t>(_scene.applMesh->_vertexCount);

  std::vector<VkAccelerationStructureGeometryKHR> geometries(geomCount);
  std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(geomCount);
  std::vector<uint32_t> primitiveCounts(geomCount);

  for (uint32_t g = 0; g < geomCount; ++g) {
    const AAPLMeshChunk &chunk = _scene.m_Chunks[g];
    VkAccelerationStructureGeometryKHR &geo = geometries[g];
    geo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo.flags = (g < opaqueCount) ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;

    auto &tri = geo.geometry.triangles;
    tri = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tri.vertexData.deviceAddress = vbAddr;
    tri.vertexStride = sizeof(float) * 3;
    tri.maxVertex = vertexCount - 1;
    tri.indexType = VK_INDEX_TYPE_UINT32;
    tri.indexData.deviceAddress = ibAddr + static_cast<VkDeviceAddress>(chunk.indexBegin) * sizeof(uint32_t);
    tri.transformData.deviceAddress = 0;

    primitiveCounts[g] = chunk.indexCount / 3;
    ranges[g].primitiveCount = primitiveCounts[g];
    ranges[g].primitiveOffset = 0;
    ranges[g].firstVertex = 0;
    ranges[g].transformOffset = 0;
  }

  VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  buildInfo.geometryCount = geomCount;
  buildInfo.pGeometries = geometries.data();

  VkAccelerationStructureBuildSizesInfoKHR sizesInfo{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  pfnGetAccelerationStructureBuildSizes(
      dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
      primitiveCounts.data(), &sizesInfo);

  // 1) AS storage
  allocateBuffer(sizesInfo.accelerationStructureSize,
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 _blasBuffer, _blasBufferMemory, true);

  VkAccelerationStructureCreateInfoKHR asCreate{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  asCreate.buffer = _blasBuffer;
  asCreate.size = sizesInfo.accelerationStructureSize;
  asCreate.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  if (pfnCreateAccelerationStructure(dev, &asCreate, nullptr, &_blas) != VK_SUCCESS) {
    throw std::runtime_error("RT: vkCreateAccelerationStructureKHR (BLAS) failed");
  }

  // 2) Scratch
  VkBuffer scratchBuf = VK_NULL_HANDLE;
  VkDeviceMemory scratchMem = VK_NULL_HANDLE;
  const VkDeviceSize scratchAlign = _device.getASProperties().minAccelerationStructureScratchOffsetAlignment;
  const VkDeviceSize scratchSize = alignUp(sizesInfo.buildScratchSize,
                                            scratchAlign ? scratchAlign : 256);
  allocateBuffer(scratchSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 scratchBuf, scratchMem, true);

  buildInfo.dstAccelerationStructure = _blas;
  buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(scratchBuf);

  std::vector<const VkAccelerationStructureBuildRangeInfoKHR *> rangePtrs(1);
  rangePtrs[0] = ranges.data();

  VkCommandBuffer cmd = _device.beginSingleTimeCommands();
  pfnCmdBuildAccelerationStructures(cmd, 1, &buildInfo, rangePtrs.data());
  // Memory barrier: BLAS build write -> TLAS build read
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       0, 1, &mb, 0, nullptr, 0, nullptr);
  _device.endSingleTimeCommands(cmd);

  // Cleanup scratch
  vkDestroyBuffer(dev, scratchBuf, nullptr);
  vkFreeMemory(dev, scratchMem, nullptr);

  // Query BLAS device address
  VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  addrInfo.accelerationStructure = _blas;
  _blasAddress = pfnGetAccelerationStructureDeviceAddress(dev, &addrInfo);

  spdlog::info("RT: BLAS built with {} geometries ({} opaque + {} alpha-mask), AS size={} bytes, scratch={} bytes",
               geomCount, opaqueCount, alphaCount,
               (uint64_t)sizesInfo.accelerationStructureSize,
               (uint64_t)sizesInfo.buildScratchSize);
}

void RayTracing::buildTLAS() {
  VkDevice dev = _device.getLogicalDevice();

  // 1 instance, identity transform.
  VkAccelerationStructureInstanceKHR instance{};
  // VkTransformMatrixKHR is row-major 3x4 (mat3x4).
  instance.transform.matrix[0][0] = 1.0f;
  instance.transform.matrix[1][1] = 1.0f;
  instance.transform.matrix[2][2] = 1.0f;
  instance.instanceCustomIndex = 0;
  instance.mask = 0xFF;
  // hitGroupStride = 2 (primary + shadow per geometry); base offset = 0.
  instance.instanceShaderBindingTableRecordOffset = 0;
  instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
  instance.accelerationStructureReference = _blasAddress;

  // Upload via host-visible instance buffer (small, 64 bytes).
  allocateBuffer(sizeof(instance),
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 _instancesBuffer, _instancesBufferMemory, true);
  void *mapped = nullptr;
  vkMapMemory(dev, _instancesBufferMemory, 0, sizeof(instance), 0, &mapped);
  std::memcpy(mapped, &instance, sizeof(instance));
  vkUnmapMemory(dev, _instancesBufferMemory);

  VkAccelerationStructureGeometryKHR geo{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geo.flags = 0;
  geo.geometry.instances = {
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
  geo.geometry.instances.arrayOfPointers = VK_FALSE;
  geo.geometry.instances.data.deviceAddress = getBufferDeviceAddress(_instancesBuffer);

  VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  buildInfo.geometryCount = 1;
  buildInfo.pGeometries = &geo;

  uint32_t primCount = 1;
  VkAccelerationStructureBuildSizesInfoKHR sizesInfo{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  pfnGetAccelerationStructureBuildSizes(
      dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
      &primCount, &sizesInfo);

  allocateBuffer(sizesInfo.accelerationStructureSize,
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 _tlasBuffer, _tlasBufferMemory, true);

  VkAccelerationStructureCreateInfoKHR asCreate{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  asCreate.buffer = _tlasBuffer;
  asCreate.size = sizesInfo.accelerationStructureSize;
  asCreate.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  if (pfnCreateAccelerationStructure(dev, &asCreate, nullptr, &_tlas) != VK_SUCCESS) {
    throw std::runtime_error("RT: vkCreateAccelerationStructureKHR (TLAS) failed");
  }

  VkBuffer scratchBuf = VK_NULL_HANDLE;
  VkDeviceMemory scratchMem = VK_NULL_HANDLE;
  const VkDeviceSize scratchAlign = _device.getASProperties().minAccelerationStructureScratchOffsetAlignment;
  const VkDeviceSize scratchSize = alignUp(sizesInfo.buildScratchSize,
                                            scratchAlign ? scratchAlign : 256);
  allocateBuffer(scratchSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 scratchBuf, scratchMem, true);

  buildInfo.dstAccelerationStructure = _tlas;
  buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(scratchBuf);

  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = 1;
  range.primitiveOffset = 0;
  range.firstVertex = 0;
  range.transformOffset = 0;
  const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;

  VkCommandBuffer cmd = _device.beginSingleTimeCommands();
  pfnCmdBuildAccelerationStructures(cmd, 1, &buildInfo, &pRange);
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0, 1, &mb, 0, nullptr, 0, nullptr);
  _device.endSingleTimeCommands(cmd);

  vkDestroyBuffer(dev, scratchBuf, nullptr);
  vkFreeMemory(dev, scratchMem, nullptr);

  spdlog::info("RT: TLAS built (1 instance), AS size={} bytes",
               (uint64_t)sizesInfo.accelerationStructureSize);
}

// --- Stage 4 stubs ---
void RayTracing::CreatePipelineAndSBT() {
  VkDevice dev = _device.getLogicalDevice();

  // 1) Load SPIR-V library blob
  std::vector<char> spv = readFile("shaders/rt_lighting.lib.spv");
  VkShaderModuleCreateInfo smInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smInfo.codeSize = spv.size();
  smInfo.pCode = reinterpret_cast<const uint32_t *>(spv.data());
  if (vkCreateShaderModule(dev, &smInfo, nullptr, &_rtShaderModule) != VK_SUCCESS) {
    throw std::runtime_error("RT: failed to create shader module from rt_lighting.lib.spv");
  }

  // 2) Stage create infos. Indices used by groups below.
  enum StageIdx : uint32_t {
    STG_RAYGEN = 0,
    STG_MISS_PRIMARY,
    STG_MISS_SHADOW,
    STG_CHIT_PRIMARY,
    STG_AHIT_PRIMARY,
    STG_AHIT_SHADOW,
    STG_COUNT
  };
  std::array<VkPipelineShaderStageCreateInfo, STG_COUNT> stages{};
  auto fillStage = [&](VkShaderStageFlagBits stage, const char *entry,
                       VkPipelineShaderStageCreateInfo &out) {
    out = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    out.stage = stage;
    out.module = _rtShaderModule;
    out.pName = entry;
  };
  fillStage(VK_SHADER_STAGE_RAYGEN_BIT_KHR,      "RayGen",            stages[STG_RAYGEN]);
  fillStage(VK_SHADER_STAGE_MISS_BIT_KHR,        "MissPrimary",       stages[STG_MISS_PRIMARY]);
  fillStage(VK_SHADER_STAGE_MISS_BIT_KHR,        "MissShadow",        stages[STG_MISS_SHADOW]);
  fillStage(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, "ClosestHitPrimary", stages[STG_CHIT_PRIMARY]);
  fillStage(VK_SHADER_STAGE_ANY_HIT_BIT_KHR,     "AnyHitAlpha",       stages[STG_AHIT_PRIMARY]);
  fillStage(VK_SHADER_STAGE_ANY_HIT_BIT_KHR,     "AnyHitAlphaShadow", stages[STG_AHIT_SHADOW]);

  // 3) Shader groups. SBT order:
  //    [0] raygen
  //    [1] miss primary
  //    [2] miss shadow
  //    [3] HG_PRI_OPAQUE  (chit only)
  //    [4] HG_PRI_ALPHA   (chit + ahit)
  //    [5] HG_SHA_OPAQUE  (empty -- shadow rays use SKIP_CLOSEST_HIT)
  //    [6] HG_SHA_ALPHA   (ahit only)
  enum GroupIdx : uint32_t {
    GRP_RAYGEN = 0,
    GRP_MISS_PRIMARY,
    GRP_MISS_SHADOW,
    GRP_HG_PRI_OPAQUE,
    GRP_HG_PRI_ALPHA,
    GRP_HG_SHA_OPAQUE,
    GRP_HG_SHA_ALPHA,
    GRP_COUNT
  };
  std::array<VkRayTracingShaderGroupCreateInfoKHR, GRP_COUNT> groups{};
  for (auto &g : groups) {
    g = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
    g.generalShader = VK_SHADER_UNUSED_KHR;
    g.closestHitShader = VK_SHADER_UNUSED_KHR;
    g.anyHitShader = VK_SHADER_UNUSED_KHR;
    g.intersectionShader = VK_SHADER_UNUSED_KHR;
  }
  groups[GRP_RAYGEN].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  groups[GRP_RAYGEN].generalShader = STG_RAYGEN;

  groups[GRP_MISS_PRIMARY].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  groups[GRP_MISS_PRIMARY].generalShader = STG_MISS_PRIMARY;

  groups[GRP_MISS_SHADOW].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  groups[GRP_MISS_SHADOW].generalShader = STG_MISS_SHADOW;

  groups[GRP_HG_PRI_OPAQUE].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  groups[GRP_HG_PRI_OPAQUE].closestHitShader = STG_CHIT_PRIMARY;

  groups[GRP_HG_PRI_ALPHA].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  groups[GRP_HG_PRI_ALPHA].closestHitShader = STG_CHIT_PRIMARY;
  groups[GRP_HG_PRI_ALPHA].anyHitShader = STG_AHIT_PRIMARY;

  groups[GRP_HG_SHA_OPAQUE].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  // empty -- both shaders unused

  groups[GRP_HG_SHA_ALPHA].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  groups[GRP_HG_SHA_ALPHA].anyHitShader = STG_AHIT_SHADOW;

  // 4) Pipeline layout: set 0 = globalSetLayout (camera UBO from GpuScene),
  //                     set 1 = _rtSetLayout (RT-specific resources).
  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                       VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                       VK_SHADER_STAGE_MISS_BIT_KHR;
  pcRange.offset = 0;
  pcRange.size = 32;  // sizeof(RTPushConsts) — keep in sync with rt_lighting.hlsl

  VkDescriptorSetLayout setLayouts[] = { _scene.globalSetLayout, _rtSetLayout };
  VkPipelineLayoutCreateInfo plInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plInfo.setLayoutCount = 2;
  plInfo.pSetLayouts = setLayouts;
  plInfo.pushConstantRangeCount = 1;
  plInfo.pPushConstantRanges = &pcRange;
  if (vkCreatePipelineLayout(dev, &plInfo, nullptr, &_rtPipelineLayout) != VK_SUCCESS) {
    throw std::runtime_error("RT: failed to create pipeline layout");
  }

  // 5) Pipeline
  VkRayTracingPipelineCreateInfoKHR rtPipelineInfo{
      VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
  rtPipelineInfo.stageCount = STG_COUNT;
  rtPipelineInfo.pStages = stages.data();
  rtPipelineInfo.groupCount = GRP_COUNT;
  rtPipelineInfo.pGroups = groups.data();
  rtPipelineInfo.maxPipelineRayRecursionDepth = 2;  // primary + shadow
  rtPipelineInfo.layout = _rtPipelineLayout;
  if (pfnCreateRayTracingPipelines(dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1,
                                    &rtPipelineInfo, nullptr, &_rtPipeline) != VK_SUCCESS) {
    throw std::runtime_error("RT: failed to create ray tracing pipeline");
  }

  // 6) SBT — one buffer with raygen / miss / hit regions.
  const VkPhysicalDeviceRayTracingPipelinePropertiesKHR &rtProps =
      _device.getRTPipelineProperties();
  const uint32_t handleSize = rtProps.shaderGroupHandleSize;
  const uint32_t handleAlignment = rtProps.shaderGroupHandleAlignment;
  const uint32_t baseAlignment = rtProps.shaderGroupBaseAlignment;
  const uint32_t handleSizeAligned = (uint32_t)alignUp(handleSize, handleAlignment);

  // Region sizes
  const uint32_t opaqueCount = (uint32_t)_scene.applMesh->_opaqueChunkCount;
  const uint32_t alphaCount  = (uint32_t)_scene.applMesh->_alphaMaskedChunkCount;
  const uint32_t geomCount   = opaqueCount + alphaCount;

  _rgenRegion.stride = (uint32_t)alignUp(handleSizeAligned, baseAlignment);
  _rgenRegion.size   = _rgenRegion.stride;          // raygen size MUST equal stride

  _missRegion.stride = handleSizeAligned;
  _missRegion.size   = (uint32_t)alignUp(handleSizeAligned * 2, baseAlignment);

  _hitRegion.stride = handleSizeAligned;
  _hitRegion.size   = (uint32_t)alignUp(handleSizeAligned * geomCount * 2, baseAlignment);

  _callRegion = {};

  // Fetch all group handles
  const uint32_t totalGroupHandles = GRP_COUNT;
  std::vector<uint8_t> handles(totalGroupHandles * handleSize);
  if (pfnGetRayTracingShaderGroupHandles(dev, _rtPipeline, 0, totalGroupHandles,
                                          handles.size(), handles.data()) != VK_SUCCESS) {
    throw std::runtime_error("RT: vkGetRayTracingShaderGroupHandlesKHR failed");
  }
  auto handlePtr = [&](uint32_t idx) { return handles.data() + idx * handleSize; };

  const VkDeviceSize sbtSize = _rgenRegion.size + _missRegion.size + _hitRegion.size;
  allocateBuffer(sbtSize,
                 VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 _sbtBuffer, _sbtMemory, true);

  uint8_t *sbtMapped = nullptr;
  vkMapMemory(dev, _sbtMemory, 0, sbtSize, 0, (void**)&sbtMapped);
  std::memset(sbtMapped, 0, sbtSize);

  // Layout:
  //   [0 .. rgenRegion.size)                     raygen
  //   [rgenRegion.size .. +missRegion.size)      miss x 2
  //   [... .. +hitRegion.size)                   hit per geometry x 2
  uint8_t *p = sbtMapped;
  std::memcpy(p, handlePtr(GRP_RAYGEN), handleSize);
  p = sbtMapped + _rgenRegion.size;

  std::memcpy(p + 0 * handleSizeAligned, handlePtr(GRP_MISS_PRIMARY), handleSize);
  std::memcpy(p + 1 * handleSizeAligned, handlePtr(GRP_MISS_SHADOW),  handleSize);
  p = sbtMapped + _rgenRegion.size + _missRegion.size;

  for (uint32_t g = 0; g < geomCount; ++g) {
    const bool isAlpha = (g >= opaqueCount);
    uint32_t priGroup = isAlpha ? GRP_HG_PRI_ALPHA : GRP_HG_PRI_OPAQUE;
    uint32_t shaGroup = isAlpha ? GRP_HG_SHA_ALPHA : GRP_HG_SHA_OPAQUE;
    std::memcpy(p + (g * 2 + 0) * handleSizeAligned, handlePtr(priGroup), handleSize);
    std::memcpy(p + (g * 2 + 1) * handleSizeAligned, handlePtr(shaGroup), handleSize);
  }
  vkUnmapMemory(dev, _sbtMemory);

  // Fill region device addresses
  VkDeviceAddress sbtAddr = getBufferDeviceAddress(_sbtBuffer);
  _rgenRegion.deviceAddress = sbtAddr;
  _missRegion.deviceAddress = sbtAddr + _rgenRegion.size;
  _hitRegion.deviceAddress  = sbtAddr + _rgenRegion.size + _missRegion.size;

  spdlog::info(
      "RT: pipeline created. SBT size={} bytes, hit records={} (handleSize={}, aligned={})",
      (uint64_t)sbtSize, geomCount * 2, handleSize, handleSizeAligned);
}

void RayTracing::CreateOutputImagesAndDescriptorSet() {
  VkDevice dev = _device.getLogicalDevice();
  const uint32_t N = _device.getSwapChainImageCount();
  _rtExtent = _device.getSwapChainExtent();

  // 1) Output images (HDR R16G16B16A16, STORAGE+TRANSFER_SRC+SAMPLED).
  _rtLitImage.resize(N);
  _rtLitMemory.resize(N);
  _rtLitImageView.resize(N);
  for (uint32_t f = 0; f < N; ++f) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = {_rtExtent.width, _rtExtent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ii, nullptr, &_rtLitImage[f]) != VK_SUCCESS) {
      throw std::runtime_error("RT: vkCreateImage (output) failed");
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, _rtLitImage[f], &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = _device.findMemoryType(req.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &ai, nullptr, &_rtLitMemory[f]) != VK_SUCCESS) {
      throw std::runtime_error("RT: vkAllocateMemory (output) failed");
    }
    vkBindImageMemory(dev, _rtLitImage[f], _rtLitMemory[f], 0);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = _rtLitImage[f];
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ii.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &vci, nullptr, &_rtLitImageView[f]) != VK_SUCCESS) {
      throw std::runtime_error("RT: vkCreateImageView (output) failed");
    }
  }

  // 2) RT descriptor set layout (set 1).
  // Bindings match rt_lighting.hlsl set 1:
  //   0  TLAS                 (acceleration structure)
  //   1  outLitColor          (storage image)
  //   2  vbPositions          (storage buffer)
  //   3  vbNormals            (storage buffer)
  //   4  vbTangents           (storage buffer)
  //   5  vbUVs                (storage buffer)
  //   6  ibIndices            (storage buffer / byte-address)
  //   7  meshChunksRT         (storage buffer)
  //   8  materialsRT          (storage buffer)
  //   9  pointLightsRT        (storage buffer)
  //  10  _Textures[]          (sampled image array, bindless)
  //  11  _LinearRepeatSampler (sampler)
  const uint32_t bindingCount = 12;
  std::array<VkDescriptorSetLayoutBinding, bindingCount> b{};
  auto fill = [](VkDescriptorSetLayoutBinding &x, uint32_t binding,
                 VkDescriptorType type, uint32_t count, VkShaderStageFlags stages) {
    x.binding = binding;
    x.descriptorType = type;
    x.descriptorCount = count;
    x.stageFlags = stages;
    x.pImmutableSamplers = nullptr;
  };
  const VkShaderStageFlags rtAllStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                          VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                                          VK_SHADER_STAGE_MISS_BIT_KHR;
  fill(b[0],  0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, rtAllStages);
  fill(b[1],  1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,             1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  fill(b[2],  2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, rtAllStages);
  fill(b[3],  3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, rtAllStages);
  fill(b[4],  4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, rtAllStages);
  fill(b[5],  5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, rtAllStages);
  fill(b[6],  6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, rtAllStages);
  fill(b[7],  7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, rtAllStages);
  fill(b[8],  8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, rtAllStages);
  fill(b[9],  9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            1, rtAllStages);
  const uint32_t bindlessCount = (uint32_t)_scene.textures.size();
  fill(b[10], 10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,            bindlessCount, rtAllStages);
  fill(b[11], 11, VK_DESCRIPTOR_TYPE_SAMPLER,                  1, rtAllStages);

  std::array<VkDescriptorBindingFlags, bindingCount> bf{};
  bf[10] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT;
  VkDescriptorSetLayoutBindingFlagsCreateInfo bfInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  bfInfo.bindingCount = bindingCount;
  bfInfo.pBindingFlags = bf.data();

  VkDescriptorSetLayoutCreateInfo slInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  slInfo.pNext = &bfInfo;
  slInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  slInfo.bindingCount = bindingCount;
  slInfo.pBindings = b.data();
  if (vkCreateDescriptorSetLayout(dev, &slInfo, nullptr, &_rtSetLayout) != VK_SUCCESS) {
    throw std::runtime_error("RT: failed to create descriptor set layout");
  }

  // 3) Descriptor pool sized for N per-frame sets.
  std::vector<VkDescriptorPoolSize> poolSizes = {
      {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, N},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              N},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             8 * N},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,              bindlessCount * N + 1},
      {VK_DESCRIPTOR_TYPE_SAMPLER,                    N}};
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT;
  pi.maxSets = N;
  pi.poolSizeCount = (uint32_t)poolSizes.size();
  pi.pPoolSizes = poolSizes.data();
  if (vkCreateDescriptorPool(dev, &pi, nullptr, &_rtDescriptorPool) != VK_SUCCESS) {
    throw std::runtime_error("RT: failed to create descriptor pool");
  }

  _rtDescriptorSets.resize(N);
  std::vector<VkDescriptorSetLayout> layouts(N, _rtSetLayout);
  VkDescriptorSetAllocateInfo aInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  aInfo.descriptorPool = _rtDescriptorPool;
  aInfo.descriptorSetCount = N;
  aInfo.pSetLayouts = layouts.data();
  if (vkAllocateDescriptorSets(dev, &aInfo, _rtDescriptorSets.data()) != VK_SUCCESS) {
    throw std::runtime_error("RT: vkAllocateDescriptorSets failed");
  }

  // 4) Write descriptors per-frame
  for (uint32_t f = 0; f < N; ++f) {
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &_tlas;

    VkDescriptorImageInfo outImg{};
    outImg.imageView = _rtLitImageView[f];
    outImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorBufferInfo bvb {_scene.applVertexBuffer,  0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bvn {_scene.applNormalBuffer,  0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bvt {_scene.applTangentBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bvu {_scene.applUVBuffer,      0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bib {_scene.applIndexBuffer,   0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bch {_scene.meshChunksBuffer,  0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bma {_scene.applMaterialBuffer,0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bpl {
        _scene._lightCuller ? _scene._lightCuller->GetPointLightCullingDataBuffer() : VK_NULL_HANDLE,
        0, VK_WHOLE_SIZE};

    std::vector<VkDescriptorImageInfo> texImgs(bindlessCount);
    for (uint32_t i = 0; i < bindlessCount; ++i) {
      texImgs[i].imageView = _scene.textures[i].second;
      texImgs[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler = _scene.textureSampler;

    std::array<VkWriteDescriptorSet, 12> w{};
    auto bufW = [&](uint32_t i, uint32_t binding, VkDescriptorBufferInfo *bi,
                    VkDescriptorType type) {
      w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w[i].dstSet = _rtDescriptorSets[f];
      w[i].dstBinding = binding;
      w[i].descriptorCount = 1;
      w[i].descriptorType = type;
      w[i].pBufferInfo = bi;
    };
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asWrite};
    w[0].dstSet = _rtDescriptorSets[f];
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[1].dstSet = _rtDescriptorSets[f];
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[1].pImageInfo = &outImg;

    bufW(2, 2, &bvb, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bufW(3, 3, &bvn, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bufW(4, 4, &bvt, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bufW(5, 5, &bvu, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bufW(6, 6, &bib, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bufW(7, 7, &bch, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bufW(8, 8, &bma, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    bufW(9, 9, &bpl, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    w[10] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[10].dstSet = _rtDescriptorSets[f];
    w[10].dstBinding = 10;
    w[10].descriptorCount = bindlessCount;
    w[10].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[10].pImageInfo = texImgs.data();

    w[11] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[11].dstSet = _rtDescriptorSets[f];
    w[11].dstBinding = 11;
    w[11].descriptorCount = 1;
    w[11].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[11].pImageInfo = &samplerInfo;

    vkUpdateDescriptorSets(dev, (uint32_t)w.size(), w.data(), 0, nullptr);
  }

  spdlog::info("RT: descriptor set + {} output images created (extent {}x{})",
               N, _rtExtent.width, _rtExtent.height);

  // 5) ImGui composite render pass: load swapchain (preserve blit), draw ImGui,
  //    transition to PRESENT_SRC_KHR.
  VkAttachmentDescription colorAtt{};
  colorAtt.format = _device.getSwapChainImageFormat();
  colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAtt.loadOp  = VK_ATTACHMENT_LOAD_OP_LOAD;
  colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorAtt.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colorAtt.finalLayout   = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &colorRef;

  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.srcAccessMask = 0;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = 1;
  rpci.pAttachments = &colorAtt;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  rpci.dependencyCount = 1;
  rpci.pDependencies = &dep;
  if (vkCreateRenderPass(dev, &rpci, nullptr, &_rtImguiPass) != VK_SUCCESS) {
    throw std::runtime_error("RT: failed to create imgui composite render pass");
  }

  _rtImguiFrameBuffer.resize(N);
  for (uint32_t f = 0; f < N; ++f) {
    VkImageView attView = _device.getSwapChainImageView((int)f);
    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbci.renderPass = _rtImguiPass;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &attView;
    fbci.width = _rtExtent.width;
    fbci.height = _rtExtent.height;
    fbci.layers = 1;
    if (vkCreateFramebuffer(dev, &fbci, nullptr, &_rtImguiFrameBuffer[f]) != VK_SUCCESS) {
      throw std::runtime_error("RT: failed to create imgui composite framebuffer");
    }
  }
}

void RayTracing::BeginImGuiCompositePass(VkCommandBuffer cb, uint32_t imageIndex,
                                          VkExtent2D extent) {
  VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  bi.renderPass = _rtImguiPass;
  bi.framebuffer = _rtImguiFrameBuffer[imageIndex];
  bi.renderArea.offset = {0, 0};
  bi.renderArea.extent = extent;
  bi.clearValueCount = 0;
  vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
}

void RayTracing::EndImGuiCompositePass(VkCommandBuffer cb) {
  vkCmdEndRenderPass(cb);
}

void RayTracing::RecordTraceRays(VkCommandBuffer cb, uint32_t imageIndex,
                                  VkExtent2D extent) {
  // Transition output image UNDEFINED/SHADER_READ → GENERAL for write
  VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  toGeneral.srcAccessMask = 0;
  toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toGeneral.image = _rtLitImage[imageIndex];
  toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cb,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                       0, 0, nullptr, 0, nullptr, 1, &toGeneral);

  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _rtPipeline);

  VkDescriptorSet sets[] = {
      _scene.globalDescriptorSets[_scene.currentFrame],
      _rtDescriptorSets[imageIndex]
  };
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                          _rtPipelineLayout, 0, 2, sets, 0, nullptr);

  // Push constants (RTPushConsts in rt_lighting.hlsl)
  struct RTPC {
    uint32_t pointLightCount;
    uint32_t shadowTaps;
    float    sunConeRadius;
    float    pixelSpreadAngle;
    uint32_t frameSeed;
    uint32_t pad0, pad1, pad2;
  } pc{};
  pc.pointLightCount = (uint32_t)_scene._pointLights.size();
  pc.shadowTaps = 4;
  // sun half angle ≈ 0.5 deg ≈ 8.7e-3 rad; tan small angle ≈ angle
  pc.sunConeRadius = 0.0087f;
  pc.pixelSpreadAngle = 2.0f * std::tan(0.5f * 1.0472f /* fovY≈60deg */) / float(extent.height);
  pc.frameSeed = _scene.frameConstants.frameCounter;
  vkCmdPushConstants(cb, _rtPipelineLayout,
                     VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                         VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                         VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                         VK_SHADER_STAGE_MISS_BIT_KHR,
                     0, sizeof(pc), &pc);

  pfnCmdTraceRays(cb, &_rgenRegion, &_missRegion, &_hitRegion, &_callRegion,
                  extent.width, extent.height, 1);
}

void RayTracing::RecordBlitToSwapchain(VkCommandBuffer cb, uint32_t imageIndex) {
  VkImage rtImg = _rtLitImage[imageIndex];
  VkImage swap = _device.getSwapChainImage((int)imageIndex);

  // RT image GENERAL → TRANSFER_SRC; swap UNDEFINED → TRANSFER_DST
  std::array<VkImageMemoryBarrier, 2> barriers{};
  barriers[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barriers[0].image = rtImg;
  barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  barriers[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[1].srcAccessMask = 0;
  barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barriers[1].image = swap;
  barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  vkCmdPipelineBarrier(cb,
                       VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, (uint32_t)barriers.size(), barriers.data());

  VkImageBlit region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.srcOffsets[0] = {0, 0, 0};
  region.srcOffsets[1] = {(int32_t)_rtExtent.width, (int32_t)_rtExtent.height, 1};
  region.dstSubresource = region.srcSubresource;
  region.dstOffsets[0] = {0, 0, 0};
  region.dstOffsets[1] = {(int32_t)_rtExtent.width, (int32_t)_rtExtent.height, 1};
  vkCmdBlitImage(cb, rtImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 1, &region, VK_FILTER_NEAREST);

  // Transition swapchain → COLOR_ATTACHMENT (for ImGui)
  VkImageMemoryBarrier toColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  toColor.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toColor.image = swap;
  toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cb,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &toColor);
}
