#ifdef ENABLE_DX12

#include "DX12Setup.h"
#include <d3d12sdklayers.h>

// ---- Helpers ----
static void ThrowIfFailed(HRESULT hr, const char* msg) {
  if (FAILED(hr)) {
    spdlog::error("DX12 Error: {} (HRESULT: 0x{:08X})", msg, (unsigned)hr);
    throw std::runtime_error(msg);
  }
}

// ---- Constructor ----
DX12Device::DX12Device(void* hwnd, uint32_t width, uint32_t height)
    : _width(width), _height(height) {
  EnableDebugLayer();
  CreateDevice();
  CreateCommandQueue();
  CreateSwapChain(hwnd);
  CreateRTVHeap();
  CreateDSVHeap();
  CreateDepthStencilBuffer();
  CreateCommandAllocatorsAndList();
  CreateFence();

  spdlog::info("DX12Device initialized: {}x{}, {} frames in flight", _width, _height, FRAME_COUNT);
}

DX12Device::~DX12Device() {
  WaitForGpu();
  if (_fenceEvent) CloseHandle(_fenceEvent);
}

// ---- Debug Layer ----
void DX12Device::EnableDebugLayer() {
#ifdef _DEBUG
  ComPtr<ID3D12Debug> debugController;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
    debugController->EnableDebugLayer();
    spdlog::info("DX12 debug layer enabled");
  }
#endif
}

// ---- Device Creation ----
void DX12Device::CreateDevice() {
  ThrowIfFailed(
      CreateDXGIFactory2(0, IID_PPV_ARGS(&_factory)),
      "Failed to create DXGI factory");

  // Pick first hardware adapter that supports D3D12
  ComPtr<IDXGIAdapter1> adapter1;
  for (UINT i = 0; _factory->EnumAdapters1(i, &adapter1) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc;
    adapter1->GetDesc1(&desc);
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

    if (SUCCEEDED(D3D12CreateDevice(adapter1.Get(), D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr))) {
      adapter1.As(&_adapter);
      char adapterName[256];
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapterName, 256, nullptr, nullptr);
      spdlog::info("DX12 adapter: {}", adapterName);
      break;
    }
  }

  ThrowIfFailed(
      D3D12CreateDevice(_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&_device)),
      "Failed to create D3D12 device");
}

// ---- Command Queue ----
void DX12Device::CreateCommandQueue() {
  D3D12_COMMAND_QUEUE_DESC queueDesc = {};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

  ThrowIfFailed(
      _device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_directQueue)),
      "Failed to create command queue");
}

// ---- Swap Chain ----
void DX12Device::CreateSwapChain(void* hwnd) {
  DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
  swapChainDesc.Width = _width;
  swapChainDesc.Height = _height;
  swapChainDesc.Format = _swapChainFormat;
  swapChainDesc.SampleDesc.Count = 1;
  swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swapChainDesc.BufferCount = FRAME_COUNT;
  swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  ComPtr<IDXGISwapChain1> swapChain1;
  ThrowIfFailed(
      _factory->CreateSwapChainForHwnd(_directQueue.Get(), (HWND)hwnd,
                                       &swapChainDesc, nullptr, nullptr, &swapChain1),
      "Failed to create swap chain");

  // Disable Alt+Enter fullscreen toggle
  _factory->MakeWindowAssociation((HWND)hwnd, DXGI_MWA_NO_ALT_ENTER);

  swapChain1.As(&_swapChain);
  _frameIndex = _swapChain->GetCurrentBackBufferIndex();
}

// ---- RTV Heap ----
void DX12Device::CreateRTVHeap() {
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
  heapDesc.NumDescriptors = FRAME_COUNT;
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

  ThrowIfFailed(
      _device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_rtvHeap)),
      "Failed to create RTV heap");

  _rtvDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  // Create RTVs for each swap chain buffer
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = _rtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
    ThrowIfFailed(
        _swapChain->GetBuffer(i, IID_PPV_ARGS(&_swapChainBuffers[i])),
        "Failed to get swap chain buffer");
    _device->CreateRenderTargetView(_swapChainBuffers[i].Get(), nullptr, rtvHandle);
    rtvHandle.ptr += _rtvDescriptorSize;
  }
}

// ---- DSV Heap ----
void DX12Device::CreateDSVHeap() {
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
  heapDesc.NumDescriptors = 1;
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

  ThrowIfFailed(
      _device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_dsvHeap)),
      "Failed to create DSV heap");
}

