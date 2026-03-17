#include "D3D11SwapChainBackend.h"

#include <new>
#include <vcclr.h>

namespace FastDrawingVisual::Rendering::Backends {

D3D11SwapChainBackend::D3D11SwapChainBackend() {}

D3D11SwapChainBackend::~D3D11SwapChainBackend() {
  if (isDisposed_) {
    return;
  }

  DestroyRenderer();
  isDisposed_ = true;
  UpdateReadyState();
}

D3D11SwapChainBackend::!D3D11SwapChainBackend() {
  DestroyRenderer();
  isDisposed_ = true;
}

bool D3D11SwapChainBackend::Initialize(int width, int height) {
  ThrowIfDisposed();
  if (width <= 0 || height <= 0) {
    throw gcnew ArgumentException(
        "Width and height must be greater than zero.");
  }

  width_ = width;
  height_ = height;

  if (renderer_ == nullptr && !CreateNativeRenderer(width, height)) {
    UpdateReadyState();
    return false;
  }

  isInitialized_ = true;
  isFaulted_ = false;
  UpdateReadyState();
  return true;
}

void D3D11SwapChainBackend::Resize(int width, int height) {
  ThrowIfDisposed();
  if (width <= 0 || height <= 0) {
    throw gcnew ArgumentException(
        "Width and height must be greater than zero.");
  }

  width_ = width;
  height_ = height;

  if (renderer_ == nullptr || !isInitialized_ || isFaulted_) {
    UpdateReadyState();
    return;
  }

  if (FAILED(renderer_->Resize(width, height))) {
    isFaulted_ = true;
  }

  UpdateReadyState();
}

IDrawingContext ^D3D11SwapChainBackend::CreateDrawingContext(int width,
                                                             int height) {
  ThrowIfDisposed();
  if (!IsReadyForRendering) {
    return nullptr;
  }

  return gcnew LayeredCommandRecordingContext(
      width, height,
      gcnew Action<LayeredFramePacket>(
          this, &D3D11SwapChainBackend::SubmitFrame));
}

IntPtr D3D11SwapChainBackend::GetSwapChain() {
  ThrowIfDisposed();
  if (!IsReadyForRendering) {
    return IntPtr::Zero;
  }

  void *swapChain = nullptr;
  if (renderer_ == nullptr || FAILED(renderer_->TryGetSwapChain(&swapChain))) {
    isFaulted_ = true;
    UpdateReadyState();
    return IntPtr::Zero;
  }

  return IntPtr(swapChain);
}

bool D3D11SwapChainBackend::CreateNativeRenderer(int width, int height) {
  auto *renderer = new (std::nothrow) fdv::d3d11::D3D11SwapChainRenderer(
      width, height);
  if (renderer == nullptr) {
    return false;
  }

  const HRESULT initializeHr = renderer->Initialize();
  if (FAILED(initializeHr)) {
    delete renderer;
    return false;
  }

  renderer->RegisterMetrics();
  renderer_ = renderer;
  return true;
}

void D3D11SwapChainBackend::SubmitFrame(LayeredFramePacket frame) {
  ThrowIfDisposed();
  if (!IsReadyForRendering || !frame.HasAnyCommands) {
    return;
  }

  LayeredFramePacket frameCopy = frame;
  pin_ptr<LayeredFramePacket> framePacket = &frameCopy;

  if (renderer_ == nullptr ||
      FAILED(renderer_->SubmitLayeredCommands(framePacket,
                                              sizeof(LayeredFramePacket)))) {
    isFaulted_ = true;
  }

  UpdateReadyState();
}

void D3D11SwapChainBackend::DestroyRenderer() {
  if (renderer_ == nullptr) {
    return;
  }

  renderer_->UnregisterMetrics();
  delete renderer_;
  renderer_ = nullptr;
  isInitialized_ = false;
  isFaulted_ = false;
}

void D3D11SwapChainBackend::UpdateReadyState() {
  const bool isReady = IsReadyForRendering;
  if (lastReadyState_ == isReady) {
    return;
  }

  lastReadyState_ = isReady;
  if (readyStateChanged_ != nullptr) {
    readyStateChanged_();
  }
}

void D3D11SwapChainBackend::ReadyStateChanged::add(Action^ handler) {
  readyStateChanged_ += handler;
}

void D3D11SwapChainBackend::ReadyStateChanged::remove(Action^ handler) {
  readyStateChanged_ -= handler;
}

void D3D11SwapChainBackend::ThrowIfDisposed() {
  if (isDisposed_) {
    throw gcnew ObjectDisposedException("D3D11SwapChainBackend");
  }
}

bool D3D11SwapChainBackend::IsReadyForRendering::get() {
  return !isDisposed_ && isInitialized_ && !isFaulted_ &&
         renderer_ != nullptr;
}

generic <typename TCapability> where TCapability : ref class
bool D3D11SwapChainBackend::TryGetCapability(
    [System::Runtime::InteropServices::Out] TCapability % capability) {
  Type^ capabilityType = TCapability::typeid;
  Type^ instanceType = GetType();
  if (capabilityType == nullptr || !capabilityType->IsAssignableFrom(instanceType)) {
    capability = TCapability();
    return false;
  }

  capability = safe_cast<TCapability>(this);
  return true;
}

} // namespace FastDrawingVisual::Rendering::Backends
