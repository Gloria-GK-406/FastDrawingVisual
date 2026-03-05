#include "BridgeRendererInternal.h"
#include "BridgeCommandProtocol.g.h"
#include "../FastDrawingVisual.LogCore/FdvLogCoreExports.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace {
constexpr UINT kBufferCount = 3;
constexpr UINT kCreationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr float kPi = 3.14159265358979323846f;
constexpr int kEllipseSegmentCount = 48;
// Experimental text command: [id][x][y][fontSize][argb][textLen][fontLen][textUtf8][fontUtf8].
constexpr std::uint8_t kCmdDrawText = 7;
constexpr int kDrawTextHeaderBytes = 24;
constexpr int kDrawTextXOffset = 0;
constexpr int kDrawTextYOffset = 4;
constexpr int kDrawTextFontSizeOffset = 8;
constexpr int kDrawTextColorOffset = 12;
constexpr int kDrawTextTextLengthOffset = 16;
constexpr int kDrawTextFontLengthOffset = 20;
constexpr const wchar_t* kLogCategory = L"NativeProxy.D3D11";
constexpr double kSlowFrameThresholdMs = 33.0;
constexpr std::uint64_t kSlowFrameLogEveryNFrames = 120;

std::uint64_t QueryQpcNow() {
  LARGE_INTEGER value{};
  QueryPerformanceCounter(&value);
  return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t QueryQpcFrequency() {
  static const std::uint64_t cached = []() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
  }();
  return cached;
}

void RecordFramePerformance(BridgeRendererD3D11* s, double frameDurationMs) {
  if (!s) {
    return;
  }

  ++s->submittedFrameCount;

  if (s->drawDurationMetricId > 0) {
    FDVLOG_LogMetric(s->drawDurationMetricId, frameDurationMs);
  }

  const std::uint64_t nowQpc = QueryQpcNow();
  if (s->lastPresentQpc != 0 && s->fpsMetricId > 0) {
    const std::uint64_t deltaTicks = nowQpc - s->lastPresentQpc;
    const std::uint64_t freq = QueryQpcFrequency();
    if (deltaTicks > 0 && freq > 0) {
      const double fps =
          static_cast<double>(freq) / static_cast<double>(deltaTicks);
      FDVLOG_LogMetric(s->fpsMetricId, fps);
    }
  }
  s->lastPresentQpc = nowQpc;

  if (frameDurationMs >= kSlowFrameThresholdMs &&
      (s->submittedFrameCount % kSlowFrameLogEveryNFrames) == 0) {
    wchar_t message[320]{};
    swprintf_s(message,
               L"slow frame renderer=0x%p drawMs=%.3f frame=%llu size=%dx%d.",
               static_cast<void*>(s), frameDurationMs,
               static_cast<unsigned long long>(s->submittedFrameCount), s->width,
               s->height);
    FDVLOG_Log(FDVLOG_LevelWarn, kLogCategory, message, false);
    FDVLOG_WriteETW(FDVLOG_LevelWarn, kLogCategory, message, false);
  }
}

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

ColorF ToPremultipliedColor(const fdv::protocol::ColorArgb8& color) {
  const float a = static_cast<float>(color.a) / 255.0f;
  return {
      (static_cast<float>(color.r) / 255.0f) * a,
      (static_cast<float>(color.g) / 255.0f) * a,
      (static_cast<float>(color.b) / 255.0f) * a,
      a,
  };
}

D2D1_COLOR_F ToD2DColor(const fdv::protocol::ColorArgb8& color) {
  return {
      static_cast<float>(color.r) / 255.0f,
      static_cast<float>(color.g) / 255.0f,
      static_cast<float>(color.b) / 255.0f,
      static_cast<float>(color.a) / 255.0f,
  };
}

std::uint32_t ReadU32(const std::uint8_t* p) {
  std::uint32_t value = 0;
  std::memcpy(&value, p, sizeof(std::uint32_t));
  return value;
}

