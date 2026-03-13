#pragma once

#include "D3D11SwapChainRenderer.h"

using namespace System;
using namespace FastDrawingVisual::CommandRuntime;
using namespace FastDrawingVisual::Rendering;

namespace FastDrawingVisual::Rendering::Backends {

public ref class D3D11SwapChainBackend sealed : public IRenderBackend,
                                                 public IDXGISwapChainProvider,
                                                 public IRenderBackendReadiness {
public:
  D3D11SwapChainBackend();
  ~D3D11SwapChainBackend();
  !D3D11SwapChainBackend();

  virtual bool Initialize(int width, int height);
  virtual void Resize(int width, int height);
  virtual IDrawingContext ^CreateDrawingContext(int width, int height);
  virtual IntPtr GetSwapChain();

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
  void SubmitFrame(BridgeLayeredFramePacket frame);
  void DestroyRenderer();
  void UpdateReadyState();
  void ThrowIfDisposed();

private:
  fdv::d3d11::D3D11SwapChainRenderer *renderer_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  bool isInitialized_ = false;
  bool isFaulted_ = false;
  bool isDisposed_ = false;
  bool lastReadyState_ = false;
  Action^ readyStateChanged_ = nullptr;
};

} // namespace FastDrawingVisual::Rendering::Backends
