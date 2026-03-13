#include "BatchComplier.h"

#include <algorithm>
#include <cmath>
#include <stringapiset.h>

namespace fdv::d3d11::batch {
namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr int kEllipseSegmentCount = 48;

struct ColorF {
  float r;
  float g;
  float b;
  float a;
};

bool Utf8ToWide(const std::uint8_t* bytes, std::uint32_t count,
                std::wstring& out) {
  out.clear();
  if (count == 0) {
    return true;
  }

  const int wideCount = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<LPCCH>(bytes),
      static_cast<int>(count), nullptr, 0);
  if (wideCount <= 0) {
    return false;
  }

  out.resize(static_cast<std::size_t>(wideCount));
  const int converted = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<LPCCH>(bytes),
      static_cast<int>(count), out.data(), wideCount);
  if (converted <= 0) {
    out.clear();
    return false;
  }

  return true;
}

ColorF ToPremultipliedColor(const fdv::protocol::ColorArgb8& color) {
  const float a = static_cast<float>(color.a) / 255.0f;
  return {
      (static_cast<float>(color.r) / 255.0f) * a,
      (static_cast<float>(color.g) / 255.0f) * a,
      (static_cast<float>(color.b) / 255.0f) * a,
      a,
  };
}

float ToNdcX(float width, float x) {
  return (x / width) * 2.0f - 1.0f;
}

float ToNdcY(float height, float y) {
  return 1.0f - (y / height) * 2.0f;
}

TriangleVertex MakeVertex(float width, float height, float x, float y,
                          const ColorF& color) {
  return {ToNdcX(width, x), ToNdcY(height, y), 0.0f, color.r, color.g,
          color.b, color.a};
}

void AppendFilledRect(float width, float height,
                      std::vector<TriangleVertex>& out, float x, float y,
                      float w, float h, const ColorF& color) {
  if (w <= 0.0f || h <= 0.0f) {
    return;
  }

  const float x0 = x;
  const float y0 = y;
  const float x1 = x + w;
  const float y1 = y + h;

  out.push_back(MakeVertex(width, height, x0, y0, color));
  out.push_back(MakeVertex(width, height, x1, y0, color));
  out.push_back(MakeVertex(width, height, x0, y1, color));

  out.push_back(MakeVertex(width, height, x1, y0, color));
  out.push_back(MakeVertex(width, height, x1, y1, color));
  out.push_back(MakeVertex(width, height, x0, y1, color));
}

void AppendStrokeRect(float width, float height,
                      std::vector<TriangleVertex>& out, float x, float y,
                      float w, float h, float thickness,
                      const ColorF& color) {
  if (w <= 0.0f || h <= 0.0f) {
    return;
  }

  const float t = (std::max)(1.0f, thickness);
  if (t * 2.0f >= w || t * 2.0f >= h) {
    AppendFilledRect(width, height, out, x, y, w, h, color);
    return;
  }

  AppendFilledRect(width, height, out, x, y, w, t, color);
  AppendFilledRect(width, height, out, x, y + h - t, w, t, color);
  AppendFilledRect(width, height, out, x, y + t, t, h - 2.0f * t, color);
  AppendFilledRect(width, height, out, x + w - t, y + t, t, h - 2.0f * t,
                   color);
}

void AppendFilledEllipse(float width, float height,
                         std::vector<TriangleVertex>& out, float cx,
                         float cy, float rx, float ry, const ColorF& color) {
  if (rx <= 0.0f || ry <= 0.0f) {
    return;
  }

  const TriangleVertex center = MakeVertex(width, height, cx, cy, color);
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
    out.push_back(MakeVertex(width, height, x0, y0, color));
    out.push_back(MakeVertex(width, height, x1, y1, color));
  }
}

void AppendStrokeEllipse(float width, float height,
                         std::vector<TriangleVertex>& out, float cx,
                         float cy, float rx, float ry, float thickness,
                         const ColorF& color) {
  if (rx <= 0.0f || ry <= 0.0f) {
    return;
  }

  const float t = (std::max)(1.0f, thickness);
  const float outerRx = rx + t * 0.5f;
  const float outerRy = ry + t * 0.5f;
  const float innerRx = rx - t * 0.5f;
  const float innerRy = ry - t * 0.5f;

  if (innerRx <= 0.0f || innerRy <= 0.0f) {
    AppendFilledEllipse(width, height, out, cx, cy, outerRx, outerRy, color);
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

    out.push_back(MakeVertex(width, height, ox0, oy0, color));
    out.push_back(MakeVertex(width, height, ox1, oy1, color));
    out.push_back(MakeVertex(width, height, ix0, iy0, color));

    out.push_back(MakeVertex(width, height, ox1, oy1, color));
    out.push_back(MakeVertex(width, height, ix1, iy1, color));
    out.push_back(MakeVertex(width, height, ix0, iy0, color));
  }
}

