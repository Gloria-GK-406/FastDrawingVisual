#include "NativeD3D9BridgeProxy.h"
#include "BridgeNativeExports.h"

namespace FastDrawingVisual::NativeD3D9Bridge {
bool NativeD3D9BridgeProxy::IsBridgeReady() { return FDV_IsBridgeReady(); }

System::IntPtr NativeD3D9BridgeProxy::CreateRenderer(System::IntPtr hwnd,
                                                     int width, int height) {
  return System::IntPtr(FDV_CreateRenderer(hwnd.ToPointer(), width, height));
}

void NativeD3D9BridgeProxy::DestroyRenderer(System::IntPtr renderer) {
  FDV_DestroyRenderer(renderer.ToPointer());
}

bool NativeD3D9BridgeProxy::Resize(System::IntPtr renderer, int width,
                                   int height) {
  return FDV_Resize(renderer.ToPointer(), width, height);
}

bool NativeD3D9BridgeProxy::SubmitCommands(System::IntPtr renderer,
                                           System::IntPtr commands,
                                           int commandBytes) {
  return FDV_SubmitCommands(renderer.ToPointer(), commands.ToPointer(),
                            commandBytes);
}

bool NativeD3D9BridgeProxy::TryAcquirePresentSurface(System::IntPtr renderer,
                                                     System::IntPtr % surface9) {
  void *surface = nullptr;
  bool ok = FDV_TryAcquirePresentSurface(renderer.ToPointer(), &surface);
  surface9 = System::IntPtr(surface);
  return ok;
}

bool NativeD3D9BridgeProxy::CopyReadyToPresentSurface(System::IntPtr renderer) {
  return FDV_CopyReadyToPresentSurface(renderer.ToPointer());
}

void NativeD3D9BridgeProxy::OnFrontBufferAvailable(System::IntPtr renderer,
                                                   bool available) {
  FDV_OnFrontBufferAvailable(renderer.ToPointer(), available);
}
} // namespace FastDrawingVisual::NativeD3D9Bridge
