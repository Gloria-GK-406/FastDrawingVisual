#include "BridgeRendererInternal.h"
#include "BridgeCommandProtocol.g.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace {
constexpr UINT kBufferCount = 3;
constexpr UINT kCreationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr float kPi = 3.14159265358979323846f;
constexpr int kEllipseSegmentCount = 48;

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

struct ColorF {
  float r;
  float g;
  float b;
  float a;
};

struct ColorVertex {
  float x;
  float y;
  float z;
  float r;
  float g;
  float b;
  float a;
};

float ReadF32(const uint8_t* p) {
  float value = 0.0f;
  std::memcpy(&value, p, sizeof(float));
  return value;
}

ColorF ReadPremultipliedColor(const uint8_t* p) {
  const float a = static_cast<float>(p[0]) / 255.0f;
  return {
      (static_cast<float>(p[1]) / 255.0f) * a,
      (static_cast<float>(p[2]) / 255.0f) * a,
      (static_cast<float>(p[3]) / 255.0f) * a,
      a,
  };
}

float ToNdcX(const BridgeRendererD3D11* s, float x) {
  return (x / static_cast<float>(s->width)) * 2.0f - 1.0f;
}

float ToNdcY(const BridgeRendererD3D11* s, float y) {
  return 1.0f - (y / static_cast<float>(s->height)) * 2.0f;
}

ColorVertex MakeVertex(const BridgeRendererD3D11* s, float x, float y,
                       const ColorF& color) {
  return {ToNdcX(s, x), ToNdcY(s, y), 0.0f, color.r, color.g, color.b,
          color.a};
}

void AppendFilledRect(const BridgeRendererD3D11* s, std::vector<ColorVertex>& out,
                      float x, float y, float w, float h,
                      const ColorF& color) {
  if (w <= 0.0f || h <= 0.0f)
    return;

  const float x0 = x;
  const float y0 = y;
  const float x1 = x + w;
  const float y1 = y + h;

  out.push_back(MakeVertex(s, x0, y0, color));
  out.push_back(MakeVertex(s, x1, y0, color));
  out.push_back(MakeVertex(s, x0, y1, color));

  out.push_back(MakeVertex(s, x1, y0, color));
  out.push_back(MakeVertex(s, x1, y1, color));
  out.push_back(MakeVertex(s, x0, y1, color));
}

void AppendStrokeRect(const BridgeRendererD3D11* s,
                      std::vector<ColorVertex>& out, float x, float y, float w,
                      float h, float thickness, const ColorF& color) {
  if (w <= 0.0f || h <= 0.0f)
    return;

  const float t = std::max(1.0f, thickness);
  if (t * 2.0f >= w || t * 2.0f >= h) {
    AppendFilledRect(s, out, x, y, w, h, color);
    return;
  }

  AppendFilledRect(s, out, x, y, w, t, color);
  AppendFilledRect(s, out, x, y + h - t, w, t, color);
  AppendFilledRect(s, out, x, y + t, t, h - 2.0f * t, color);
  AppendFilledRect(s, out, x + w - t, y + t, t, h - 2.0f * t, color);
}

void AppendFilledEllipse(const BridgeRendererD3D11* s,
                         std::vector<ColorVertex>& out, float cx, float cy,
                         float rx, float ry, const ColorF& color) {
  if (rx <= 0.0f || ry <= 0.0f)
    return;

  const ColorVertex center = MakeVertex(s, cx, cy, color);
  for (int i = 0; i < kEllipseSegmentCount; ++i) {
    const float a0 = (2.0f * kPi * static_cast<float>(i)) /
                     static_cast<float>(kEllipseSegmentCount);
    const float a1 = (2.0f * kPi * static_cast<float>(i + 1)) /
                     static_cast<float>(kEllipseSegmentCount);

    const float x0 = cx + std::cos(a0) * rx;
    const float y0 = cy + std::sin(a0) * ry;
    const float x1 = cx + std::cos(a1) * rx;
    const float y1 = cy + std::sin(a1) * ry;

    out.push_back(center);
    out.push_back(MakeVertex(s, x0, y0, color));
    out.push_back(MakeVertex(s, x1, y1, color));
  }
}

