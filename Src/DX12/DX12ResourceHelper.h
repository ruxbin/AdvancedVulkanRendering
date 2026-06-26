#pragma once
#ifdef ENABLE_DX12

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <stdexcept>

#include "spdlog/spdlog.h"

using Microsoft::WRL::ComPtr;

namespace DX12Util {

inline void ThrowIfFailed(HRESULT hr, const char* msg) {
  if (FAILED(hr)) {
    spdlog::error("DX12 Error: {} (HRESULT: 0x{:08X})", msg, (unsigned)hr);
    throw std::runtime_error(msg);
  }
}

// Create a GPU-local buffer and upload initial data via an upload heap.
// Returns the default buffer. uploadBuffer must be kept alive until the command list executes.
inline ComPtr<ID3D12Resource> CreateDefaultBuffer(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const void* initData,
    UINT64 byteSize,
    ComPtr<ID3D12Resource>& uploadBuffer) {

  ComPtr<ID3D12Resource> defaultBuffer;

  D3D12_HEAP_PROPERTIES defaultHeap = {};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC bufDesc = {};
  bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufDesc.Width = byteSize;
  bufDesc.Height = 1;
  bufDesc.DepthOrArraySize = 1;
  bufDesc.MipLevels = 1;
  bufDesc.SampleDesc.Count = 1;
  bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ThrowIfFailed(device->CreateCommittedResource(
      &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
      D3D12_RESOURCE_STATE_COMMON, nullptr,
      IID_PPV_ARGS(&defaultBuffer)),
      "CreateDefaultBuffer: default heap");

  D3D12_HEAP_PROPERTIES uploadHeapProps = {};
  uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
  ThrowIfFailed(device->CreateCommittedResource(
      &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
      IID_PPV_ARGS(&uploadBuffer)),
      "CreateDefaultBuffer: upload heap");

  // Copy data to upload buffer
  void* mapped = nullptr;
  uploadBuffer->Map(0, nullptr, &mapped);
  memcpy(mapped, initData, (size_t)byteSize);
  uploadBuffer->Unmap(0, nullptr);

  // Transition default buffer to COPY_DEST
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = defaultBuffer.Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cmdList->ResourceBarrier(1, &barrier);

  cmdList->CopyBufferRegion(defaultBuffer.Get(), 0, uploadBuffer.Get(), 0, byteSize);

  // Transition to generic read
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
  cmdList->ResourceBarrier(1, &barrier);

  return defaultBuffer;
}

// Create a persistent upload buffer (HOST_VISIBLE, for uniform/constant buffers).
// Returns mapped pointer via `mappedData`.
inline ComPtr<ID3D12Resource> CreateUploadBuffer(
    ID3D12Device* device,
    UINT64 byteSize,
    void** mappedData = nullptr) {

  ComPtr<ID3D12Resource> buffer;

  D3D12_HEAP_PROPERTIES uploadHeap = {};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC bufDesc = {};
  bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufDesc.Width = byteSize;
  bufDesc.Height = 1;
  bufDesc.DepthOrArraySize = 1;
  bufDesc.MipLevels = 1;
  bufDesc.SampleDesc.Count = 1;
  bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ThrowIfFailed(device->CreateCommittedResource(
      &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
      IID_PPV_ARGS(&buffer)),
      "CreateUploadBuffer");

  if (mappedData) {
    buffer->Map(0, nullptr, mappedData);
  }

  return buffer;
}

// Create a GPU-local texture2D.
inline ComPtr<ID3D12Resource> CreateTexture2D(
    ID3D12Device* device,
    uint32_t width, uint32_t height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
    uint32_t mipLevels = 1,
    uint32_t arraySize = 1,
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON,
    const D3D12_CLEAR_VALUE* clearValue = nullptr) {

  ComPtr<ID3D12Resource> texture;

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = (UINT16)arraySize;
  desc.MipLevels = (UINT16)mipLevels;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;

  ThrowIfFailed(device->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
      initialState, clearValue,
      IID_PPV_ARGS(&texture)),
      "CreateTexture2D");

  return texture;
}

// Create a GPU-local buffer (no initial upload).
inline ComPtr<ID3D12Resource> CreateGPUBuffer(
    ID3D12Device* device,
    UINT64 byteSize,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON) {

  ComPtr<ID3D12Resource> buffer;

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = byteSize;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;

  ThrowIfFailed(device->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
      initialState, nullptr,
      IID_PPV_ARGS(&buffer)),
      "CreateGPUBuffer");

  return buffer;
}

// Emit a single transition barrier.
inline void TransitionBarrier(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after,
    UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  barrier.Transition.Subresource = subresource;
  cmdList->ResourceBarrier(1, &barrier);
}

// Emit a UAV barrier.
inline void UAVBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource) {
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = resource;
  cmdList->ResourceBarrier(1, &barrier);
}

// Read a compiled shader file (.cso) into a byte vector.
inline std::vector<char> ReadShaderFile(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    spdlog::error("Failed to open shader file: {}", path);
    return {};
  }
  fseek(f, 0, SEEK_END);
  size_t size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<char> data(size);
  fread(data.data(), 1, size, f);
  fclose(f);
  return data;
}

} // namespace DX12Util

#endif // ENABLE_DX12
