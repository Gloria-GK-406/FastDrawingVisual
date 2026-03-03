#include "NativeProxy.h"

#include "BridgeNativeExports.h"

namespace FastDrawingVisual::NativeProxy {
bool NativeProxy::IsBridgeReady() { return FDV_IsBridgeReady(); }

int NativeProxy::GetBridgeCapabilities() {
  return FDV_GetBridgeCapabilities();
}

System::IntPtr NativeProxy::CreateRenderer(System::IntPtr hwnd, int width,
                                           int height) {
  return System::IntPtr(FDV_CreateRenderer(hwnd.ToPointer(), width, height));
}

void NativeProxy::DestroyRenderer(System::IntPtr renderer) {
  FDV_DestroyRenderer(renderer.ToPointer());
}

bool NativeProxy::Resize(System::IntPtr renderer, int width, int height) {
  return FDV_Resize(renderer.ToPointer(), width, height);
}

bool NativeProxy::SubmitCommands(System::IntPtr renderer,
                                 System::IntPtr commands, int commandBytes) {
  return FDV_SubmitCommands(renderer.ToPointer(), commands.ToPointer(),
                            commandBytes);
}

bool NativeProxy::TryAcquirePresentSurface(System::IntPtr renderer,
                                           System::IntPtr% surface9) {
  void* nativeSurface = nullptr;
  bool ok = FDV_TryAcquirePresentSurface(renderer.ToPointer(), &nativeSurface);
  surface9 = System::IntPtr(nativeSurface);
  return ok;
}

bool NativeProxy::CopyReadyToPresentSurface(System::IntPtr renderer) {
  return FDV_CopyReadyToPresentSurface(renderer.ToPointer());
}

void NativeProxy::OnFrontBufferAvailable(System::IntPtr renderer,
                                         bool available) {
  FDV_OnFrontBufferAvailable(renderer.ToPointer(), available);
}

bool NativeProxy::TryGetSwapChain(System::IntPtr renderer,
                                  System::IntPtr% swapChain) {
  void* nativeSwapChain = nullptr;
  bool ok = FDV_TryGetSwapChain(renderer.ToPointer(), &nativeSwapChain);
  swapChain = System::IntPtr(nativeSwapChain);
  return ok;
}

bool NativeProxy::ClearAndPresent(System::IntPtr renderer, float red,
                                  float green, float blue, float alpha,
                                  int syncInterval) {
  return FDV_ClearAndPresent(renderer.ToPointer(), red, green, blue, alpha,
                             syncInterval);
}

int NativeProxy::GetLastErrorHr(System::IntPtr renderer) {
  return FDV_GetLastErrorHr(renderer.ToPointer());
}
} // namespace FastDrawingVisual::NativeProxy

