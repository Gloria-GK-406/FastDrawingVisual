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

namespace fdv::d3d9 {

using LayerPacket = fdv::nativeproxy::shared::LayerPacket;
using LayeredFramePacket = fdv::nativeproxy::shared::LayeredFramePacket;
using RendererLockGuard = fdv::nativeproxy::shared::RendererLockGuard;

struct SurfaceSlot;
struct D3D9RendererState;

class D3D9Renderer final {
 public:
  D3D9Renderer(HWND hwnd, int width, int height);
  ~D3D9Renderer();

  D3D9Renderer(const D3D9Renderer&) = delete;
  D3D9Renderer& operator=(const D3D9Renderer&) = delete;

  HRESULT Initialize();
  HRESULT Resize(int width, int height);
  HRESULT SubmitLayeredCommands(const void* framePacket, int framePacketBytes);
  HRESULT TryAcquirePresentSurface(void** outSurface9);
  HRESULT CopyReadyToPresentSurface();
  void OnFrontBufferAvailable(bool available);
  void RegisterMetrics();
  void UnregisterMetrics();

 private:
  bool ValidateFramePacket(const void* framePacket, int framePacketBytes) const;
  HRESULT SubmitLayeredCommandsAndPreparePresent(
      const LayeredFramePacket* framePacket);
  HRESULT BeginSubmitFrame(SurfaceSlot*& drawSlot, int& drawSlotIndex);
  HRESULT SubmitCompiledBatches(SurfaceSlot* drawSlot, const LayerPacket& layer);

  D3D9RendererState* state_ = nullptr;
  int parseSubmitDurationMetricId_ = 0;
};

}  // namespace fdv::d3d9