bool Utf8ToWide(const std::uint8_t* bytes, std::uint32_t count,
                std::wstring& out) {
  out.clear();
  if (count == 0)
    return true;

  const int wideCount = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<LPCCH>(bytes),
      static_cast<int>(count), nullptr, 0);
  if (wideCount <= 0)
    return false;

  out.resize(static_cast<std::size_t>(wideCount));
  const int converted = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<LPCCH>(bytes),
      static_cast<int>(count), out.data(), wideCount);
  if (converted <= 0)
    return false;

  return true;
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

void ReleaseRenderTargetResources(BridgeRendererD3D11* s) {
  if (!s)
    return;

  if (s->d2dContext != nullptr)
    s->d2dContext->SetTarget(nullptr);

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
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                     __uuidof(IDWriteFactory),
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

  if (!EnsureD2DAndDWrite(s))
    return false;

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

bool SubmitCommandsAndPresent(BridgeRendererD3D11* s, const void* commands,
                              int commandBytes) {
  const auto frameStart = std::chrono::steady_clock::now();

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

  const auto* cursor = static_cast<const std::uint8_t*>(commands);
  const auto* end = cursor + commandBytes;
  std::vector<ColorVertex> vertices;
  vertices.reserve(6 * 8);

  bool d2dDrawActive = false;
  auto endD2DDraw = [&]() -> bool {
    if (!d2dDrawActive)
      return true;

    HRESULT hr = s->d2dContext->EndDraw();
    d2dDrawActive = false;
    if (FAILED(hr)) {
      SetLastError(s, hr);
      return false;
    }

    return true;
  };

  auto beginD2DDraw = [&]() -> bool {
    if (d2dDrawActive)
      return true;

    if (!s->d2dContext || !s->d2dTargetBitmap || !s->d2dSolidBrush ||
        !s->dwriteFactory) {
      SetLastError(s, E_UNEXPECTED);
      return false;
    }

    s->context->Flush();
    s->d2dContext->BeginDraw();
    d2dDrawActive = true;
    return true;
  };

  while (cursor < end) {
    const std::uint8_t cmd = *cursor++;

    switch (cmd) {
    case fdv::protocol::kCmdClear: {
      if (cursor + fdv::protocol::kClearPayloadBytes > end) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      if (!endD2DDraw())
        return false;

      const auto color = fdv::protocol::ReadColorArgb8(
          cursor + fdv::protocol::kClearColorOffset);
      const ColorF clearPremultiplied = ToPremultipliedColor(color);
      const float clearPremultipliedColor[4] = {clearPremultiplied.r,
                                                clearPremultiplied.g,
                                                clearPremultiplied.b,
                                                clearPremultiplied.a};
      s->context->ClearRenderTargetView(currentRtv, clearPremultipliedColor);
      cursor += fdv::protocol::kClearPayloadBytes;
      break;
    }

    case fdv::protocol::kCmdFillRect: {
      if (cursor + fdv::protocol::kFillRectPayloadBytes > end) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      if (!endD2DDraw())
        return false;

      const float x =
          fdv::protocol::ReadF32(cursor + fdv::protocol::kFillRectXOffset);
      const float y =
          fdv::protocol::ReadF32(cursor + fdv::protocol::kFillRectYOffset);
      const float width =
          fdv::protocol::ReadF32(cursor + fdv::protocol::kFillRectWidthOffset);
      const float height = fdv::protocol::ReadF32(
          cursor + fdv::protocol::kFillRectHeightOffset);
      const auto color = fdv::protocol::ReadColorArgb8(
          cursor + fdv::protocol::kFillRectColorOffset);
      vertices.clear();
      AppendFilledRect(s, vertices, x, y, width, height,
                       ToPremultipliedColor(color));
      if (!DrawTriangleList(s, vertices))
        return false;
      cursor += fdv::protocol::kFillRectPayloadBytes;
      break;
    }

    case fdv::protocol::kCmdStrokeRect: {
      if (cursor + fdv::protocol::kStrokeRectPayloadBytes > end) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      if (!endD2DDraw())
        return false;

      const float x =
          fdv::protocol::ReadF32(cursor + fdv::protocol::kStrokeRectXOffset);
      const float y =
          fdv::protocol::ReadF32(cursor + fdv::protocol::kStrokeRectYOffset);
      const float width = fdv::protocol::ReadF32(
          cursor + fdv::protocol::kStrokeRectWidthOffset);
      const float height = fdv::protocol::ReadF32(
          cursor + fdv::protocol::kStrokeRectHeightOffset);
      const float thickness = fdv::protocol::ReadF32(
          cursor + fdv::protocol::kStrokeRectThicknessOffset);
      const auto color = fdv::protocol::ReadColorArgb8(
          cursor + fdv::protocol::kStrokeRectColorOffset);
      vertices.clear();
      AppendStrokeRect(s, vertices, x, y, width, height, thickness,
                       ToPremultipliedColor(color));
      if (!DrawTriangleList(s, vertices))
        return false;
      cursor += fdv::protocol::kStrokeRectPayloadBytes;
      break;
    }

    case fdv::protocol::kCmdFillEllipse: {
      if (cursor + fdv::protocol::kFillEllipsePayloadBytes > end) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      if (!endD2DDraw())
        return false;

      const float centerX = fdv::protocol::ReadF32(
          cursor + fdv::protocol::kFillEllipseCenterXOffset);
      const float centerY = fdv::protocol::ReadF32(
          cursor + fdv::protocol::kFillEllipseCenterYOffset);
      const float radiusX = fdv::protocol::ReadF32(
          cursor + fdv::protocol::kFillEllipseRadiusXOffset);
      const float radiusY = fdv::protocol::ReadF32(
          cursor + fdv::protocol::kFillEllipseRadiusYOffset);
      const auto color = fdv::protocol::ReadColorArgb8(
          cursor + fdv::protocol::kFillEllipseColorOffset);
      vertices.clear();
      AppendFilledEllipse(s, vertices, centerX, centerY, radiusX, radiusY,
                          ToPremultipliedColor(color));
      if (!DrawTriangleList(s, vertices))
        return false;
      cursor += fdv::protocol::kFillEllipsePayloadBytes;
      break;
    }

    case fdv::protocol::kCmdStrokeEllipse: {
      if (cursor + fdv::protocol::kStrokeEllipsePayloadBytes > end) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      if (!endD2DDraw())
        return false;

      const float centerX = fdv::protocol::ReadF32(
          cursor + fdv::protocol::kStrokeEllipseCenterXOffset);
      const float centerY = fdv::protocol::ReadF32(
          cursor + fdv::protocol::kStrokeEllipseCenterYOffset);
      const float radiusX = fdv::protocol::ReadF32(
          cursor + fdv::protocol::kStrokeEllipseRadiusXOffset);
      const float radiusY = fdv::protocol::ReadF32(
          cursor + fdv::protocol::kStrokeEllipseRadiusYOffset);
      const float thickness = fdv::protocol::ReadF32(
          cursor + fdv::protocol::kStrokeEllipseThicknessOffset);
      const auto color = fdv::protocol::ReadColorArgb8(
          cursor + fdv::protocol::kStrokeEllipseColorOffset);
      vertices.clear();
      AppendStrokeEllipse(s, vertices, centerX, centerY, radiusX, radiusY,
                          thickness, ToPremultipliedColor(color));
      if (!DrawTriangleList(s, vertices))
        return false;
      cursor += fdv::protocol::kStrokeEllipsePayloadBytes;
      break;
    }

    case fdv::protocol::kCmdLine: {
      if (cursor + fdv::protocol::kLinePayloadBytes > end) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      if (!endD2DDraw())
        return false;

      const float x0 =
          fdv::protocol::ReadF32(cursor + fdv::protocol::kLineX0Offset);
      const float y0 =
          fdv::protocol::ReadF32(cursor + fdv::protocol::kLineY0Offset);
      const float x1 =
          fdv::protocol::ReadF32(cursor + fdv::protocol::kLineX1Offset);
      const float y1 =
          fdv::protocol::ReadF32(cursor + fdv::protocol::kLineY1Offset);
      const float thickness =
          fdv::protocol::ReadF32(cursor + fdv::protocol::kLineThicknessOffset);
      const auto color = fdv::protocol::ReadColorArgb8(
          cursor + fdv::protocol::kLineColorOffset);
      vertices.clear();
      AppendLine(s, vertices, x0, y0, x1, y1, thickness,
                 ToPremultipliedColor(color));
      if (!DrawTriangleList(s, vertices))
        return false;
      cursor += fdv::protocol::kLinePayloadBytes;
      break;
    }

    case kCmdDrawText: {
      if (cursor + kDrawTextHeaderBytes > end) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      const float x = fdv::protocol::ReadF32(cursor + kDrawTextXOffset);
      const float y = fdv::protocol::ReadF32(cursor + kDrawTextYOffset);
      const float fontSize =
          std::max(1.0f, fdv::protocol::ReadF32(cursor + kDrawTextFontSizeOffset));
      const auto color =
          fdv::protocol::ReadColorArgb8(cursor + kDrawTextColorOffset);
      const std::uint32_t textLength =
          ReadU32(cursor + kDrawTextTextLengthOffset);
      const std::uint32_t fontLength =
          ReadU32(cursor + kDrawTextFontLengthOffset);

      const std::uint64_t trailingBytes = static_cast<std::uint64_t>(textLength) +
                                          static_cast<std::uint64_t>(fontLength);
      const std::uint64_t availableBytes =
          static_cast<std::uint64_t>(end - cursor - kDrawTextHeaderBytes);
      if (trailingBytes > availableBytes) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      const auto* textUtf8 = cursor + kDrawTextHeaderBytes;
      const auto* fontUtf8 = textUtf8 + textLength;
      cursor += kDrawTextHeaderBytes + static_cast<std::ptrdiff_t>(trailingBytes);

      if (textLength == 0)
        break;

      std::wstring textWide;
      if (!Utf8ToWide(textUtf8, textLength, textWide) || textWide.empty()) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      std::wstring fontWide;
      if (!Utf8ToWide(fontUtf8, fontLength, fontWide)) {
        SetLastError(s, E_INVALIDARG);
        return false;
      }

      if (fontWide.empty())
        fontWide = L"Segoe UI";

      if (!beginD2DDraw())
        return false;

      IDWriteTextFormat* textFormat = nullptr;
      HRESULT hr = s->dwriteFactory->CreateTextFormat(
          fontWide.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize,
          L"en-us", &textFormat);
      if (FAILED(hr) || textFormat == nullptr) {
        SetLastError(s, FAILED(hr) ? hr : E_FAIL);
        SafeRelease(&textFormat);
        return false;
      }

      textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
      textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

      D2D1_RECT_F layoutRect = {x, y, static_cast<float>(s->width),
                                static_cast<float>(s->height)};
      if (layoutRect.right <= layoutRect.left)
        layoutRect.right = layoutRect.left + 1.0f;
      if (layoutRect.bottom <= layoutRect.top)
        layoutRect.bottom = layoutRect.top + 1.0f;

      s->d2dSolidBrush->SetColor(ToD2DColor(color));
      s->d2dContext->DrawText(
          textWide.c_str(), static_cast<UINT32>(textWide.size()), textFormat,
          &layoutRect, s->d2dSolidBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP,
          DWRITE_MEASURING_MODE_NATURAL);
      textFormat->Release();
      break;
    }

    default:
      SetLastError(s, E_INVALIDARG);
      return false;
    }
  }

  if (!endD2DDraw())
    return false;

  HRESULT hr = s->swapChain->Present(1, 0);
  if (FAILED(hr)) {
    SetLastError(s, hr);
    return false;
  }

  const auto frameEnd = std::chrono::steady_clock::now();
  const double frameDurationMs =
      std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
  RecordFramePerformance(s, frameDurationMs);

  SetLastError(s, S_OK);
  return true;
}
