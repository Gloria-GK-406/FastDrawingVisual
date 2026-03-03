#include "BridgeNativeExports.h"
#include "BridgeRendererInternal.h"

#include <new>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

FDV_NATIVE_REGION_BEGIN

namespace {
constexpr int kCapabilityCommandStream = 1 << 0;
constexpr int kCapabilitySwapChain = 1 << 3;
constexpr int kCapabilityResize = 1 << 5;
} // namespace

extern "C" {
__declspec(dllexport) bool __cdecl FDV_IsBridgeReady() { return true; }

__declspec(dllexport) int __cdecl FDV_GetBridgeCapabilities() {
  return kCapabilityCommandStream | kCapabilitySwapChain | kCapabilityResize;
}

__declspec(dllexport) void* __cdecl FDV_CreateRenderer(void* hwnd, int width,
                                                       int height) {
  (void)hwnd;
  if (width <= 0 || height <= 0)
    return nullptr;

  auto* s = new (std::nothrow) BridgeRendererD3D11();
  if (!s)
    return nullptr;

  s->width = width;
  s->height = height;
  s->lastErrorHr = S_OK;
  InitializeCriticalSectionAndSpinCount(&s->cs, 1000);
  s->csInitialized = true;

  if (!CreateDeviceAndSwapChain(s)) {
    if (s->csInitialized) {
      DeleteCriticalSection(&s->cs);
      s->csInitialized = false;
    }
    delete s;
    return nullptr;
  }

  return s;
}

__declspec(dllexport) void __cdecl FDV_DestroyRenderer(void* renderer) {
  auto* s = static_cast<BridgeRendererD3D11*>(renderer);
  if (!s)
    return;

  EnterCriticalSection(&s->cs);
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
  if (!s)
    return false;

  EnterCriticalSection(&s->cs);
  bool ok = ResizeSwapChain(s, width, height);
  LeaveCriticalSection(&s->cs);
  return ok;
}

__declspec(dllexport) bool __cdecl FDV_SubmitCommands(void* renderer,
                                                      const void* commands,
                                                      int commandBytes) {
  auto* s = static_cast<BridgeRendererD3D11*>(renderer);
  if (!s)
    return false;

  EnterCriticalSection(&s->cs);
  bool ok = SubmitCommandsAndPresent(s, commands, commandBytes);
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
