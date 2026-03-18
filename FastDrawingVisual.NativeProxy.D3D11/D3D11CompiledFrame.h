#pragma once

#include <cstdint>
#include <vector>

#include "D3DBatchTypes.h"
#include "BatchComplier.h"

#include "../FastDrawingVisual.NativeProxy.Shared/FramePacket.h"

namespace fdv::d3d11::compiled {

using LayeredFramePacket = fdv::nativeproxy::shared::LayeredFramePacket;

enum class CompiledOpKind : std::uint8_t {
  Clear = 0,
  Triangles = 1,
  ShapeInstances = 2,
  Text = 3,
  Image = 4,
};

struct CompiledOp {
  CompiledOpKind kind = CompiledOpKind::Clear;
  std::uint32_t payloadIndex = 0;
  float clearColor[4] = {};
};

struct CompiledShapeBatch {
  std::vector<batch::ShapeInstance> items;
};

struct CompiledTextBatch {
  std::vector<batch::TextBatchItem> items;
};

struct CompiledImageBatch {
  std::vector<std::vector<std::uint8_t>> pixelBlobs;
  std::vector<batch::ImageBatchItem> items;
};

struct CompiledLayer {
  int layerIndex = -1;
  std::vector<CompiledOp> ops;
  std::vector<CompiledShapeBatch> shapeBatches;
  std::vector<CompiledTextBatch> textBatches;
  std::vector<CompiledImageBatch> imageBatches;
};

struct CompileStats {
  int layerCount = 0;
  int clearBatchCount = 0;
  int triangleBatchCount = 0;
  int shapeBatchCount = 0;
  int textBatchCount = 0;
  int imageBatchCount = 0;
  int maxTriangleBatchVertexCount = 0;
  int maxTextBatchItemCount = 0;
  int maxImageBatchItemCount = 0;
  double compileMs = 0.0;
  batch::BatchCompileStats aggregate{};
};

struct CompiledFrame {
  int width = 0;
  int height = 0;
  std::vector<CompiledLayer> layers;
};

HRESULT CompileFrame(int width, int height, const LayeredFramePacket* framePacket,
                     CompiledFrame& outFrame, CompileStats& outStats);

} // namespace fdv::d3d11::compiled