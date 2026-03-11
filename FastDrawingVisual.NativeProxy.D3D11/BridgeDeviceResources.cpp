#include "BridgeRendererInternal.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace {
constexpr UINT kBufferCount = 3;
constexpr UINT kCreationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

bool CompileShader(const char* source, const char* target, ID3DBlob** blobOut,
                   HRESULT* hrOut) {
  if (!source || !target || !blobOut) {
    return false;
  }

  *blobOut = nullptr;
  ID3DBlob* errorBlob = nullptr;
  const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr,
                                nullptr, "main", target, 0, 0, blobOut,
                                &errorBlob);
  SafeRelease(&errorBlob);

  if (hrOut != nullptr) {
    *hrOut = hr;
  }

  return SUCCEEDED(hr) && *blobOut != nullptr;
}

void ReleaseRenderTargetResources(BridgeRendererD3D11* s) {
  if (!s) {
    return;
  }

  if (s->d2dContext != nullptr) {
    s->d2dContext->SetTarget(nullptr);
  }

  SafeRelease(&s->d2dSolidBrush);
  SafeRelease(&s->d2dTargetBitmap);
  SafeRelease(&s->rtv0);
}

bool EnsureD2DAndDWrite(BridgeRendererD3D11* s) {
  if (!s || !s->device) {
    SetLastError(s, E_UNEXPECTED);
    return false;
  }

  if (s->d2dFactory != nullptr && s->d2dDevice != nullptr &&
      s->d2dContext != nullptr && s->dwriteFactory != nullptr) {
    SetLastError(s, S_OK);
    return true;
  }

  if (s->d2dFactory == nullptr) {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   __uuidof(ID2D1Factory1), nullptr,
                                   reinterpret_cast<void**>(&s->d2dFactory));
    if (FAILED(hr) || s->d2dFactory == nullptr) {
      SetLastError(s, FAILED(hr) ? hr : E_FAIL);
      return false;
    }
  }

  if (s->dwriteFactory == nullptr) {
    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&s->dwriteFactory));
    if (FAILED(hr) || s->dwriteFactory == nullptr) {
      SetLastError(s, FAILED(hr) ? hr : E_FAIL);
      return false;
    }
  }

  if (s->d2dDevice == nullptr || s->d2dContext == nullptr) {
    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = s->device->QueryInterface(__uuidof(IDXGIDevice),
                                           reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || dxgiDevice == nullptr) {
      SetLastError(s, FAILED(hr) ? hr : E_FAIL);
      return false;
    }

    if (s->d2dDevice == nullptr) {
      hr = s->d2dFactory->CreateDevice(dxgiDevice, &s->d2dDevice);
      if (FAILED(hr) || s->d2dDevice == nullptr) {
        dxgiDevice->Release();
        SetLastError(s, FAILED(hr) ? hr : E_FAIL);
        return false;
      }
    }

    if (s->d2dContext == nullptr) {
      hr = s->d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                             &s->d2dContext);
      if (FAILED(hr) || s->d2dContext == nullptr) {
        dxgiDevice->Release();
        SetLastError(s, FAILED(hr) ? hr : E_FAIL);
        return false;
      }
    }

    dxgiDevice->Release();
  }

  SetLastError(s, S_OK);
  return true;
}

