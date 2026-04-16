#pragma once
#ifdef ENABLE_DX12

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cassert>

using Microsoft::WRL::ComPtr;

// Simple descriptor heap wrapper with static region + per-frame ring buffer.
// CBV/SRV/UAV heap: shader-visible, used for all resource bindings.
// Sampler heap: shader-visible, small fixed set.
class DX12DescriptorHeap {
public:
  // staticCount: number of descriptors reserved at the start for persistent resources
  // dynamicCount: number of descriptors for per-frame ring buffer allocation
  void Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
            uint32_t staticCount, uint32_t dynamicCount, bool shaderVisible);

  ID3D12DescriptorHeap* GetHeap() const { return _heap.Get(); }

  // --- Static region [0, staticCount) ---
  D3D12_CPU_DESCRIPTOR_HANDLE GetStaticCPU(uint32_t index) const;
  D3D12_GPU_DESCRIPTOR_HANDLE GetStaticGPU(uint32_t index) const;

  // --- Dynamic ring buffer [staticCount, staticCount + dynamicCount) ---
  // Call ResetFrame() at the start of each frame to reset the allocator.
  void ResetFrame();

  // Allocate `count` contiguous descriptors from the ring buffer.
  // Returns the CPU handle for writing + GPU handle for binding.
  struct Allocation {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    uint32_t index; // absolute index in the heap
  };
  Allocation AllocateDynamic(uint32_t count = 1);

  uint32_t GetDescriptorSize() const { return _descriptorSize; }

private:
  ComPtr<ID3D12DescriptorHeap> _heap;
  uint32_t _descriptorSize = 0;
  uint32_t _staticCount = 0;
  uint32_t _dynamicStart = 0;
  uint32_t _dynamicCount = 0;
  uint32_t _dynamicOffset = 0; // current ring position within dynamic region
  D3D12_CPU_DESCRIPTOR_HANDLE _cpuStart = {};
  D3D12_GPU_DESCRIPTOR_HANDLE _gpuStart = {};
};

#endif // ENABLE_DX12
