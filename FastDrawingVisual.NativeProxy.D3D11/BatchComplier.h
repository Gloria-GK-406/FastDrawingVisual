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
#include "D3DBatchTypes.h"

namespace fdv::d3d11::batch {

class BatchCompiler {
 public:
  BatchCompiler() = default;

  void Reset(int width, int height, const void* commands, int commandBytes,
             const void* blobs, int blobBytes);
  bool TryGetNextBatch(CompiledBatchView& out, HRESULT& outErrorHr);

 private:
  bool TryReadNextCommand(fdv::protocol::Command& out, HRESULT& outErrorHr);
  bool AppendTriangleCommand(const fdv::protocol::Command& command,
                             HRESULT& outErrorHr);
  bool AppendTextCommand(const fdv::protocol::Command& command,
                         HRESULT& outErrorHr);

  std::optional<fdv::protocol::CommandReader> reader_;
  int width_ = 0;
  int height_ = 0;
  float widthF_ = 0.0f;
  float heightF_ = 0.0f;
  fdv::protocol::Command pendingCommand_{}; 
  bool hasPendingCommand_ = false;
  std::vector<TriangleVertex> triangleVertices_;
  std::vector<TextBatchItem> textItems_;
};

} // namespace fdv::d3d11::batch