bool CreateRenderTarget(BridgeRendererD3D11* s) {
  if (!s || !s->device || !s->swapChain) {
    SetLastError(s, E_UNEXPECTED);
    return false;
  }

  if (!EnsureD2DAndDWrite(s)) {
    return false;
  }

  ReleaseRenderTargetResources(s);

  ID3D11Texture2D* backBuffer = nullptr;
  HRESULT hr = s->swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void**>(&backBuffer));
  if (FAILED(hr) || backBuffer == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  hr = s->device->CreateRenderTargetView(backBuffer, nullptr, &s->rtv0);
  if (FAILED(hr) || s->rtv0 == nullptr) {
    backBuffer->Release();
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  IDXGISurface* dxgiSurface = nullptr;
  hr = backBuffer->QueryInterface(__uuidof(IDXGISurface),
                                  reinterpret_cast<void**>(&dxgiSurface));
  backBuffer->Release();
  if (FAILED(hr) || dxgiSurface == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  D2D1_BITMAP_PROPERTIES1 bitmapProps = {};
  bitmapProps.pixelFormat.format = kSwapChainFormat;
  bitmapProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
  bitmapProps.dpiX = 96.0f;
  bitmapProps.dpiY = 96.0f;
  bitmapProps.bitmapOptions =
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

  hr = s->d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface, &bitmapProps,
                                                  &s->d2dTargetBitmap);
  dxgiSurface->Release();
  if (FAILED(hr) || s->d2dTargetBitmap == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  s->d2dContext->SetTarget(s->d2dTargetBitmap);
  s->d2dContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

  D2D1_COLOR_F white = {1.0f, 1.0f, 1.0f, 1.0f};
  hr = s->d2dContext->CreateSolidColorBrush(white, &s->d2dSolidBrush);
  if (FAILED(hr) || s->d2dSolidBrush == nullptr) {
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

  if (s->dxgiFactory != nullptr) {
    SetLastError(s, S_OK);
    return true;
  }

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
  swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
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

bool CreateDrawPipeline(BridgeRendererD3D11* s) {
  if (!s || !s->device) {
    SetLastError(s, E_UNEXPECTED);
    return false;
  }

  if (s->vertexShader != nullptr && s->pixelShader != nullptr &&
      s->inputLayout != nullptr && s->blendState != nullptr &&
      s->rasterizerState != nullptr) {
    SetLastError(s, S_OK);
    return true;
  }

  static const char* kVertexShaderSrc = R"(
struct VSInput
{
    float3 pos : POSITION;
    float4 color : COLOR;
};

struct PSInput
{
    float4 pos : SV_Position;
    float4 color : COLOR;
};

PSInput main(VSInput input)
{
    PSInput output;
    output.pos = float4(input.pos, 1.0f);
    output.color = input.color;
    return output;
}
)";

  static const char* kPixelShaderSrc = R"(
struct PSInput
{
    float4 pos : SV_Position;
    float4 color : COLOR;
};

float4 main(PSInput input) : SV_Target
{
    return input.color;
}
)";

  ID3DBlob* vsBlob = nullptr;
  ID3DBlob* psBlob = nullptr;
  HRESULT hr = S_OK;

  if (!CompileShader(kVertexShaderSrc, "vs_4_0_level_9_1", &vsBlob, &hr)) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  if (!CompileShader(kPixelShaderSrc, "ps_4_0_level_9_1", &psBlob, &hr)) {
    SafeRelease(&vsBlob);
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  hr = s->device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                     vsBlob->GetBufferSize(), nullptr,
                                     &s->vertexShader);
  if (FAILED(hr) || s->vertexShader == nullptr) {
    SafeRelease(&psBlob);
    SafeRelease(&vsBlob);
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  hr = s->device->CreatePixelShader(psBlob->GetBufferPointer(),
                                    psBlob->GetBufferSize(), nullptr,
                                    &s->pixelShader);
  if (FAILED(hr) || s->pixelShader == nullptr) {
    SafeRelease(&psBlob);
    SafeRelease(&vsBlob);
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  D3D11_INPUT_ELEMENT_DESC inputLayout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  hr = s->device->CreateInputLayout(
      inputLayout, ARRAYSIZE(inputLayout), vsBlob->GetBufferPointer(),
      vsBlob->GetBufferSize(), &s->inputLayout);
  SafeRelease(&psBlob);
  SafeRelease(&vsBlob);
  if (FAILED(hr) || s->inputLayout == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  D3D11_BLEND_DESC blendDesc = {};
  blendDesc.AlphaToCoverageEnable = FALSE;
  blendDesc.IndependentBlendEnable = FALSE;
  blendDesc.RenderTarget[0].BlendEnable = TRUE;
  blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blendDesc.RenderTarget[0].RenderTargetWriteMask =
      D3D11_COLOR_WRITE_ENABLE_ALL;

  hr = s->device->CreateBlendState(&blendDesc, &s->blendState);
  if (FAILED(hr) || s->blendState == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  D3D11_RASTERIZER_DESC rsDesc = {};
  rsDesc.FillMode = D3D11_FILL_SOLID;
  rsDesc.CullMode = D3D11_CULL_NONE;
  rsDesc.FrontCounterClockwise = FALSE;
  rsDesc.DepthBias = 0;
  rsDesc.DepthBiasClamp = 0.0f;
  rsDesc.SlopeScaledDepthBias = 0.0f;
  rsDesc.DepthClipEnable = FALSE;
  rsDesc.ScissorEnable = FALSE;
  rsDesc.MultisampleEnable = FALSE;
  rsDesc.AntialiasedLineEnable = FALSE;

  hr = s->device->CreateRasterizerState(&rsDesc, &s->rasterizerState);
  if (FAILED(hr) || s->rasterizerState == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  SetLastError(s, S_OK);
  return true;
}

bool EnsureDynamicVertexBuffer(BridgeRendererD3D11* s, UINT requiredBytes) {
  if (!s || !s->device) {
    SetLastError(s, E_UNEXPECTED);
    return false;
  }

  const UINT minBytes = std::max(requiredBytes, 1024u);
  if (s->dynamicVertexBuffer != nullptr &&
      s->dynamicVertexCapacityBytes >= minBytes) {
    SetLastError(s, S_OK);
    return true;
  }

  SafeRelease(&s->dynamicVertexBuffer);
  s->dynamicVertexCapacityBytes = 0;

  D3D11_BUFFER_DESC bufferDesc = {};
  bufferDesc.ByteWidth = minBytes;
  bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
  bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bufferDesc.MiscFlags = 0;
  bufferDesc.StructureByteStride = 0;

  HRESULT hr =
      s->device->CreateBuffer(&bufferDesc, nullptr, &s->dynamicVertexBuffer);
  if (FAILED(hr) || s->dynamicVertexBuffer == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  s->dynamicVertexCapacityBytes = minBytes;
  SetLastError(s, S_OK);
  return true;
}

void ReleaseRendererResources(BridgeRendererD3D11* s) {
  if (!s) {
    return;
  }

  for (auto& entry : s->textFormats) {
    SafeRelease(&entry.format);
  }
  s->textFormats.clear();
  s->textFormatUseTick = 0;

  ReleaseRenderTargetResources(s);
  SafeRelease(&s->dynamicVertexBuffer);
  s->dynamicVertexCapacityBytes = 0;
  SafeRelease(&s->rasterizerState);
  SafeRelease(&s->blendState);
  SafeRelease(&s->inputLayout);
  SafeRelease(&s->pixelShader);
  SafeRelease(&s->vertexShader);
  SafeRelease(&s->swapChain);
  SafeRelease(&s->dxgiFactory);
  SafeRelease(&s->d2dContext);
  SafeRelease(&s->d2dDevice);
  SafeRelease(&s->d2dFactory);
  SafeRelease(&s->dwriteFactory);
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

  if (!CreateDrawPipeline(s)) {
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

  ReleaseRenderTargetResources(s);

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
