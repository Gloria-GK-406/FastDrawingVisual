#include "BridgeRendererInternal.h"
#include "BridgeCommandProtocol.g.h"
#include "../FastDrawingVisual.LogCore/FdvLogCoreExports.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <vector>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr int kEllipseSegmentCount = 48;
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

void RecordFramePerformance(BridgeRendererD3D11* s, double drawDurationMs) {
  if (!s) {
    return;
  }

  ++s->submittedFrameCount;

  if (s->drawDurationMetricId > 0) {
    FDVLOG_LogMetric(s->drawDurationMetricId, drawDurationMs);
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

  if (drawDurationMs >= kSlowFrameThresholdMs &&
      (s->submittedFrameCount % kSlowFrameLogEveryNFrames) == 0) {
    wchar_t message[320]{};
    swprintf_s(message,
               L"slow frame renderer=0x%p drawMs=%.3f frame=%llu size=%dx%d.",
               static_cast<void*>(s), drawDurationMs,
               static_cast<unsigned long long>(s->submittedFrameCount), s->width,
               s->height);
    FDVLOG_Log(FDVLOG_LevelWarn, kLogCategory, message, false);
    FDVLOG_WriteETW(FDVLOG_LevelWarn, kLogCategory, message, false);
  }
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
  if (w <= 0.0f || h <= 0.0f) {
    return;
  }

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
  if (w <= 0.0f || h <= 0.0f) {
    return;
  }

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
  if (rx <= 0.0f || ry <= 0.0f) {
    return;
  }

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
  if (rx <= 0.0f || ry <= 0.0f) {
    return;
  }

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

bool DrawTriangleList(BridgeRendererD3D11* s, const std::vector<ColorVertex>& v) {
  if (!s || !s->context) {
    SetLastError(s, E_UNEXPECTED);
    return false;
  }

  if (v.empty()) {
    return true;
  }

  const UINT byteSize = static_cast<UINT>(v.size() * sizeof(ColorVertex));
  if (!EnsureDynamicVertexBuffer(s, byteSize)) {
    return false;
  }

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
} // namespace

bool SubmitCommandsAndPresent(BridgeRendererD3D11* s, const void* commands,
                              int commandBytes, const void* blobs,
                              int blobBytes) {
  const auto drawStart = std::chrono::steady_clock::now();

  if (!s || !s->context || !s->swapChain || !s->rtv0 || !commands ||
      commandBytes <= 0 || s->width <= 0 || s->height <= 0) {
    SetLastError(s, E_UNEXPECTED);
    return false;
  }

  if (!CreateDrawPipeline(s)) {
    return false;
  }

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

  std::vector<ColorVertex> vertices;
  vertices.reserve(6 * 8);

  fdv::protocol::CommandReader reader(commands, commandBytes, blobs, blobBytes);
  fdv::protocol::Command command{};
  bool d2dDrawActive = false;
  while (reader.TryReadNext(command)) {
    switch (command.type) {
    case fdv::protocol::CommandType::Clear: {
      if (!EndD2DDraw(s, d2dDrawActive)) {
        return false;
      }

      const auto& payload =
          std::get<fdv::protocol::ClearPayload>(command.payload);
      const auto color = payload.color;
      const ColorF clearPremultiplied = ToPremultipliedColor(color);
      const float clearPremultipliedColor[4] = {clearPremultiplied.r,
                                                clearPremultiplied.g,
                                                clearPremultiplied.b,
                                                clearPremultiplied.a};
      s->context->ClearRenderTargetView(currentRtv, clearPremultipliedColor);
      break;
    }

    case fdv::protocol::CommandType::FillRect: {
      if (!EndD2DDraw(s, d2dDrawActive)) {
        return false;
      }

      const auto& payload =
          std::get<fdv::protocol::FillRectPayload>(command.payload);
      vertices.clear();
      AppendFilledRect(s, vertices, payload.x, payload.y, payload.width,
                       payload.height, ToPremultipliedColor(payload.color));
      if (!DrawTriangleList(s, vertices)) {
        return false;
      }
      break;
    }

    case fdv::protocol::CommandType::StrokeRect: {
      if (!EndD2DDraw(s, d2dDrawActive)) {
        return false;
      }

      const auto& payload =
          std::get<fdv::protocol::StrokeRectPayload>(command.payload);
      vertices.clear();
      AppendStrokeRect(s, vertices, payload.x, payload.y, payload.width,
                       payload.height, payload.thickness,
                       ToPremultipliedColor(payload.color));
      if (!DrawTriangleList(s, vertices)) {
        return false;
      }
      break;
    }

    case fdv::protocol::CommandType::FillEllipse: {
      if (!EndD2DDraw(s, d2dDrawActive)) {
        return false;
      }

      const auto& payload =
          std::get<fdv::protocol::FillEllipsePayload>(command.payload);
      vertices.clear();
      AppendFilledEllipse(s, vertices, payload.centerX, payload.centerY,
                          payload.radiusX, payload.radiusY,
                          ToPremultipliedColor(payload.color));
      if (!DrawTriangleList(s, vertices)) {
        return false;
      }
      break;
    }

    case fdv::protocol::CommandType::StrokeEllipse: {
      if (!EndD2DDraw(s, d2dDrawActive)) {
        return false;
      }

      const auto& payload =
          std::get<fdv::protocol::StrokeEllipsePayload>(command.payload);
      vertices.clear();
      AppendStrokeEllipse(s, vertices, payload.centerX, payload.centerY,
                          payload.radiusX, payload.radiusY, payload.thickness,
                          ToPremultipliedColor(payload.color));
      if (!DrawTriangleList(s, vertices)) {
        return false;
      }
      break;
    }

    case fdv::protocol::CommandType::Line: {
      if (!EndD2DDraw(s, d2dDrawActive)) {
        return false;
      }

      const auto& payload =
          std::get<fdv::protocol::LinePayload>(command.payload);
      vertices.clear();
      AppendLine(s, vertices, payload.x0, payload.y0, payload.x1, payload.y1,
                 payload.thickness, ToPremultipliedColor(payload.color));
      if (!DrawTriangleList(s, vertices)) {
        return false;
      }
      break;
    }

    case fdv::protocol::CommandType::DrawText: {
      const auto& payload =
          std::get<fdv::protocol::DrawTextPayload>(command.payload);
      if (!ExecuteDrawTextCommand(s, payload, reader, d2dDrawActive)) {
        return false;
      }
      break;
    }

    default:
      SetLastError(s, E_INVALIDARG);
      return false;
    }
  }

  if (reader.HasError()) {
    SetLastError(s, E_INVALIDARG);
    return false;
  }

  if (!EndD2DDraw(s, d2dDrawActive)) {
    return false;
  }

  HRESULT hr = s->swapChain->Present(0, 0);

  const auto drawEnd = std::chrono::steady_clock::now();
  const double drawDurationMs =
      std::chrono::duration<double, std::milli>(drawEnd - drawStart).count();

  if (FAILED(hr)) {
    SetLastError(s, hr);
    return false;
  }

  RecordFramePerformance(s, drawDurationMs);

  SetLastError(s, S_OK);
  return true;
}
