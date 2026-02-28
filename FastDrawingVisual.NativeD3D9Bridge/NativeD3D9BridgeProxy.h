#pragma once

namespace FastDrawingVisual::NativeD3D9Bridge {
public
ref class BridgeMetadata abstract sealed {
public:
  literal int ApiVersion = 1;
};

public
ref class NativeD3D9BridgeProxy abstract sealed {
public:
  static bool IsBridgeReady();
  static System::IntPtr CreateRenderer(System::IntPtr hwnd, int width,
                                       int height);
  static void DestroyRenderer(System::IntPtr renderer);
  static bool Resize(System::IntPtr renderer, int width, int height);
  static bool SubmitCommands(System::IntPtr renderer, System::IntPtr commands,
                             int commandBytes);
  static bool TryAcquirePresentSurface(System::IntPtr renderer,
                                       System::IntPtr % surface9);
  static void OnSurfacePresented(System::IntPtr renderer);
  static void OnFrontBufferAvailable(System::IntPtr renderer, bool available);
};
} // namespace FastDrawingVisual::NativeD3D9Bridge
