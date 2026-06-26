#pragma once
#ifdef ENABLE_DX12

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "spdlog/spdlog.h"

using Microsoft::WRL::ComPtr;

struct SDL_Window;

class DX12Device {
public:
  static constexpr uint32_t FRAME_COUNT = 3;

  DX12Device(void* hwnd, uint32_t width, uint32_t height);
  DX12Device() = delete;
  DX12Device(const DX12Device&) = delete;
  ~DX12Device();

  ID3D12Device* GetDevice() const { return _device.Get(); }
  ID3D12CommandQueue* GetCommandQueue() const { return _directQueue.Get(); }
  IDXGISwapChain4* GetSwapChain() const { return _swapChain.Get(); }
  ID3D12CommandAllocator* GetCommandAllocator(uint32_t frame) const { return _cmdAllocators[frame].Get(); }
  ID3D12GraphicsCommandList* GetCommandList() const { return _cmdList.Get(); }

  ID3D12Resource* GetSwapChainBuffer(uint32_t i) const { return _swapChainBuffers[i].Get(); }
  ID3D12Resource* GetDepthStencilBuffer() const { return _depthStencilBuffer.Get(); }

  D3D12_CPU_DESCRIPTOR_HANDLE GetRTV(uint32_t i) const;
  D3D12_CPU_DESCRIPTOR_HANDLE GetDSV() const;

  DXGI_FORMAT GetSwapChainFormat() const { return _swapChainFormat; }
  DXGI_FORMAT GetDepthFormat() const { return _depthFormat; }
  uint32_t GetWidth() const { return _width; }
  uint32_t GetHeight() const { return _height; }
  uint32_t GetFrameIndex() const { return _frameIndex; }
  uint32_t GetFrameCount() const { return FRAME_COUNT; }

  // Frame synchronization
  void WaitForGpu();
  void MoveToNextFrame();
  void BeginFrame();
  void EndFrameAndPresent();

private:
  void EnableDebugLayer();
  void CreateDevice();
  void CreateCommandQueue();
  void CreateSwapChain(void* hwnd);
  void CreateRTVHeap();
  void CreateDSVHeap();
  void CreateDepthStencilBuffer();
  void CreateCommandAllocatorsAndList();
  void CreateFence();

  ComPtr<IDXGIFactory6> _factory;
  ComPtr<IDXGIAdapter4> _adapter;
  ComPtr<ID3D12Device> _device;
  ComPtr<ID3D12CommandQueue> _directQueue;
  ComPtr<IDXGISwapChain4> _swapChain;

  // RTV
  ComPtr<ID3D12DescriptorHeap> _rtvHeap;
  uint32_t _rtvDescriptorSize = 0;
  ComPtr<ID3D12Resource> _swapChainBuffers[FRAME_COUNT];

  // DSV
  ComPtr<ID3D12DescriptorHeap> _dsvHeap;
  ComPtr<ID3D12Resource> _depthStencilBuffer;

  // Command
  ComPtr<ID3D12CommandAllocator> _cmdAllocators[FRAME_COUNT];
  ComPtr<ID3D12GraphicsCommandList> _cmdList;

  // Sync
  ComPtr<ID3D12Fence> _fence;
  HANDLE _fenceEvent = nullptr;
  uint64_t _fenceValues[FRAME_COUNT] = {};

  uint32_t _frameIndex = 0;
  DXGI_FORMAT _swapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  DXGI_FORMAT _depthFormat = DXGI_FORMAT_D32_FLOAT;
  uint32_t _width, _height;
};

#endif // ENABLE_DX12