void AppendStrokeEllipse(const BridgeRendererD3D11* s,
                         std::vector<ColorVertex>& out, float cx, float cy,
                         float rx, float ry, float thickness,
                         const ColorF& color) {
  if (rx <= 0.0f || ry <= 0.0f)
    return;

  const float t = std::max(1.0f, thickness);
  const float outerRx = rx + t * 0.5f;
  const float outerRy = ry + t * 0.5f;
  const float innerRx = rx - t * 0.5f;
  const float innerRy = ry - t * 0.5f;

  if (innerRx <= 0.0f || innerRy <= 0.0f) {
    AppendFilledEllipse(s, out, cx, cy, outerRx, outerRy, color);
    return;
  }

  for (int i = 0; i < kEllipseSegmentCount; ++i) {
    const float a0 = (2.0f * kPi * static_cast<float>(i)) /
                     static_cast<float>(kEllipseSegmentCount);
    const float a1 = (2.0f * kPi * static_cast<float>(i + 1)) /
                     static_cast<float>(kEllipseSegmentCount);

    const float ox0 = cx + std::cos(a0) * outerRx;
    const float oy0 = cy + std::sin(a0) * outerRy;
    const float ox1 = cx + std::cos(a1) * outerRx;
    const float oy1 = cy + std::sin(a1) * outerRy;

    const float ix0 = cx + std::cos(a0) * innerRx;
    const float iy0 = cy + std::sin(a0) * innerRy;
    const float ix1 = cx + std::cos(a1) * innerRx;
    const float iy1 = cy + std::sin(a1) * innerRy;

    out.push_back(MakeVertex(s, ox0, oy0, color));
    out.push_back(MakeVertex(s, ox1, oy1, color));
    out.push_back(MakeVertex(s, ix0, iy0, color));

    out.push_back(MakeVertex(s, ox1, oy1, color));
    out.push_back(MakeVertex(s, ix1, iy1, color));
    out.push_back(MakeVertex(s, ix0, iy0, color));
  }
}

void AppendLine(const BridgeRendererD3D11* s, std::vector<ColorVertex>& out,
                float x0, float y0, float x1, float y1, float thickness,
                const ColorF& color) {
  const float t = std::max(1.0f, thickness);
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const float len = std::sqrt(dx * dx + dy * dy);

  if (len < 0.0001f) {
    const float half = t * 0.5f;
    AppendFilledRect(s, out, x0 - half, y0 - half, t, t, color);
    return;
  }

  const float half = t * 0.5f;
  const float nx = -dy / len * half;
  const float ny = dx / len * half;

  const ColorVertex v0 = MakeVertex(s, x0 + nx, y0 + ny, color);
  const ColorVertex v1 = MakeVertex(s, x1 + nx, y1 + ny, color);
  const ColorVertex v2 = MakeVertex(s, x1 - nx, y1 - ny, color);
  const ColorVertex v3 = MakeVertex(s, x0 - nx, y0 - ny, color);

  out.push_back(v0);
  out.push_back(v1);
  out.push_back(v2);

  out.push_back(v0);
  out.push_back(v2);
  out.push_back(v3);
}

