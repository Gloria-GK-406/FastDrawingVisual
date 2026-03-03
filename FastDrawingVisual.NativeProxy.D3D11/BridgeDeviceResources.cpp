#include "BridgeRendererInternal.h"

namespace {
constexpr UINT kBufferCount = 3;
constexpr UINT kCreationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

template <typename T> void SafeRelease(T** ptr) {
  if (ptr == nullptr || *ptr == nullptr)
    return;

  (*ptr)->Release();
  *ptr = nullptr;
}

void SetLastError(BridgeRendererD3D11* s, HRESULT hr) {
  if (s != nullptr)
    s->lastErrorHr = hr;
}

bool CreateRenderTarget(BridgeRendererD3D11* s) {
  if (!s || !s->device || !s->swapChain) {
    SetLastError(s, E_UNEXPECTED);
    return false;
  }

  ID3D11Texture2D* backBuffer = nullptr;
  HRESULT hr = s->swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void**>(&backBuffer));
  if (FAILED(hr) || backBuffer == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  hr = s->device->CreateRenderTargetView(backBuffer, nullptr, &s->rtv0);
  backBuffer->Release();
  if (FAILED(hr) || s->rtv0 == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  SetLastError(s, S_OK);
  return true;
}

bool EnsureFactory(BridgeRendererD3D11* s) {
  if (!s || !s->device) {
    SetLastError(s, E_UNEXPECTED);
    return false;
  }

  if (s->dxgiFactory != nullptr)
    return true;

  IDXGIDevice* dxgiDevice = nullptr;
  IDXGIAdapter* adapter = nullptr;

  HRESULT hr = s->device->QueryInterface(__uuidof(IDXGIDevice),
                                         reinterpret_cast<void**>(&dxgiDevice));
  if (FAILED(hr) || dxgiDevice == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  hr = dxgiDevice->GetAdapter(&adapter);
  dxgiDevice->Release();
  if (FAILED(hr) || adapter == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  hr = adapter->GetParent(__uuidof(IDXGIFactory2),
                          reinterpret_cast<void**>(&s->dxgiFactory));
  adapter->Release();
  if (FAILED(hr) || s->dxgiFactory == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  SetLastError(s, S_OK);
  return true;
}

bool CreateSwapChain(BridgeRendererD3D11* s) {
  if (!s || !s->device || !s->dxgiFactory) {
    SetLastError(s, E_UNEXPECTED);
    return false;
  }

  DXGI_SWAP_CHAIN_DESC1 swapDesc = {};
  swapDesc.Width = static_cast<UINT>(s->width);
  swapDesc.Height = static_cast<UINT>(s->height);
  swapDesc.Format = kSwapChainFormat;
  swapDesc.Stereo = FALSE;
  swapDesc.SampleDesc.Count = 1;
  swapDesc.SampleDesc.Quality = 0;
  swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swapDesc.BufferCount = kBufferCount;
  swapDesc.Scaling = DXGI_SCALING_STRETCH;
  swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  swapDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  swapDesc.Flags = 0;

  HRESULT hr = s->dxgiFactory->CreateSwapChainForComposition(
      s->device, &swapDesc, nullptr, &s->swapChain);
  if (FAILED(hr) || s->swapChain == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  SetLastError(s, S_OK);
  return true;
}
} // namespace

void ReleaseRendererResources(BridgeRendererD3D11* s) {
  if (!s)
    return;

  SafeRelease(&s->rtv0);
  SafeRelease(&s->swapChain);
  SafeRelease(&s->dxgiFactory);
  SafeRelease(&s->context);
  SafeRelease(&s->device);
  SetLastError(s, S_OK);
}

bool CreateDeviceAndSwapChain(BridgeRendererD3D11* s) {
  if (!s || s->width <= 0 || s->height <= 0) {
    SetLastError(s, E_INVALIDARG);
    return false;
  }

  if (s->device != nullptr && s->swapChain != nullptr && s->rtv0 != nullptr) {
    SetLastError(s, S_OK);
    return true;
  }

  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 kCreationFlags, nullptr, 0, D3D11_SDK_VERSION,
                                 &s->device, nullptr, &s->context);
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                           kCreationFlags, nullptr, 0, D3D11_SDK_VERSION,
                           &s->device, nullptr, &s->context);
  }

  if (FAILED(hr) || s->device == nullptr || s->context == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    ReleaseRendererResources(s);
    return false;
  }

  if (!EnsureFactory(s)) {
    ReleaseRendererResources(s);
    return false;
  }

  if (!CreateSwapChain(s)) {
    ReleaseRendererResources(s);
    return false;
  }

  if (!CreateRenderTarget(s)) {
    ReleaseRendererResources(s);
    return false;
  }

  SetLastError(s, S_OK);
  return true;
}

bool ResizeSwapChain(BridgeRendererD3D11* s, int width, int height) {
  if (!s || !s->swapChain || !s->device || !s->context) {
    SetLastError(s, E_UNEXPECTED);
    return false;
  }

  if (width <= 0 || height <= 0) {
    SetLastError(s, E_INVALIDARG);
    return false;
  }

  if (s->width == width && s->height == height) {
    SetLastError(s, S_OK);
    return true;
  }

  SafeRelease(&s->rtv0);

  HRESULT hr = s->swapChain->ResizeBuffers(kBufferCount, static_cast<UINT>(width),
                                           static_cast<UINT>(height),
                                           kSwapChainFormat, 0);
  if (FAILED(hr)) {
    SetLastError(s, hr);
    return false;
  }

  s->width = width;
  s->height = height;
  return CreateRenderTarget(s);
}

bool ClearAndPresent(BridgeRendererD3D11* s, float red, float green, float blue,
                     float alpha, int syncInterval) {
  if (!s || !s->context || !s->swapChain || !s->rtv0) {
    SetLastError(s, E_UNEXPECTED);
    return false;
  }

  ID3D11RenderTargetView* currentRtv = s->rtv0;
  float color[4] = {red, green, blue, alpha};

  s->context->OMSetRenderTargets(1, &currentRtv, nullptr);
  s->context->ClearRenderTargetView(currentRtv, color);

  HRESULT hr = s->swapChain->Present(syncInterval < 0 ? 0 : syncInterval, 0);
  if (FAILED(hr)) {
    SetLastError(s, hr);
    return false;
  }

  SetLastError(s, S_OK);
  return true;
}

