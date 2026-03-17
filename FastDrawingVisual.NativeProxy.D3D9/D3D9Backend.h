#pragma once

#include "D3D9Renderer.h"

using namespace System;
using namespace FastDrawingVisual::CommandRuntime;
using namespace FastDrawingVisual::Rendering;

namespace System::Windows {
ref class Application;
ref class Window;
}

namespace System::Windows::Interop {
ref class HwndSource;
}

namespace FastDrawingVisual::Rendering::Backends {

public ref class D3D9Backend sealed : public IRenderBackend,
                                       public ID3D9PresentationSource,
                                       public IRenderBackendReadiness {
 public:
  D3D9Backend();
  ~D3D9Backend();
  !D3D9Backend();

  virtual bool Initialize(int width, int height);
  virtual void Resize(int width, int height);
  virtual IDrawingContext ^CreateDrawingContext(int width, int height);
  virtual IntPtr GetSurface9();
  virtual bool CopyReadyToPresentSurface();
  virtual void NotifyFrontBufferAvailable(bool available);

  generic <typename TCapability> where TCapability : ref class
  virtual bool TryGetCapability(
      [System::Runtime::InteropServices::Out] TCapability % capability);

  virtual property bool IsReadyForRendering { bool get(); }

  virtual event Action ^ReadyStateChanged {
    void add(Action ^handler);
    void remove(Action ^handler);
  }

 private:
  bool CreateNativeRenderer(int width, int height);
  void SubmitFrame(LayeredFramePacket frame);
  void DestroyRenderer();
  IntPtr GetOrCreateDeviceHwnd();
  void UpdateReadyState();
  void ThrowIfDisposed();

 private:
  fdv::d3d9::D3D9Renderer *renderer_ = nullptr;
  System::Windows::Interop::HwndSource^ fallbackHwndSource_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  bool isInitialized_ = false;
  bool isFaulted_ = false;
  bool isDisposed_ = false;
  bool lastReadyState_ = false;
  Action^ readyStateChanged_ = nullptr;
};

}  // namespace FastDrawingVisual::Rendering::Backends