bool CompileShader(const char* source, const char* target, ID3DBlob** blobOut,
                   HRESULT* hrOut) {
  if (!source || !target || !blobOut)
    return false;

  *blobOut = nullptr;
  ID3DBlob* errorBlob = nullptr;
  const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr,
                                nullptr, "main", target, 0, 0, blobOut,
                                &errorBlob);
  if (errorBlob != nullptr)
    errorBlob->Release();

  if (hrOut != nullptr)
    *hrOut = hr;

  return SUCCEEDED(hr) && *blobOut != nullptr;
}

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
    vsBlob->Release();
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  hr = s->device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                     vsBlob->GetBufferSize(), nullptr,
                                     &s->vertexShader);
  if (FAILED(hr) || s->vertexShader == nullptr) {
    psBlob->Release();
    vsBlob->Release();
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  hr = s->device->CreatePixelShader(psBlob->GetBufferPointer(),
                                    psBlob->GetBufferSize(), nullptr,
                                    &s->pixelShader);
  if (FAILED(hr) || s->pixelShader == nullptr) {
    psBlob->Release();
    vsBlob->Release();
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
  psBlob->Release();
  vsBlob->Release();
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
  blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

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

  HRESULT hr = s->device->CreateBuffer(&bufferDesc, nullptr,
                                       &s->dynamicVertexBuffer);
  if (FAILED(hr) || s->dynamicVertexBuffer == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  s->dynamicVertexCapacityBytes = minBytes;
  SetLastError(s, S_OK);
  return true;
}

bool DrawTriangleList(BridgeRendererD3D11* s, const std::vector<ColorVertex>& v) {
  if (!s || !s->context) {
    SetLastError(s, E_UNEXPECTED);
    return false;
  }

  if (v.empty())
    return true;

  const UINT byteSize = static_cast<UINT>(v.size() * sizeof(ColorVertex));
  if (!EnsureDynamicVertexBuffer(s, byteSize))
    return false;

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  HRESULT hr = s->context->Map(s->dynamicVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD,
                               0, &mapped);
  if (FAILED(hr) || mapped.pData == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    return false;
  }

  std::memcpy(mapped.pData, v.data(), byteSize);
  s->context->Unmap(s->dynamicVertexBuffer, 0);

  UINT stride = sizeof(ColorVertex);
  UINT offset = 0;
  ID3D11Buffer* vb = s->dynamicVertexBuffer;
  s->context->IASetInputLayout(s->inputLayout);
  s->context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  s->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  s->context->VSSetShader(s->vertexShader, nullptr, 0);
  s->context->PSSetShader(s->pixelShader, nullptr, 0);

  const float blendFactor[4] = {0, 0, 0, 0};
  s->context->OMSetBlendState(s->blendState, blendFactor, 0xFFFFFFFF);
  s->context->RSSetState(s->rasterizerState);
  s->context->Draw(static_cast<UINT>(v.size()), 0);
  return true;
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

  SafeRelease(&s->dynamicVertexBuffer);
  s->dynamicVertexCapacityBytes = 0;
  SafeRelease(&s->rasterizerState);
  SafeRelease(&s->blendState);
  SafeRelease(&s->inputLayout);
  SafeRelease(&s->pixelShader);
  SafeRelease(&s->vertexShader);
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

bool SubmitCommandsAndPresent(BridgeRendererD3D11* s, const void* commands,
                              int commandBytes) {
  if (!s || !s->context || !s->swapChain || !s->rtv0 || !commands ||
      commandBytes <= 0 || s->width <= 0 || s->height <= 0) {
    SetLastError(s, E_UNEXPECTED);
    return false;
  }

  if (!CreateDrawPipeline(s))
    return false;

  ID3D11RenderTargetView* currentRtv = s->rtv0;
  s->context->OMSetRenderTargets(1, &currentRtv, nullptr);

  D3D11_VIEWPORT viewport = {};
  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = 0.0f;
  viewport.Width = static_cast<float>(s->width);
  viewport.Height = static_cast<float>(s->height);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  s->context->RSSetViewports(1, &viewport);

  const auto* p = static_cast<const uint8_t*>(commands);
  const auto* end = p + commandBytes;
  std::vector<ColorVertex> vertices;
  vertices.reserve(6 * 8);

  while (p < end) {
    const uint8_t cmd = *p++;

    switch (cmd) {
    case fdv::protocol::kCmdClear: {
      if (end - p < fdv::protocol::kClearPayloadBytes) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      const ColorF color =
          ReadPremultipliedColor(p + fdv::protocol::kClearColorOffset);
      p += fdv::protocol::kClearPayloadBytes;
      const float clearColor[4] = {color.r, color.g, color.b, color.a};
      s->context->ClearRenderTargetView(currentRtv, clearColor);
      break;
    }

    case fdv::protocol::kCmdFillRect: {
      if (end - p < fdv::protocol::kFillRectPayloadBytes) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      const float x = ReadF32(p + fdv::protocol::kFillRectXOffset);
      const float y = ReadF32(p + fdv::protocol::kFillRectYOffset);
      const float w = ReadF32(p + fdv::protocol::kFillRectWidthOffset);
      const float h = ReadF32(p + fdv::protocol::kFillRectHeightOffset);
      const ColorF color =
          ReadPremultipliedColor(p + fdv::protocol::kFillRectColorOffset);
      p += fdv::protocol::kFillRectPayloadBytes;

      vertices.clear();
      AppendFilledRect(s, vertices, x, y, w, h, color);
      if (!DrawTriangleList(s, vertices))
        return false;
      break;
    }

    case fdv::protocol::kCmdStrokeRect: {
      if (end - p < fdv::protocol::kStrokeRectPayloadBytes) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      const float x = ReadF32(p + fdv::protocol::kStrokeRectXOffset);
      const float y = ReadF32(p + fdv::protocol::kStrokeRectYOffset);
      const float w = ReadF32(p + fdv::protocol::kStrokeRectWidthOffset);
      const float h = ReadF32(p + fdv::protocol::kStrokeRectHeightOffset);
      const float t = ReadF32(p + fdv::protocol::kStrokeRectThicknessOffset);
      const ColorF color =
          ReadPremultipliedColor(p + fdv::protocol::kStrokeRectColorOffset);
      p += fdv::protocol::kStrokeRectPayloadBytes;

      vertices.clear();
      AppendStrokeRect(s, vertices, x, y, w, h, t, color);
      if (!DrawTriangleList(s, vertices))
        return false;
      break;
    }

    case fdv::protocol::kCmdFillEllipse: {
      if (end - p < fdv::protocol::kFillEllipsePayloadBytes) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      const float cx = ReadF32(p + fdv::protocol::kFillEllipseCenterXOffset);
      const float cy = ReadF32(p + fdv::protocol::kFillEllipseCenterYOffset);
      const float rx = ReadF32(p + fdv::protocol::kFillEllipseRadiusXOffset);
      const float ry = ReadF32(p + fdv::protocol::kFillEllipseRadiusYOffset);
      const ColorF color =
          ReadPremultipliedColor(p + fdv::protocol::kFillEllipseColorOffset);
      p += fdv::protocol::kFillEllipsePayloadBytes;

      vertices.clear();
      AppendFilledEllipse(s, vertices, cx, cy, rx, ry, color);
      if (!DrawTriangleList(s, vertices))
        return false;
      break;
    }

    case fdv::protocol::kCmdStrokeEllipse: {
      if (end - p < fdv::protocol::kStrokeEllipsePayloadBytes) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      const float cx = ReadF32(p + fdv::protocol::kStrokeEllipseCenterXOffset);
      const float cy = ReadF32(p + fdv::protocol::kStrokeEllipseCenterYOffset);
      const float rx = ReadF32(p + fdv::protocol::kStrokeEllipseRadiusXOffset);
      const float ry = ReadF32(p + fdv::protocol::kStrokeEllipseRadiusYOffset);
      const float t = ReadF32(p + fdv::protocol::kStrokeEllipseThicknessOffset);
      const ColorF color =
          ReadPremultipliedColor(p + fdv::protocol::kStrokeEllipseColorOffset);
      p += fdv::protocol::kStrokeEllipsePayloadBytes;

      vertices.clear();
      AppendStrokeEllipse(s, vertices, cx, cy, rx, ry, t, color);
      if (!DrawTriangleList(s, vertices))
        return false;
      break;
    }

    case fdv::protocol::kCmdLine: {
      if (end - p < fdv::protocol::kLinePayloadBytes) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      const float x0 = ReadF32(p + fdv::protocol::kLineX0Offset);
      const float y0 = ReadF32(p + fdv::protocol::kLineY0Offset);
      const float x1 = ReadF32(p + fdv::protocol::kLineX1Offset);
      const float y1 = ReadF32(p + fdv::protocol::kLineY1Offset);
      const float t = ReadF32(p + fdv::protocol::kLineThicknessOffset);
      const ColorF color =
          ReadPremultipliedColor(p + fdv::protocol::kLineColorOffset);
      p += fdv::protocol::kLinePayloadBytes;

      vertices.clear();
      AppendLine(s, vertices, x0, y0, x1, y1, t, color);
      if (!DrawTriangleList(s, vertices))
        return false;
      break;
    }

    default:
      SetLastError(s, E_INVALIDARG);
      return false;
    }
  }

  HRESULT hr = s->swapChain->Present(1, 0);
  if (FAILED(hr)) {
    SetLastError(s, hr);
    return false;
  }

  SetLastError(s, S_OK);
  return true;
}
