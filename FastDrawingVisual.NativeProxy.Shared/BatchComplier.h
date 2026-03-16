#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "BridgeCommandProtocol.g.h"
#include "BatchTypes.h"

namespace fdv::nativeproxy::shared::batch {

class BatchCompiler {
 public:
  BatchCompiler() = default;

  void Reset(int width, int height, const void* commands, int commandBytes,
             const void* blobs, int blobBytes);
  HRESULT TryGetNextBatch(CompiledBatchView& out);
  HRESULT TryGetNextBatch2(CompiledBatchView& out);
  const std::vector<ShapeInstance>& GetShapeInstances() const {
    return shapeInstances_;
  }
  const std::vector<TextBatchItem>& GetTextItems() const { return textItems_; }
  const BatchCompileStats& lastBatchStats() const { return lastBatchStats_; }

 private:
  std::optional<fdv::protocol::CommandReader> reader_;
  int width_ = 0;
  int height_ = 0;
  float widthF_ = 0.0f;
  float heightF_ = 0.0f;
  BatchCompileStats lastBatchStats_{};
  std::vector<ShapeInstance> shapeInstances_;
  std::vector<TextBatchItem> textItems_;
};

} // namespace fdv::nativeproxy::shared::batch
