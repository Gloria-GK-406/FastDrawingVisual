#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>

#include "../FastDrawingVisual.NativeProxy.Shared/FramePacket.h"
#include "../FastDrawingVisual.NativeProxy.Shared/RendererLockGuard.h"

namespace fdv::d3d11 {

using LayerPacket = fdv::nativeproxy::shared::LayerPacket;
using LayeredFramePacket = fdv::nativeproxy::shared::LayeredFramePacket;
using RendererLockGuard = fdv::nativeproxy::shared::RendererLockGuard;

struct D3D11SwapChainRendererState;
struct SubmitFrameDiagnostics;

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
  HRESULT SubmitCompiledBatches(const LayerPacket& layer, int layerIndex,
                                void* currentRtv,
                                SubmitFrameDiagnostics& diagnostics);
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
  int compileDurationMetricId_ = 0;
  int commandReadDurationMetricId_ = 0;
  int commandBuildDurationMetricId_ = 0;
  int triangleCpuDurationMetricId_ = 0;
  int triangleUploadDurationMetricId_ = 0;
  int triangleDrawCallDurationMetricId_ = 0;
  int textDurationMetricId_ = 0;
  int presentDurationMetricId_ = 0;
  std::uint64_t lastPresentQpc_ = 0;
  std::uint64_t submittedFrameCount_ = 0;

  bool csInitialized_ = false;
  CRITICAL_SECTION cs_{};
};

} // namespace fdv::d3d11
