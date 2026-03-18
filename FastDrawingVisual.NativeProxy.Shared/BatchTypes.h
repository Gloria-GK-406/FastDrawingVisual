#pragma once

#include <cstdint>
#include <string>

#include "CommandProtocol.g.h"

namespace fdv::nativeproxy::shared::batch {

enum class BatchKind : std::uint8_t {
  Clear = 0,
  Triangles = 1,
  ShapeInstances = 2,
  Text = 3,
  Image = 4,
  Unknown = 255,
};

enum class ShapeInstanceType : std::uint8_t {
  FillRect = 0,
  StrokeRect = 1,
  FillEllipse = 2,
  StrokeEllipse = 3,
  Line = 4,
};

struct TriangleVertex {
  float x;
  float y;
  float z;
  float r;
  float g;
  float b;
  float a;
};

struct RectInstance {
  float x;
  float y;
  float width;
  float height;
  float thickness;
  float r;
  float g;
  float b;
  float a;
};

struct EllipseInstance {
  float centerX;
  float centerY;
  float radiusX;
  float radiusY;
  float thickness;
  float r;
  float g;
  float b;
  float a;
};

struct ShapeInstance {
  float x;
  float y;
  float width;
  float height;
  float data0x;
  float data0y;
  float data0z;
  float data0w;
  float fillR;
  float fillG;
  float fillB;
  float fillA;
  float strokeR;
  float strokeG;
  float strokeB;
  float strokeA;
  float strokeWidth;
  float radius;
  float type;
  float flags;
};

struct TextBatchItem {
  std::wstring text;
  std::wstring fontFamily;
  float fontSize = 0.0f;
  float layoutLeft = 0.0f;
  float layoutTop = 0.0f;
  float layoutRight = 0.0f;
  float layoutBottom = 0.0f;
  fdv::protocol::ColorArgb8 color{};
};

struct ImageBatchItem {
  const std::uint8_t* pixels = nullptr;
  std::uint32_t pixelBytes = 0;
  std::uint32_t pixelWidth = 0;
  std::uint32_t pixelHeight = 0;
  std::uint32_t stride = 0;
  float destLeft = 0.0f;
  float destTop = 0.0f;
  float destRight = 0.0f;
  float destBottom = 0.0f;
};

struct BatchCommandStats {
  int32_t clearCount = 0;
  int32_t fillRectCount = 0;
  int32_t strokeRectCount = 0;
  int32_t fillEllipseCount = 0;
  int32_t strokeEllipseCount = 0;
  int32_t lineCount = 0;
  int32_t drawTextRunCount = 0;
  int32_t drawImageCount = 0;
};

struct BatchCompileStats {
  int32_t commandCount = 0;
  int32_t triangleVertexCount = 0;
  int32_t shapeInstanceCount = 0;
  int32_t textItemCount = 0;
  int32_t textCharCount = 0;
  int32_t imageItemCount = 0;
  int32_t imagePixelBytes = 0;
  double commandReadMs = 0.0;
  double commandBuildMs = 0.0;
  BatchCommandStats commands{};
};

struct CompiledBatchView {
  BatchKind kind = BatchKind::Clear;
  float clearColor[4] = {};
};

} // namespace fdv::nativeproxy::shared::batch
