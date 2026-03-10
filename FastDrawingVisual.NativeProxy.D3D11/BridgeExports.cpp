#include "BridgeNativeExports.h"
#include "BridgeRendererInternal.h"
#include "../FastDrawingVisual.LogCore/FdvLogCoreExports.h"

#include <chrono>
#include <cstdarg>
#include <cwchar>
#include <new>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

FDV_NATIVE_REGION_BEGIN

namespace {
constexpr int kCapabilityCommandStream = 1 << 0;
constexpr int kCapabilitySwapChain = 1 << 3;
constexpr int kCapabilityResize = 1 << 5;
constexpr uint32_t kMetricWindowSec = 1;
constexpr const wchar_t* kLogCategory = L"NativeProxy.D3D11";
constexpr const wchar_t* kDrawMetricFormat =
    L"name={name} periodSec={periodSec}s samples={count} avgMs={avg} minMs={min} maxMs={max}";
constexpr const wchar_t* kFpsMetricFormat =
    L"name={name} periodSec={periodSec}s samples={count} avgFps={avg} minFps={min} maxFps={max}";

void LogNative(int level, const wchar_t* message, bool etw) {
  FDVLOG_Log(level, kLogCategory, message, false);
  if (etw) {
    FDVLOG_WriteETW(level, kLogCategory, message, false);
  }
}

void LogNativef(int level, bool etw, const wchar_t* format, ...) {
  if (!format) {
    return;
  }

  wchar_t buffer[512]{};
  va_list args{};
  va_start(args, format);
  _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
  va_end(args);
  LogNative(level, buffer, etw);
}

void RegisterRendererMetrics(BridgeRendererD3D11* s) {
  if (!s) {
    return;
  }

  bool changed = false;

  if (s->parseSubmitDurationMetricId <= 0) {
    wchar_t metricName[104]{};
    swprintf_s(metricName, L"native.d3d11.r%p.parse_submit_ms",
               static_cast<void*>(s));
    FDVLOG_MetricSpec spec{};
    spec.name = metricName;
    spec.periodSec = kMetricWindowSec;
    spec.format = kDrawMetricFormat;
    spec.level = FDVLOG_LevelInfo;
    s->parseSubmitDurationMetricId = FDVLOG_RegisterMetric(&spec);
    changed = (s->parseSubmitDurationMetricId > 0) || changed;
  }

  if (s->drawDurationMetricId <= 0) {
    wchar_t metricName[96]{};
    swprintf_s(metricName, L"native.d3d11.r%p.draw_ms", static_cast<void*>(s));
    FDVLOG_MetricSpec spec{};
    spec.name = metricName;
    spec.periodSec = kMetricWindowSec;
    spec.format = kDrawMetricFormat;
    spec.level = FDVLOG_LevelInfo;
    s->drawDurationMetricId = FDVLOG_RegisterMetric(&spec);
    changed = (s->drawDurationMetricId > 0) || changed;
  }

  if (s->fpsMetricId <= 0) {
    wchar_t metricName[96]{};
    swprintf_s(metricName, L"native.d3d11.r%p.fps", static_cast<void*>(s));
    FDVLOG_MetricSpec spec{};
    spec.name = metricName;
    spec.periodSec = kMetricWindowSec;
    spec.format = kFpsMetricFormat;
    spec.level = FDVLOG_LevelInfo;
    s->fpsMetricId = FDVLOG_RegisterMetric(&spec);
    changed = (s->fpsMetricId > 0) || changed;
  }

  if (changed) {
    LogNativef(FDVLOG_LevelDebug, false,
               L"renderer=0x%p metric registration parseSubmitMetricId=%d drawMetricId=%d fpsMetricId=%d",
               static_cast<void*>(s), s->parseSubmitDurationMetricId,
               s->drawDurationMetricId, s->fpsMetricId);
  }
}

void UnregisterRendererMetrics(BridgeRendererD3D11* s) {
  if (!s) {
    return;
  }

  if (s->parseSubmitDurationMetricId > 0) {
    FDVLOG_UnregisterMetric(s->parseSubmitDurationMetricId);
    s->parseSubmitDurationMetricId = 0;
  }

  if (s->drawDurationMetricId > 0) {
    FDVLOG_UnregisterMetric(s->drawDurationMetricId);
    s->drawDurationMetricId = 0;
  }

  if (s->fpsMetricId > 0) {
    FDVLOG_UnregisterMetric(s->fpsMetricId);
    s->fpsMetricId = 0;
  }
}
} // namespace

