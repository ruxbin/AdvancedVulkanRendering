#ifdef ENABLE_DX12

#include "DX12DescriptorHeap.h"
#include "spdlog/spdlog.h"

void DX12DescriptorHeap::Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                               uint32_t staticCount, uint32_t dynamicCount, bool shaderVisible) {
  _staticCount = staticCount;
  _dynamicStart = staticCount;
  _dynamicCount = dynamicCount;
  _dynamicOffset = 0;

  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  desc.NumDescriptors = staticCount + dynamicCount;
  desc.Type = type;
  desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

  HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_heap));
  if (FAILED(hr)) {
    spdlog::error("Failed to create descriptor heap (type={}, count={})", (int)type, desc.NumDescriptors);
    return;
  }

  _descriptorSize = device->GetDescriptorHandleIncrementSize(type);
  _cpuStart = _heap->GetCPUDescriptorHandleForHeapStart();
  if (shaderVisible)
    _gpuStart = _heap->GetGPUDescriptorHandleForHeapStart();
  else
    _gpuStart = {};
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12DescriptorHeap::GetStaticCPU(uint32_t index) const {
  assert(index < _staticCount);
  D3D12_CPU_DESCRIPTOR_HANDLE handle = _cpuStart;
  handle.ptr += (SIZE_T)index * _descriptorSize;
  return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DX12DescriptorHeap::GetStaticGPU(uint32_t index) const {
  assert(index < _staticCount);
  D3D12_GPU_DESCRIPTOR_HANDLE handle = _gpuStart;
  handle.ptr += (UINT64)index * _descriptorSize;
  return handle;
}

void DX12DescriptorHeap::ResetFrame() {
  _dynamicOffset = 0;
}

DX12DescriptorHeap::Allocation DX12DescriptorHeap::AllocateDynamic(uint32_t count) {
  assert(_dynamicOffset + count <= _dynamicCount && "Dynamic descriptor heap overflow");

  uint32_t absIndex = _dynamicStart + _dynamicOffset;
  Allocation alloc;
  alloc.cpu = _cpuStart;
  alloc.cpu.ptr += (SIZE_T)absIndex * _descriptorSize;
  alloc.gpu = _gpuStart;
  alloc.gpu.ptr += (UINT64)absIndex * _descriptorSize;
  alloc.index = absIndex;

  _dynamicOffset += count;
  return alloc;
}

#endif // ENABLE_DX12
