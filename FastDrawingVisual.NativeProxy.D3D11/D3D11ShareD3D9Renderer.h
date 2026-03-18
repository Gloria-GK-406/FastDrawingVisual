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

struct D3D11ShareD3D9RendererState;
struct SubmitFrameDiagnostics;

class D3D11ShareD3D9Renderer final {
 public:
  D3D11ShareD3D9Renderer(HWND hwnd, int width, int height);
  ~D3D11ShareD3D9Renderer();

  D3D11ShareD3D9Renderer(const D3D11ShareD3D9Renderer&) = delete;
  D3D11ShareD3D9Renderer& operator=(const D3D11ShareD3D9Renderer&) = delete;

  HRESULT Initialize();
  HRESULT Resize(int width, int height);
  HRESULT SubmitLayeredCommands(const void* framePacket, int framePacketBytes);
  HRESULT TryAcquirePresentSurface(void** outSurface9);
  HRESULT CopyReadyToPresentSurface();
  void OnFrontBufferAvailable(bool available);

  void RegisterMetrics();
  void UnregisterMetrics();

  int width() const { return width_; }
  int height() const { return height_; }
  std::uint64_t submittedFrameCount() const { return submittedFrameCount_; }

 private:
  bool ValidateFramePacket(const void* framePacket, int framePacketBytes) const;
  HRESULT SubmitLayeredCommandsAndPreparePresent(
      const LayeredFramePacket* framePacket);
  HRESULT BeginSubmitFrame(void*& currentRtv, int& drawSlotIndex);
  HRESULT SubmitCompiledBatches(const LayerPacket& layer, int layerIndex,
                                int drawSlotIndex, void* currentRtv,
                                SubmitFrameDiagnostics& diagnostics);
  void RecordFramePerformance(double drawDurationMs);

  HRESULT CreateDevicesAndResources();
  void ReleaseRendererResources();
  void ReleaseRenderTargetResources();
  HRESULT ResizeFrameResources(int width, int height);
  HRESULT CreateD3D11Device();
  HRESULT CreateD3D9Device();
  HRESULT CreateFrameResources();
  HRESULT EnsureTextRenderer();
  HRESULT CreateDrawPipeline();

 private:
  D3D11ShareD3D9RendererState* state_ = nullptr;
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
