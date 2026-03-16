#include "BatchComplier.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stringapiset.h>

namespace fdv::nativeproxy::shared::batch {
namespace {

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

ColorF TransparentColor() { return {0.0f, 0.0f, 0.0f, 0.0f}; }

float ToShapeTypeValue(ShapeInstanceType type) {
  return static_cast<float>(static_cast<std::uint32_t>(type));
}

ShapeInstance MakeShapeInstance(float x, float y, float width, float height,
                                float data0x, float data0y, float data0z,
                                float data0w, const ColorF& fillColor,
                                const ColorF& strokeColor, float strokeWidth,
                                float radius, ShapeInstanceType type,
                                float flags = 0.0f) {
  return {
      x,
      y,
      width,
      height,
      data0x,
      data0y,
      data0z,
      data0w,
      fillColor.r,
      fillColor.g,
      fillColor.b,
      fillColor.a,
      strokeColor.r,
      strokeColor.g,
      strokeColor.b,
      strokeColor.a,
      strokeWidth,
      radius,
      ToShapeTypeValue(type),
      flags,
  };
}

ShapeInstance MakeFillRectInstance(const fdv::protocol::FillRectPayload& payload) {
  return MakeShapeInstance(payload.x, payload.y, payload.width, payload.height,
                           0.0f, 0.0f, 0.0f, 0.0f,
                           ToPremultipliedColor(payload.color),
                           TransparentColor(), 0.0f, 0.0f,
                           ShapeInstanceType::FillRect);
}

ShapeInstance MakeStrokeRectInstance(
    const fdv::protocol::StrokeRectPayload& payload) {
  return MakeShapeInstance(payload.x, payload.y, payload.width, payload.height,
                           0.0f, 0.0f, 0.0f, 0.0f, TransparentColor(),
                           ToPremultipliedColor(payload.color),
                           (std::max)(1.0f, payload.thickness), 0.0f,
                           ShapeInstanceType::StrokeRect);
}

ShapeInstance MakeFillEllipseInstance(
    const fdv::protocol::FillEllipsePayload& payload) {
  return MakeShapeInstance(payload.centerX - payload.radiusX,
                           payload.centerY - payload.radiusY,
                           payload.radiusX * 2.0f, payload.radiusY * 2.0f,
                           0.0f, 0.0f, 0.0f, 0.0f,
                           ToPremultipliedColor(payload.color),
                           TransparentColor(), 0.0f, 0.0f,
                           ShapeInstanceType::FillEllipse);
}

ShapeInstance MakeStrokeEllipseInstance(
    const fdv::protocol::StrokeEllipsePayload& payload) {
  const float thickness = (std::max)(1.0f, payload.thickness);
  const float outerRadiusX = payload.radiusX + thickness * 0.5f;
  const float outerRadiusY = payload.radiusY + thickness * 0.5f;
  return MakeShapeInstance(
      payload.centerX - outerRadiusX, payload.centerY - outerRadiusY,
      outerRadiusX * 2.0f, outerRadiusY * 2.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      TransparentColor(), ToPremultipliedColor(payload.color), thickness, 0.0f,
      ShapeInstanceType::StrokeEllipse);
}

ShapeInstance MakeLineInstance(const fdv::protocol::LinePayload& payload) {
  const float thickness = (std::max)(1.0f, payload.thickness);
  const float halfThickness = thickness * 0.5f;
  const float minX = (std::min)(payload.x0, payload.x1) - halfThickness;
  const float minY = (std::min)(payload.y0, payload.y1) - halfThickness;
  const float maxX = (std::max)(payload.x0, payload.x1) + halfThickness;
  const float maxY = (std::max)(payload.y0, payload.y1) + halfThickness;
  const float centerX = (minX + maxX) * 0.5f;
  const float centerY = (minY + maxY) * 0.5f;
  return MakeShapeInstance(
      minX, minY, maxX - minX, maxY - minY, payload.x0 - centerX,
      payload.y0 - centerY, payload.x1 - centerX, payload.y1 - centerY,
      TransparentColor(), ToPremultipliedColor(payload.color), thickness, 0.0f,
      ShapeInstanceType::Line);
}

bool IsShapeCommand(fdv::protocol::CommandType type) {
  switch (type) {
  case fdv::protocol::CommandType::FillRect:
  case fdv::protocol::CommandType::StrokeRect:
  case fdv::protocol::CommandType::FillEllipse:
  case fdv::protocol::CommandType::StrokeEllipse:
  case fdv::protocol::CommandType::Line:
    return true;
  default:
    return false;
  }
}

double DurationMs(const std::chrono::steady_clock::time_point& start,
                  const std::chrono::steady_clock::time_point& end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

void AccumulateCommandStats(BatchCompileStats& stats,
                            fdv::protocol::CommandType type) {
  ++stats.commandCount;
  switch (type) {
  case fdv::protocol::CommandType::Clear:
    ++stats.commands.clearCount;
    break;
  case fdv::protocol::CommandType::FillRect:
    ++stats.commands.fillRectCount;
    break;
  case fdv::protocol::CommandType::StrokeRect:
    ++stats.commands.strokeRectCount;
    break;
  case fdv::protocol::CommandType::FillEllipse:
    ++stats.commands.fillEllipseCount;
    break;
  case fdv::protocol::CommandType::StrokeEllipse:
    ++stats.commands.strokeEllipseCount;
    break;
  case fdv::protocol::CommandType::Line:
    ++stats.commands.lineCount;
    break;
  case fdv::protocol::CommandType::DrawTextRun:
    ++stats.commands.drawTextRunCount;
    break;
  default:
    break;
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
  lastBatchStats_ = {};
  shapeInstances_.clear();
  textItems_.clear();
}

bool BatchCompiler::TryGetNextBatch(CompiledBatchView& out,
                                    HRESULT& outErrorHr) {
  out = {};
  outErrorHr = S_OK;
  lastBatchStats_ = {};

  shapeInstances_.clear();
  textItems_.clear();

  fdv::protocol::Command command{};
  if (hasPendingCommand_) {
    command = pendingCommand_;
    hasPendingCommand_ = false;
  } else {
    const auto readStart = std::chrono::steady_clock::now();
    const bool hasCommand = reader_->TryReadNext(command);
    const auto readEnd = std::chrono::steady_clock::now();
    lastBatchStats_.commandReadMs += DurationMs(readStart, readEnd);
    if (!hasCommand) {
      outErrorHr = S_FALSE;
      return false;
    }
  }

  BatchKind kind = BatchKind::Clear;
  if (IsShapeCommand(command.type)) {
    kind = BatchKind::ShapeInstances;
  } else if (command.type == fdv::protocol::CommandType::DrawTextRun) {
    kind = BatchKind::Text;
  }
  out.kind = kind;

  if (kind == BatchKind::Clear) {
    AccumulateCommandStats(lastBatchStats_, command.type);

    const auto& payload =
        std::get<fdv::protocol::ClearPayload>(command.payload);
    const ColorF clearColor = ToPremultipliedColor(payload.color);
    out.clearColor[0] = clearColor.r;
    out.clearColor[1] = clearColor.g;
    out.clearColor[2] = clearColor.b;
    out.clearColor[3] = clearColor.a;
    return true;
  }

  while (true) {
    const auto appendStart = std::chrono::steady_clock::now();

    switch (command.type) {
    case fdv::protocol::CommandType::FillRect:
      shapeInstances_.push_back(MakeFillRectInstance(
          std::get<fdv::protocol::FillRectPayload>(command.payload)));
      AccumulateCommandStats(lastBatchStats_, command.type);
      ++lastBatchStats_.shapeInstanceCount;
      break;

    case fdv::protocol::CommandType::StrokeRect:
      shapeInstances_.push_back(MakeStrokeRectInstance(
          std::get<fdv::protocol::StrokeRectPayload>(command.payload)));
      AccumulateCommandStats(lastBatchStats_, command.type);
      ++lastBatchStats_.shapeInstanceCount;
      break;

    case fdv::protocol::CommandType::FillEllipse:
      shapeInstances_.push_back(MakeFillEllipseInstance(
          std::get<fdv::protocol::FillEllipsePayload>(command.payload)));
      AccumulateCommandStats(lastBatchStats_, command.type);
      ++lastBatchStats_.shapeInstanceCount;
      break;

    case fdv::protocol::CommandType::StrokeEllipse:
      shapeInstances_.push_back(MakeStrokeEllipseInstance(
          std::get<fdv::protocol::StrokeEllipsePayload>(command.payload)));
      AccumulateCommandStats(lastBatchStats_, command.type);
      ++lastBatchStats_.shapeInstanceCount;
      break;

    case fdv::protocol::CommandType::Line:
      shapeInstances_.push_back(MakeLineInstance(
          std::get<fdv::protocol::LinePayload>(command.payload)));
      AccumulateCommandStats(lastBatchStats_, command.type);
      ++lastBatchStats_.shapeInstanceCount;
      break;

    case fdv::protocol::CommandType::DrawTextRun: {
      const auto& payload =
          std::get<fdv::protocol::DrawTextRunPayload>(command.payload);
      fdv::protocol::BlobSpan textUtf8{};
      fdv::protocol::BlobSpan fontFamilyUtf8{};
      reader_->TryResolveBlob(payload.textUtf8, textUtf8);
      reader_->TryResolveBlob(payload.fontFamilyUtf8, fontFamilyUtf8);

      AccumulateCommandStats(lastBatchStats_, command.type);
      if (textUtf8.bytes != 0) {
        TextBatchItem item{};
        Utf8ToWide(textUtf8.data, textUtf8.bytes, item.text);
        Utf8ToWide(fontFamilyUtf8.data, fontFamilyUtf8.bytes, item.fontFamily);
        if (item.fontFamily.empty()) {
          item.fontFamily = L"Segoe UI";
        }

        item.fontSize = (std::max)(1.0f, payload.fontSize);
        item.layoutLeft = payload.x;
        item.layoutTop = payload.y;
        item.layoutRight = widthF_;
        item.layoutBottom = heightF_;
        item.color = payload.color;

        ++lastBatchStats_.textItemCount;
        lastBatchStats_.textCharCount += static_cast<int32_t>(item.text.size());
        textItems_.push_back(std::move(item));
      }
      break;
    }

    default:
      break;
    }

    const auto appendEnd = std::chrono::steady_clock::now();
    lastBatchStats_.commandBuildMs += DurationMs(appendStart, appendEnd);

    fdv::protocol::Command next{};
    const auto readStart = std::chrono::steady_clock::now();
    const bool hasNext = reader_->TryReadNext(next);
    const auto readEnd = std::chrono::steady_clock::now();
    lastBatchStats_.commandReadMs += DurationMs(readStart, readEnd);
    if (!hasNext) {
      break;
    }

    bool keepBatch = false;
    switch (kind) {
    case BatchKind::ShapeInstances:
      keepBatch = IsShapeCommand(next.type);
      break;
    case BatchKind::Text:
      keepBatch = next.type == fdv::protocol::CommandType::DrawTextRun;
      break;
    default:
      break;
    }

    if (!keepBatch) {
      pendingCommand_ = next;
      hasPendingCommand_ = true;
      break;
    }

    command = next;
  }

  if (kind == BatchKind::ShapeInstances) {
    out.shapeInstances = shapeInstances_.data();
    out.shapeInstanceCount = static_cast<int32_t>(shapeInstances_.size());
  } else if (kind == BatchKind::Text) {
    out.textItems = textItems_.data();
    out.textItemCount = static_cast<int32_t>(textItems_.size());
  }

  return true;
}

} // namespace fdv::nativeproxy::shared::batch
