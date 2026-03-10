// BridgeExports.cpp
// C ABI exports only. Implementation is split across helper/source modules.

#include <new>
#include <chrono>
#include <cwchar>

#include "BridgeNativeExports.h"
#include "BridgeRendererInternal.h"
#include "../FastDrawingVisual.LogCore/FdvLogCoreExports.h"

// The DXSDK NuGet package injects the correct lib via its .targets file,
// but list them explicitly so an IDE-only build also links correctly.
#pragma comment(lib, "d3d9.lib")

FDV_NATIVE_REGION_BEGIN

namespace {
constexpr int kCapabilityCommandStream = 1 << 0;
constexpr int kCapabilityPresentSurface = 1 << 1;
constexpr int kCapabilityFrontBufferNotifications = 1 << 2;
constexpr uint32_t kMetricWindowSec = 1;
constexpr const wchar_t *kParseSubmitMetricFormat =
    L"name={name} periodSec={periodSec}s samples={count} avgMs={avg} minMs={min} maxMs={max}";

void RegisterRendererMetrics(BridgeRenderer *s) {
  if (!s || s->parseSubmitDurationMetricId > 0) {
    return;
  }

  wchar_t metricName[104]{};
  swprintf_s(metricName, L"native.d3d9.r%p.parse_submit_ms",
             static_cast<void *>(s));
  FDVLOG_MetricSpec spec{};
  spec.name = metricName;
  spec.periodSec = kMetricWindowSec;
  spec.format = kParseSubmitMetricFormat;
  spec.level = FDVLOG_LevelInfo;
  s->parseSubmitDurationMetricId = FDVLOG_RegisterMetric(&spec);
}

void UnregisterRendererMetrics(BridgeRenderer *s) {
  if (!s || s->parseSubmitDurationMetricId <= 0) {
    return;
  }

  FDVLOG_UnregisterMetric(s->parseSubmitDurationMetricId);
  s->parseSubmitDurationMetricId = 0;
}
} // namespace

