#pragma once

namespace FastDrawingVisual::NativeProxy {
[System::Flags]
public
enum class NativeProxyCapability : int {
  None = 0,
  CommandStream = 1 << 0,
  PresentSurface = 1 << 1,
  FrontBufferNotifications = 1 << 2,
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
  static System::IntPtr CreateRenderer(System::IntPtr hwnd, int width,
                                       int height);
  static void DestroyRenderer(System::IntPtr renderer);
  static bool Resize(System::IntPtr renderer, int width, int height);
  static bool SubmitCommands(System::IntPtr renderer, System::IntPtr commands,
                             int commandBytes);
  static bool TryAcquirePresentSurface(System::IntPtr renderer,
                                       System::IntPtr % surface9);
  static bool CopyReadyToPresentSurface(System::IntPtr renderer);
  static void OnFrontBufferAvailable(System::IntPtr renderer, bool available);
};
} // namespace FastDrawingVisual::NativeProxy
