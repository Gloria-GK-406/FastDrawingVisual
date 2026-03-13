#pragma once

#include <cstdint>

namespace fdv::nativeproxy::shared {

struct LayerPacket {
  const void* commandData = nullptr;
  int32_t commandBytes = 0;
  const void* blobData = nullptr;
  int32_t blobBytes = 0;
  int32_t commandCount = 0;
};

struct LayeredFramePacket {
  static constexpr int kMaxLayerCount = 8;
  LayerPacket layers[kMaxLayerCount];
};

} // namespace fdv::nativeproxy::shared