// ---- Depth Buffer ----
void DX12Device::CreateDepthStencilBuffer() {
  D3D12_RESOURCE_DESC depthDesc = {};
  depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  depthDesc.Width = _width;
  depthDesc.Height = _height;
  depthDesc.DepthOrArraySize = 1;
  depthDesc.MipLevels = 1;
  depthDesc.Format = _depthFormat;
  depthDesc.SampleDesc.Count = 1;
  depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

  D3D12_CLEAR_VALUE clearValue = {};
  clearValue.Format = _depthFormat;
  clearValue.DepthStencil.Depth = 0.0f; // reverse-Z: near=1, far=0
  clearValue.DepthStencil.Stencil = 0;

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

  ThrowIfFailed(
      _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
                                        &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                        &clearValue, IID_PPV_ARGS(&_depthStencilBuffer)),
      "Failed to create depth stencil buffer");

  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
  dsvDesc.Format = _depthFormat;
  dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  _device->CreateDepthStencilView(_depthStencilBuffer.Get(), &dsvDesc,
                                   _dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

// ---- Command Allocators + List ----
void DX12Device::CreateCommandAllocatorsAndList() {
  for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
    ThrowIfFailed(
        _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         IID_PPV_ARGS(&_cmdAllocators[i])),
        "Failed to create command allocator");
  }

  ThrowIfFailed(
      _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                  _cmdAllocators[0].Get(), nullptr,
                                  IID_PPV_ARGS(&_cmdList)),
      "Failed to create command list");

  // Command list starts open, close it for the first frame
  _cmdList->Close();
}

// ---- Fence ----
void DX12Device::CreateFence() {
  ThrowIfFailed(
      _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence)),
      "Failed to create fence");

  _fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!_fenceEvent)
    throw std::runtime_error("Failed to create fence event");

  for (uint32_t i = 0; i < FRAME_COUNT; ++i)
    _fenceValues[i] = 0;
}

// ---- Descriptor Handle Helpers ----
D3D12_CPU_DESCRIPTOR_HANDLE DX12Device::GetRTV(uint32_t i) const {
  D3D12_CPU_DESCRIPTOR_HANDLE handle = _rtvHeap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += (SIZE_T)i * _rtvDescriptorSize;
  return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Device::GetDSV() const {
  return _dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

// ---- Frame Synchronization ----
void DX12Device::WaitForGpu() {
  // Signal and wait for the fence
  uint64_t fenceVal = ++_fenceValues[_frameIndex];
  _directQueue->Signal(_fence.Get(), fenceVal);

  if (_fence->GetCompletedValue() < fenceVal) {
    _fence->SetEventOnCompletion(fenceVal, _fenceEvent);
    WaitForSingleObject(_fenceEvent, INFINITE);
  }
}

void DX12Device::MoveToNextFrame() {
  // Schedule a signal for current frame
  uint64_t currentFenceValue = _fenceValues[_frameIndex];
  _directQueue->Signal(_fence.Get(), currentFenceValue);

  // Advance frame index
  _frameIndex = _swapChain->GetCurrentBackBufferIndex();

  // Wait if the next frame's fence hasn't been reached yet
  if (_fence->GetCompletedValue() < _fenceValues[_frameIndex]) {
    _fence->SetEventOnCompletion(_fenceValues[_frameIndex], _fenceEvent);
    WaitForSingleObject(_fenceEvent, INFINITE);
  }

  _fenceValues[_frameIndex] = currentFenceValue + 1;
}

void DX12Device::BeginFrame() {
  // Reset command allocator and command list for this frame
  _cmdAllocators[_frameIndex]->Reset();
  _cmdList->Reset(_cmdAllocators[_frameIndex].Get(), nullptr);

  // Transition swap chain buffer to RENDER_TARGET
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = _swapChainBuffers[_frameIndex].Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  _cmdList->ResourceBarrier(1, &barrier);
}

void DX12Device::EndFrameAndPresent() {
  // Transition swap chain buffer to PRESENT
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = _swapChainBuffers[_frameIndex].Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  _cmdList->ResourceBarrier(1, &barrier);

  // Close and execute
  _cmdList->Close();
  ID3D12CommandList* ppCmdLists[] = { _cmdList.Get() };
  _directQueue->ExecuteCommandLists(1, ppCmdLists);

  // Present
  _swapChain->Present(1, 0); // VSync on

  MoveToNextFrame();
}

#endif // ENABLE_DX12
