#include "D3D11ShareD3D9Backend.h"
#include "D3D11ShareD3D9Renderer.h"

#include <new>
#include <vcclr.h>

namespace FastDrawingVisual::Rendering::Backends {

D3D11ShareD3D9Backend::D3D11ShareD3D9Backend() {}

D3D11ShareD3D9Backend::~D3D11ShareD3D9Backend() {
  if (isDisposed_) {
    return;
  }

  DestroyRenderer();

  isDisposed_ = true;
  UpdateReadyState();
}

D3D11ShareD3D9Backend::!D3D11ShareD3D9Backend() {
  DestroyRenderer();
  isDisposed_ = true;
}

bool D3D11ShareD3D9Backend::Initialize(int width, int height) {
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

void D3D11ShareD3D9Backend::Resize(int width, int height) {
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

IDrawingContext ^D3D11ShareD3D9Backend::CreateDrawingContext(int width,
                                                             int height) {
  ThrowIfDisposed();
  if (!IsReadyForRendering) {
    return nullptr;
  }

  return gcnew LayeredCommandRecordingContext(
      width, height,
      gcnew Action<LayeredFramePacket>(this,
                                       &D3D11ShareD3D9Backend::SubmitFrame));
}

IntPtr D3D11ShareD3D9Backend::GetSurface9() {
  ThrowIfDisposed();
  if (!IsReadyForRendering) {
    return IntPtr::Zero;
  }

  void* surface = nullptr;
  const HRESULT hr =
      renderer_ == nullptr ? E_UNEXPECTED
                           : renderer_->TryAcquirePresentSurface(&surface);
  if (FAILED(hr)) {
    isFaulted_ = true;
    UpdateReadyState();
    return IntPtr::Zero;
  }

  return IntPtr(surface);
}

bool D3D11ShareD3D9Backend::CopyReadyToPresentSurface() {
  ThrowIfDisposed();
  if (!IsReadyForRendering) {
    return false;
  }

  const HRESULT hr =
      renderer_ == nullptr ? E_UNEXPECTED : renderer_->CopyReadyToPresentSurface();
  if (FAILED(hr)) {
    isFaulted_ = true;
    UpdateReadyState();
    return false;
  }

  return hr == S_OK;
}

void D3D11ShareD3D9Backend::NotifyFrontBufferAvailable(bool available) {
  ThrowIfDisposed();

  if (!available) {
    isInitialized_ = false;
    isFaulted_ = true;
    if (renderer_ != nullptr) {
      renderer_->OnFrontBufferAvailable(false);
    }

    UpdateReadyState();
    return;
  }

  if (width_ <= 0 || height_ <= 0) {
    return;
  }

  if (renderer_ == nullptr && !CreateNativeRenderer(width_, height_)) {
    UpdateReadyState();
    return;
  }

  if (renderer_ != nullptr) {
    renderer_->OnFrontBufferAvailable(true);
    if (FAILED(renderer_->Resize(width_, height_))) {
      isFaulted_ = true;
      UpdateReadyState();
      return;
    }
  }

  isFaulted_ = false;
  isInitialized_ = true;
  UpdateReadyState();
}

bool D3D11ShareD3D9Backend::CreateNativeRenderer(int width, int height) {
  auto* renderer = new (std::nothrow) fdv::d3d11::D3D11ShareD3D9Renderer(
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

void D3D11ShareD3D9Backend::SubmitFrame(LayeredFramePacket frame) {
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

void D3D11ShareD3D9Backend::DestroyRenderer() {
  if (renderer_ == nullptr) {
    return;
  }

  renderer_->UnregisterMetrics();
  delete renderer_;
  renderer_ = nullptr;
  isInitialized_ = false;
  isFaulted_ = false;
}

void D3D11ShareD3D9Backend::UpdateReadyState() {
  const bool isReady = IsReadyForRendering;
  if (lastReadyState_ == isReady) {
    return;
  }

  lastReadyState_ = isReady;
  if (readyStateChanged_ != nullptr) {
    readyStateChanged_();
  }
}

void D3D11ShareD3D9Backend::ReadyStateChanged::add(Action^ handler) {
  readyStateChanged_ += handler;
}

void D3D11ShareD3D9Backend::ReadyStateChanged::remove(Action^ handler) {
  readyStateChanged_ -= handler;
}

void D3D11ShareD3D9Backend::ThrowIfDisposed() {
  if (isDisposed_) {
    throw gcnew ObjectDisposedException("D3D11ShareD3D9Backend");
  }
}

bool D3D11ShareD3D9Backend::IsReadyForRendering::get() {
  return !isDisposed_ && isInitialized_ && !isFaulted_ && renderer_ != nullptr;
}

generic <typename TCapability> where TCapability : ref class
bool D3D11ShareD3D9Backend::TryGetCapability(
    [System::Runtime::InteropServices::Out] TCapability % capability) {
  Type^ capabilityType = TCapability::typeid;
  Type^ instanceType = GetType();
  if (capabilityType == nullptr ||
      !capabilityType->IsAssignableFrom(instanceType)) {
    capability = TCapability();
    return false;
  }

  capability = safe_cast<TCapability>(this);
  return true;
}

} // namespace FastDrawingVisual::Rendering::Backends
