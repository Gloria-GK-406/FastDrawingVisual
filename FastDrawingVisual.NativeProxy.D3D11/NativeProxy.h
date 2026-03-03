#pragma once

namespace FastDrawingVisual::NativeProxy {
[System::Flags]
public
enum class NativeProxyCapability : int {
  None = 0,
  CommandStream = 1 << 0,
  PresentSurface = 1 << 1,
  FrontBufferNotifications = 1 << 2,
  SwapChain = 1 << 3,
  ClearPresent = 1 << 4,
  Resize = 1 << 5,
};

public
ref class NativeProxyMetadata abstract sealed {
public:
  literal int ApiVersion = 1;
};

public
ref class NativeProxy abstract sealed {
public:
  static bool IsBridgeReady();
  static int GetBridgeCapabilities();
  static System::IntPtr CreateRenderer(System::IntPtr hwnd, int width, int height);
  static void DestroyRenderer(System::IntPtr renderer);
  static bool Resize(System::IntPtr renderer, int width, int height);

  // D3D9 command-stream API placeholders for signature compatibility.
  static bool SubmitCommands(System::IntPtr renderer, System::IntPtr commands,
                             int commandBytes);
  static bool TryAcquirePresentSurface(System::IntPtr renderer,
                                       System::IntPtr% surface9);
  static bool CopyReadyToPresentSurface(System::IntPtr renderer);
  static void OnFrontBufferAvailable(System::IntPtr renderer, bool available);

  // D3D11-specific API; D3D9 side keeps placeholders.
  static bool TryGetSwapChain(System::IntPtr renderer,
                              System::IntPtr% swapChain);
  static bool ClearAndPresent(System::IntPtr renderer, float red, float green,
                              float blue, float alpha, int syncInterval);
  static int GetLastErrorHr(System::IntPtr renderer);
};
} // namespace FastDrawingVisual::NativeProxy