extern "C" {
__declspec(dllexport) bool __cdecl FDV_IsBridgeReady() { return true; }

__declspec(dllexport) int __cdecl FDV_GetBridgeCapabilities() {
  return kCapabilityCommandStream | kCapabilitySwapChain | kCapabilityResize;
}

__declspec(dllexport) void* __cdecl FDV_CreateRenderer(void* hwnd, int width,
                                                       int height) {
  (void)hwnd;
  if (width <= 0 || height <= 0) {
    LogNativef(FDVLOG_LevelWarn, true,
               L"FDV_CreateRenderer rejected invalid size width=%d height=%d.",
               width, height);
    return nullptr;
  }

  auto* s = new (std::nothrow) BridgeRendererD3D11();
  if (!s) {
    LogNative(FDVLOG_LevelError,
              L"FDV_CreateRenderer allocation failed for BridgeRendererD3D11.",
              true);
    return nullptr;
  }

  s->width = width;
  s->height = height;
  s->lastErrorHr = S_OK;
  InitializeCriticalSectionAndSpinCount(&s->cs, 1000);
  s->csInitialized = true;

  if (!CreateDeviceAndSwapChain(s)) {
    LogNativef(FDVLOG_LevelError, true,
               L"FDV_CreateRenderer failed width=%d height=%d hr=0x%08X.",
               width, height, static_cast<unsigned int>(s->lastErrorHr));
    if (s->csInitialized) {
      DeleteCriticalSection(&s->cs);
      s->csInitialized = false;
    }
    delete s;
    return nullptr;
  }

  RegisterRendererMetrics(s);
  LogNativef(FDVLOG_LevelInfo, true,
             L"FDV_CreateRenderer success renderer=0x%p width=%d height=%d.",
             static_cast<void*>(s), width, height);
  return s;
}

__declspec(dllexport) void __cdecl FDV_DestroyRenderer(void* renderer) {
  auto* s = static_cast<BridgeRendererD3D11*>(renderer);
  if (!s)
    return;

  EnterCriticalSection(&s->cs);
  LogNativef(FDVLOG_LevelInfo, false,
             L"FDV_DestroyRenderer begin renderer=0x%p frames=%llu lastHr=0x%08X.",
             static_cast<void*>(s),
             static_cast<unsigned long long>(s->submittedFrameCount),
             static_cast<unsigned int>(s->lastErrorHr));
  UnregisterRendererMetrics(s);
  ReleaseRendererResources(s);
  LeaveCriticalSection(&s->cs);

  if (s->csInitialized) {
    DeleteCriticalSection(&s->cs);
    s->csInitialized = false;
  }

  delete s;
}

__declspec(dllexport) bool __cdecl FDV_Resize(void* renderer, int width,
                                              int height) {
  auto* s = static_cast<BridgeRendererD3D11*>(renderer);
  if (!s) {
    LogNative(FDVLOG_LevelWarn, L"FDV_Resize ignored because renderer is null.",
              false);
    return false;
  }

  EnterCriticalSection(&s->cs);
  const int oldWidth = s->width;
  const int oldHeight = s->height;
  bool ok = ResizeSwapChain(s, width, height);
  if (ok) {
    LogNativef(FDVLOG_LevelInfo, true,
               L"FDV_Resize success renderer=0x%p from=%dx%d to=%dx%d.",
               static_cast<void*>(s), oldWidth, oldHeight, width, height);
  } else {
    LogNativef(FDVLOG_LevelWarn, true,
               L"FDV_Resize failed renderer=0x%p from=%dx%d to=%dx%d hr=0x%08X.",
               static_cast<void*>(s), oldWidth, oldHeight, width, height,
               static_cast<unsigned int>(s->lastErrorHr));
  }
  LeaveCriticalSection(&s->cs);
  return ok;
}

__declspec(dllexport) bool __cdecl FDV_SubmitCommands(void* renderer,
                                                      const void* commands,
                                                      int commandBytes,
                                                      const void* blobs,
                                                      int blobBytes) {
  auto* s = static_cast<BridgeRendererD3D11*>(renderer);
  if (!s) {
    LogNative(FDVLOG_LevelWarn,
              L"FDV_SubmitCommands ignored because renderer is null.", false);
    return false;
  }

  EnterCriticalSection(&s->cs);
  RegisterRendererMetrics(s);
  const auto parseSubmitStart = std::chrono::steady_clock::now();
  bool ok = SubmitCommandsAndPresent(s, commands, commandBytes, blobs, blobBytes);
  const auto parseSubmitEnd = std::chrono::steady_clock::now();
  if (s->parseSubmitDurationMetricId > 0) {
    const double parseSubmitDurationMs =
        std::chrono::duration<double, std::milli>(parseSubmitEnd -
                                                  parseSubmitStart)
            .count();
    FDVLOG_LogMetric(s->parseSubmitDurationMetricId, parseSubmitDurationMs);
  }
  if (!ok) {
    LogNativef(FDVLOG_LevelError, true,
               L"FDV_SubmitCommands failed renderer=0x%p bytes=%d blobBytes=%d hr=0x%08X.",
               static_cast<void*>(s), commandBytes, blobBytes,
               static_cast<unsigned int>(s->lastErrorHr));
  }
  LeaveCriticalSection(&s->cs);
  return ok;
}

__declspec(dllexport) bool __cdecl FDV_SubmitLayeredCommands(
    void* renderer, const void* framePacket, int framePacketBytes) {
  auto* s = static_cast<BridgeRendererD3D11*>(renderer);
  if (!s) {
    LogNative(FDVLOG_LevelWarn,
              L"FDV_SubmitLayeredCommands ignored because renderer is null.",
              false);
    return false;
  }

  if (!framePacket ||
      framePacketBytes < static_cast<int>(sizeof(BridgeLayeredFramePacketNative))) {
    EnterCriticalSection(&s->cs);
    SetLastError(s, E_INVALIDARG);
    LeaveCriticalSection(&s->cs);
    LogNativef(FDVLOG_LevelWarn, true,
               L"FDV_SubmitLayeredCommands rejected invalid packet renderer=0x%p bytes=%d.",
               static_cast<void*>(s), framePacketBytes);
    return false;
  }

  EnterCriticalSection(&s->cs);
  RegisterRendererMetrics(s);
  const auto parseSubmitStart = std::chrono::steady_clock::now();
  bool ok = SubmitLayeredCommandsAndPresent(
      s, static_cast<const BridgeLayeredFramePacketNative*>(framePacket));
  const auto parseSubmitEnd = std::chrono::steady_clock::now();
  if (s->parseSubmitDurationMetricId > 0) {
    const double parseSubmitDurationMs =
        std::chrono::duration<double, std::milli>(parseSubmitEnd -
                                                  parseSubmitStart)
            .count();
    FDVLOG_LogMetric(s->parseSubmitDurationMetricId, parseSubmitDurationMs);
  }
  if (!ok) {
    LogNativef(FDVLOG_LevelError, true,
               L"FDV_SubmitLayeredCommands failed renderer=0x%p packetBytes=%d hr=0x%08X.",
               static_cast<void*>(s), framePacketBytes,
               static_cast<unsigned int>(s->lastErrorHr));
  }
  LeaveCriticalSection(&s->cs);
  return ok;
}

__declspec(dllexport) bool __cdecl FDV_TryAcquirePresentSurface(
    void* renderer, void** outSurface9) {
  (void)renderer;
  if (outSurface9)
    *outSurface9 = nullptr;
  return false;
}

__declspec(dllexport) bool __cdecl FDV_CopyReadyToPresentSurface(
    void* renderer) {
  (void)renderer;
  return false;
}

__declspec(dllexport) void __cdecl FDV_OnFrontBufferAvailable(void* renderer,
                                                              bool available) {
  (void)renderer;
  (void)available;
}

__declspec(dllexport) bool __cdecl FDV_TryGetSwapChain(void* renderer,
                                                       void** outSwapChain) {
  if (!renderer || !outSwapChain)
    return false;

  auto* s = static_cast<BridgeRendererD3D11*>(renderer);
  *outSwapChain = nullptr;

  EnterCriticalSection(&s->cs);
  bool ok = false;
  if (s->swapChain != nullptr) {
    *outSwapChain = s->swapChain;
    ok = true;
  } else {
    s->lastErrorHr = E_UNEXPECTED;
  }
  LeaveCriticalSection(&s->cs);

  return ok;
}

__declspec(dllexport) int32_t __cdecl FDV_GetLastErrorHr(void* renderer) {
  auto* s = static_cast<BridgeRendererD3D11*>(renderer);
  if (!s)
    return static_cast<int32_t>(E_POINTER);

  return static_cast<int32_t>(s->lastErrorHr);
}
}

FDV_NATIVE_REGION_END
