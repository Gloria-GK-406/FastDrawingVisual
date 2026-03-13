#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>

namespace fdv::d3d11 {

struct LayerPacket {
  const void* commandData = nullptr;
  int32_t commandBytes = 0;
  const void* blobData = nullptr;
  int32_t blobBytes = 0;
  int32_t commandCount = 0;
};

struct LayeredFramePacket {
  static constexpr int kMaxLayerCount = 8;
  LayerPacket layers[kMaxLayerCount];
};

class RendererLockGuard final {
 public:
  explicit RendererLockGuard(CRITICAL_SECTION* cs) : cs_(cs) {
    if (cs_ != nullptr) {
      EnterCriticalSection(cs_);
    }
  }

  ~RendererLockGuard() {
    if (cs_ != nullptr) {
      LeaveCriticalSection(cs_);
    }
  }

  RendererLockGuard(const RendererLockGuard&) = delete;
  RendererLockGuard& operator=(const RendererLockGuard&) = delete;

 private:
  CRITICAL_SECTION* cs_ = nullptr;
};

struct D3D11SwapChainRendererState;

class D3D11SwapChainRenderer final {
 public:
  D3D11SwapChainRenderer(int width, int height);
  ~D3D11SwapChainRenderer();

  D3D11SwapChainRenderer(const D3D11SwapChainRenderer&) = delete;
  D3D11SwapChainRenderer& operator=(const D3D11SwapChainRenderer&) = delete;

  HRESULT Initialize();
  HRESULT Resize(int width, int height);
  HRESULT SubmitLayeredCommands(const void* framePacket, int framePacketBytes);
  HRESULT TryGetSwapChain(void** outSwapChain);

  void RegisterMetrics();
  void UnregisterMetrics();

  int width() const { return width_; }
  int height() const { return height_; }
  std::uint64_t submittedFrameCount() const { return submittedFrameCount_; }

 private:
  bool ValidateFramePacket(const void* framePacket, int framePacketBytes) const;
  HRESULT SubmitLayeredCommandsAndPresent(
      const LayeredFramePacket* framePacket);
  HRESULT BeginSubmitFrame(void*& currentRtv);
  HRESULT SubmitCompiledBatches(const LayerPacket& layer, void* currentRtv);
  void RecordFramePerformance(double drawDurationMs);

  HRESULT CreateDeviceAndSwapChain();
  void ReleaseRendererResources();
  void ReleaseRenderTargetResources();
  HRESULT ResizeSwapChain(int width, int height);
  HRESULT EnsureFactory();
  HRESULT CreateSwapChain();
  HRESULT EnsureD2DAndDWrite();
  HRESULT CreateRenderTarget();
  HRESULT CreateDrawPipeline();

 private:
  D3D11SwapChainRendererState* state_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int parseSubmitDurationMetricId_ = 0;
  int drawDurationMetricId_ = 0;
  int fpsMetricId_ = 0;
  std::uint64_t lastPresentQpc_ = 0;
  std::uint64_t submittedFrameCount_ = 0;

  bool csInitialized_ = false;
  CRITICAL_SECTION cs_{};
};

} // namespace fdv::d3d11
