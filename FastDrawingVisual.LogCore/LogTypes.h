#pragma once

#include "FdvLogCoreExports.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace fdvlog {

constexpr size_t kMaxCategoryChars = 63;
constexpr size_t kMaxMessageChars = 511;
constexpr int kDefaultRingBufferCapacity = 8192;
constexpr int kDefaultFlushIntervalMs = 200;
constexpr int kDefaultFileMaxBytes = 30 * 1024 * 1024;
constexpr int kDefaultFileMaxFiles = 10;

enum class EventType : uint8_t { Text, Metric };

struct TextPayload {
  int level = FDVLOG_LevelInfo;
  uint32_t threadId = 0;
  wchar_t category[kMaxCategoryChars + 1]{};
  wchar_t message[kMaxMessageChars + 1]{};
};

struct MetricPayload {
  int metricId = 0;
  double value = 0.0;
};

struct LogEvent {
  EventType type = EventType::Text;
  uint64_t qpcTicks = 0;
  TextPayload text;
  MetricPayload metric;
};

struct LoggerConfig {
  size_t ringBufferCapacity = static_cast<size_t>(kDefaultRingBufferCapacity);
  int flushIntervalMs = kDefaultFlushIntervalMs;
  bool enableFileSink = true;
  std::wstring filePath = L"logs\\fastdrawingvisual.log";
  uint64_t fileMaxBytes = static_cast<uint64_t>(kDefaultFileMaxBytes);
  int fileMaxFiles = kDefaultFileMaxFiles;
  bool enableDebugOutput = true;
  bool enableEtw = true;
};

} // namespace fdvlog
