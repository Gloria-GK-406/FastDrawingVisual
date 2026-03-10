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
                                 System::IntPtr commands, int commandBytes,
                                 System::IntPtr blobs, int blobBytes) {
  return FDV_SubmitCommands(renderer.ToPointer(), commands.ToPointer(),
                            commandBytes, blobs.ToPointer(), blobBytes);
}

bool NativeProxy::SubmitLayeredCommands(System::IntPtr renderer,
                                        System::IntPtr framePacket,
                                        int framePacketBytes) {
  return FDV_SubmitLayeredCommands(renderer.ToPointer(), framePacket.ToPointer(),
                                   framePacketBytes);
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

int NativeProxy::GetLastErrorHr(System::IntPtr renderer) {
  return FDV_GetLastErrorHr(renderer.ToPointer());
}
} // namespace FastDrawingVisual::NativeProxy
