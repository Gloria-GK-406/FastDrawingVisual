#pragma once

#include <cstdint>
#include <string>

#include "BridgeCommandProtocol.g.h"

namespace fdv::d3d11::batch {

enum class BatchKind : std::uint8_t {
  Clear = 0,
  Triangles = 1,
  Text = 2,
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

struct CompiledBatchView {
  BatchKind kind = BatchKind::Clear;
  float clearColor[4] = {};
  const TriangleVertex* triangleVertices = nullptr;
  int32_t triangleVertexCount = 0;
  const TextBatchItem* textItems = nullptr;
  int32_t textItemCount = 0;
};

} // namespace fdv::d3d11::batch