void AppendLine(float width, float height,
                std::vector<TriangleVertex>& out, float x0, float y0,
                float x1, float y1, float thickness, const ColorF& color) {
  const float t = (std::max)(1.0f, thickness);
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const float len = std::sqrt(dx * dx + dy * dy);

  if (len < 0.0001f) {
    const float half = t * 0.5f;
    AppendFilledRect(width, height, out, x0 - half, y0 - half, t, t, color);
    return;
  }

  const float half = t * 0.5f;
  const float nx = -dy / len * half;
  const float ny = dx / len * half;

  const TriangleVertex v0 = MakeVertex(width, height, x0 + nx, y0 + ny, color);
  const TriangleVertex v1 = MakeVertex(width, height, x1 + nx, y1 + ny, color);
  const TriangleVertex v2 = MakeVertex(width, height, x1 - nx, y1 - ny, color);
  const TriangleVertex v3 = MakeVertex(width, height, x0 - nx, y0 - ny, color);

  out.push_back(v0);
  out.push_back(v1);
  out.push_back(v2);

  out.push_back(v0);
  out.push_back(v2);
  out.push_back(v3);
}

BatchKind GetBatchKind(fdv::protocol::CommandType type) {
  switch (type) {
  case fdv::protocol::CommandType::Clear:
    return BatchKind::Clear;
  case fdv::protocol::CommandType::FillRect:
  case fdv::protocol::CommandType::StrokeRect:
  case fdv::protocol::CommandType::FillEllipse:
  case fdv::protocol::CommandType::StrokeEllipse:
  case fdv::protocol::CommandType::Line:
    return BatchKind::Triangles;
  case fdv::protocol::CommandType::DrawTextRun:
    return BatchKind::Text;
  default:
    return BatchKind::Clear;
  }
}
} // namespace

void BatchCompiler::Reset(int width, int height, const void* commands,
                          int commandBytes, const void* blobs, int blobBytes) {
  reader_.emplace(commands, commandBytes, blobs, blobBytes);
  width_ = width;
  height_ = height;
  widthF_ = static_cast<float>(width);
  heightF_ = static_cast<float>(height);
  hasPendingCommand_ = false;
  triangleVertices_.clear();
  textItems_.clear();
}

bool BatchCompiler::TryReadNextCommand(fdv::protocol::Command& out,
                                       HRESULT& outErrorHr) {
  outErrorHr = S_OK;
  if (!reader_.has_value()) {
    outErrorHr = S_FALSE;
    return false;
  }

  const bool hasCommand = reader_->TryReadNext(out);
  if (!hasCommand) {
    outErrorHr = reader_->HasError() ? E_INVALIDARG : S_FALSE;
    return false;
  }

  return true;
}

bool BatchCompiler::AppendTriangleCommand(const fdv::protocol::Command& command,
                                          HRESULT& outErrorHr) {
  outErrorHr = S_OK;

  switch (command.type) {
  case fdv::protocol::CommandType::FillRect: {
    const auto& payload =
        std::get<fdv::protocol::FillRectPayload>(command.payload);
    AppendFilledRect(widthF_, heightF_, triangleVertices_, payload.x, payload.y,
                     payload.width, payload.height,
                     ToPremultipliedColor(payload.color));
    return true;
  }

  case fdv::protocol::CommandType::StrokeRect: {
    const auto& payload =
        std::get<fdv::protocol::StrokeRectPayload>(command.payload);
    AppendStrokeRect(widthF_, heightF_, triangleVertices_, payload.x, payload.y,
                     payload.width, payload.height, payload.thickness,
                     ToPremultipliedColor(payload.color));
    return true;
  }

  case fdv::protocol::CommandType::FillEllipse: {
    const auto& payload =
        std::get<fdv::protocol::FillEllipsePayload>(command.payload);
    AppendFilledEllipse(widthF_, heightF_, triangleVertices_, payload.centerX,
                        payload.centerY, payload.radiusX, payload.radiusY,
                        ToPremultipliedColor(payload.color));
    return true;
  }

  case fdv::protocol::CommandType::StrokeEllipse: {
    const auto& payload =
        std::get<fdv::protocol::StrokeEllipsePayload>(command.payload);
    AppendStrokeEllipse(widthF_, heightF_, triangleVertices_, payload.centerX,
                        payload.centerY, payload.radiusX, payload.radiusY,
                        payload.thickness, ToPremultipliedColor(payload.color));
    return true;
  }

  case fdv::protocol::CommandType::Line: {
    const auto& payload = std::get<fdv::protocol::LinePayload>(command.payload);
    AppendLine(widthF_, heightF_, triangleVertices_, payload.x0, payload.y0,
               payload.x1, payload.y1, payload.thickness,
               ToPremultipliedColor(payload.color));
    return true;
  }

  default:
    outErrorHr = E_INVALIDARG;
    return false;
  }
}