extern "C" {
__declspec(dllexport) bool __cdecl FDV_IsBridgeReady() {
  return true;
}

__declspec(dllexport) int __cdecl FDV_GetBridgeCapabilities() {
  return kCapabilityCommandStream | kCapabilityPresentSurface |
         kCapabilityFrontBufferNotifications;
}

__declspec(dllexport) void *__cdecl FDV_CreateRenderer(void *hwnd, int width,
                                                       int height) {
  if (hwnd == nullptr || width <= 0 || height <= 0)
    return nullptr;

  auto *s = new (std::nothrow) BridgeRenderer();
  if (!s)
    return nullptr;

  s->hwnd = static_cast<HWND>(hwnd);
  s->width = width;
  s->height = height;
  InitializeCriticalSectionAndSpinCount(&s->cs, 1000);
  s->csInitialized = true;

  if (!CreateDeviceAndSurface(s)) {
    DeleteCriticalSection(&s->cs);
    s->csInitialized = false;
    delete s;
    return nullptr;
  }

  RegisterRendererMetrics(s);
  return s;
}

__declspec(dllexport) void __cdecl FDV_DestroyRenderer(void *renderer) {
  auto *s = static_cast<BridgeRenderer *>(renderer);
  if (!s)
    return;

  UnregisterRendererMetrics(s);
  ReleaseDeviceResources(s);

  if (s->d3d9) {
    s->d3d9->Release();
    s->d3d9 = nullptr;
  }

  if (s->csInitialized) {
    DeleteCriticalSection(&s->cs);
    s->csInitialized = false;
  }

  delete s;
}

__declspec(dllexport) bool __cdecl FDV_Resize(void *renderer, int width,
                                              int height) {
  auto *s = static_cast<BridgeRenderer *>(renderer);
  if (!s || width <= 0 || height <= 0)
    return false;

  EnterCriticalSection(&s->cs);

  bool ok = false;
  if (s->width == width && s->height == height) {
    ok = true;
  } else {
    s->width = width;
    s->height = height;
    if (!s->device)
      ok = CreateDeviceAndSurface(s);
    else
      ok = CreateFrameResources(s);
  }

  LeaveCriticalSection(&s->cs);
  return ok;
}

__declspec(dllexport) bool __cdecl FDV_SubmitCommands(void *renderer,
                                                      const void *commands,
                                                      int commandBytes,
                                                      const void *blobs,
                                                      int blobBytes) {
  auto *s = static_cast<BridgeRenderer *>(renderer);
  if (!s || !commands || commandBytes <= 0)
    return false;
  if (!s->device)
    return false;

  EnterCriticalSection(&s->cs);
  RegisterRendererMetrics(s);

  HRESULT hr = s->device->TestCooperativeLevel();
  if (hr == D3DERR_DEVICENOTRESET) {
    if (!ResetDeviceAndSurface(s)) {
      LeaveCriticalSection(&s->cs);
      return false;
    }
  } else if (FAILED(hr)) {
    LeaveCriticalSection(&s->cs);
    return false;
  }

  int drawSlotIndex = FindSlotByState(s, SurfaceState::Ready);
  if (drawSlotIndex < 0) {
    LeaveCriticalSection(&s->cs);
    return false;
  }

  SurfaceSlot *drawSlot = &s->slots[drawSlotIndex];
  drawSlot->state = SurfaceState::Drawing;

  const auto parseSubmitStart = std::chrono::steady_clock::now();
  bool result =
      ExecuteCommands(s, drawSlot, static_cast<const uint8_t *>(commands),
                      commandBytes, static_cast<const uint8_t *>(blobs),
                      blobBytes);
  const auto parseSubmitEnd = std::chrono::steady_clock::now();
  if (s->parseSubmitDurationMetricId > 0) {
    const double parseSubmitDurationMs =
        std::chrono::duration<double, std::milli>(parseSubmitEnd -
                                                  parseSubmitStart)
            .count();
    FDVLOG_LogMetric(s->parseSubmitDurationMetricId, parseSubmitDurationMs);
  }
  if (result) {
    DemoteReadyForPresentSlots(s, drawSlotIndex);
    drawSlot->state = SurfaceState::ReadyForPresent;
  } else {
    drawSlot->state = SurfaceState::Ready;
  }

  LeaveCriticalSection(&s->cs);
  return result;
}

__declspec(dllexport) bool __cdecl FDV_TryAcquirePresentSurface(
    void *renderer, void **outSurface9) {
  if (!renderer || !outSurface9)
    return false;

  auto *s = static_cast<BridgeRenderer *>(renderer);
  *outSurface9 = nullptr;

  EnterCriticalSection(&s->cs);

  bool ok = false;
  if (s->device) {
    if (s->presentingSurface) {
      *outSurface9 = s->presentingSurface;
      ok = true;
    }
  }

  LeaveCriticalSection(&s->cs);
  return ok;
}

__declspec(dllexport) bool __cdecl FDV_CopyReadyToPresentSurface(void *renderer) {
  auto *s = static_cast<BridgeRenderer *>(renderer);
  if (!s)
    return false;

  EnterCriticalSection(&s->cs);

  bool ok = false;
  if (s->device) {
    const int readySlotIndex = FindSlotByState(s, SurfaceState::ReadyForPresent);

    if (readySlotIndex >= 0 && s->presentingSurface &&
        s->slots[readySlotIndex].renderTarget) {
      HRESULT hr = s->device->StretchRect(
          s->slots[readySlotIndex].renderTarget, nullptr,
          s->presentingSurface, nullptr, D3DTEXF_NONE);
      if (SUCCEEDED(hr)) {
        s->slots[readySlotIndex].state = SurfaceState::Ready;
        ok = true;
      }
    }
  }

  LeaveCriticalSection(&s->cs);
  return ok;
}

__declspec(dllexport) void __cdecl FDV_OnFrontBufferAvailable(void *renderer,
                                                              bool available) {
  auto *s = static_cast<BridgeRenderer *>(renderer);
  if (!s)
    return;

  EnterCriticalSection(&s->cs);
  s->frontBufferAvailable = available;

  if (!available) {
    for (int i = 0; i < kFrameCount; i++)
      s->slots[i].state = SurfaceState::Ready;
  } else if (s->device) {
    HRESULT hr = s->device->TestCooperativeLevel();
    if (hr == D3DERR_DEVICENOTRESET)
      ResetDeviceAndSurface(s);
  }

  LeaveCriticalSection(&s->cs);
}

__declspec(dllexport) bool __cdecl FDV_TryGetSwapChain(
    void *renderer, void **outSwapChain) {
  (void)renderer;
  if (outSwapChain)
    *outSwapChain = nullptr;
  return false;
}

__declspec(dllexport) bool __cdecl FDV_ClearAndPresent(
    void *renderer, float red, float green, float blue, float alpha,
    int syncInterval) {
  (void)renderer;
  (void)red;
  (void)green;
  (void)blue;
  (void)alpha;
  (void)syncInterval;
  return false;
}

__declspec(dllexport) int32_t __cdecl FDV_GetLastErrorHr(void *renderer) {
  (void)renderer;
  return static_cast<int32_t>(E_NOTIMPL);
}
}

FDV_NATIVE_REGION_END