bool BatchCompiler::AppendTextCommand(const fdv::protocol::Command& command,
                                      HRESULT& outErrorHr) {
  outErrorHr = S_OK;
  if (!reader_.has_value() ||
      command.type != fdv::protocol::CommandType::DrawTextRun) {
    outErrorHr = E_INVALIDARG;
    return false;
  }

  const auto& payload =
      std::get<fdv::protocol::DrawTextRunPayload>(command.payload);

  TextBatchItem item{};
  fdv::protocol::BlobSpan textUtf8{};
  fdv::protocol::BlobSpan fontFamilyUtf8{};
  if (!reader_->TryResolveBlob(payload.textUtf8, textUtf8) ||
      !reader_->TryResolveBlob(payload.fontFamilyUtf8, fontFamilyUtf8)) {
    outErrorHr = E_INVALIDARG;
    return false;
  }

  if (textUtf8.bytes == 0) {
    return true;
  }

  if (!Utf8ToWide(textUtf8.data, textUtf8.bytes, item.text) ||
      item.text.empty()) {
    outErrorHr = E_INVALIDARG;
    return false;
  }

  if (!Utf8ToWide(fontFamilyUtf8.data, fontFamilyUtf8.bytes, item.fontFamily)) {
    outErrorHr = E_INVALIDARG;
    return false;
  }

  if (item.fontFamily.empty()) {
    item.fontFamily = L"Segoe UI";
  }

  item.fontSize = (std::max)(1.0f, payload.fontSize);
  item.layoutLeft = payload.x;
  item.layoutTop = payload.y;
  item.layoutRight = widthF_;
  item.layoutBottom = heightF_;
  if (item.layoutRight <= item.layoutLeft) {
    item.layoutRight = item.layoutLeft + 1.0f;
  }
  if (item.layoutBottom <= item.layoutTop) {
    item.layoutBottom = item.layoutTop + 1.0f;
  }
  item.color = payload.color;
  textItems_.push_back(item);
  return true;
}

bool BatchCompiler::TryGetNextBatch(CompiledBatchView& out,
                                    HRESULT& outErrorHr) {
  out = {};
  outErrorHr = S_OK;

  triangleVertices_.clear();
  textItems_.clear();

  if (width_ <= 0 || height_ <= 0) {
    outErrorHr = E_INVALIDARG;
    return false;
  }

  fdv::protocol::Command command{};
  if (hasPendingCommand_) {
    command = pendingCommand_;
    hasPendingCommand_ = false;
  } else if (!TryReadNextCommand(command, outErrorHr)) {
    return false;
  }

  const BatchKind kind = GetBatchKind(command.type);
  out.kind = kind;

  if (kind == BatchKind::Clear) {
    if (command.type != fdv::protocol::CommandType::Clear) {
      outErrorHr = E_INVALIDARG;
      return false;
    }

    const auto& payload =
        std::get<fdv::protocol::ClearPayload>(command.payload);
    const ColorF clearColor = ToPremultipliedColor(payload.color);
    out.clearColor[0] = clearColor.r;
    out.clearColor[1] = clearColor.g;
    out.clearColor[2] = clearColor.b;
    out.clearColor[3] = clearColor.a;
    return true;
  }

  auto appendCurrent = [&](const fdv::protocol::Command& current) {
    switch (kind) {
    case BatchKind::Triangles:
      return AppendTriangleCommand(current, outErrorHr);
    case BatchKind::Text:
      return AppendTextCommand(current, outErrorHr);
    default:
      outErrorHr = E_INVALIDARG;
      return false;
    }
  };

  if (!appendCurrent(command)) {
    return false;
  }

  while (true) {
    fdv::protocol::Command next{};
    HRESULT readHr = S_OK;
    if (!TryReadNextCommand(next, readHr)) {
      if (readHr == S_FALSE) {
        break;
      }

      outErrorHr = readHr;
      return false;
    }

    const BatchKind nextKind = GetBatchKind(next.type);
    if (nextKind == BatchKind::Clear || nextKind != kind) {
      pendingCommand_ = next;
      hasPendingCommand_ = true;
      break;
    }

    if (!appendCurrent(next)) {
      return false;
    }
  }

  if (kind == BatchKind::Triangles) {
    out.triangleVertices = triangleVertices_.data();
    out.triangleVertexCount = static_cast<int32_t>(triangleVertices_.size());
  } else if (kind == BatchKind::Text) {
    out.textItems = textItems_.data();
    out.textItemCount = static_cast<int32_t>(textItems_.size());
  }

  return true;
}

} // namespace fdv::d3d11::batch
