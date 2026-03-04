#include "FdvLogCoreExports.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <evntprov.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")

const GUID kFdvLogEtwProviderId = {
    0x5f0add12,
    0x61d2,
    0x4ee9,
    {0x84, 0x71, 0x76, 0x8f, 0x2d, 0x6d, 0xa2, 0x0b},
};

namespace {
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
  uint32_t metricId = 0;
  int64_t value = 0;
  uint32_t windowMs = 1000;
  int aggregation = FDVLOG_AggregationRate;
};

struct LogEvent {
  EventType type = EventType::Text;
  uint64_t qpcTicks = 0;
  TextPayload text;
  MetricPayload metric;
};

struct MetricState {
  uint32_t windowMs = 1000;
  int aggregation = FDVLOG_AggregationRate;
  uint64_t windowStart100ns = 0;
  uint64_t windowEnd100ns = 0;
  int64_t sum = 0;
  int64_t minValue = 0;
  int64_t maxValue = 0;
  uint64_t sampleCount = 0;
  bool hasSamples = false;
  bool emptyEmittedSinceLastSample = false;
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

static uint64_t CombineFileTime(const FILETIME &ft) {
  ULARGE_INTEGER value{};
  value.LowPart = ft.dwLowDateTime;
  value.HighPart = ft.dwHighDateTime;
  return value.QuadPart;
}

static uint64_t QueryQpcNow() {
  LARGE_INTEGER value{};
  QueryPerformanceCounter(&value);
  return static_cast<uint64_t>(value.QuadPart);
}

static uint64_t QueryQpcFrequency() {
  LARGE_INTEGER value{};
  QueryPerformanceFrequency(&value);
  return static_cast<uint64_t>(value.QuadPart);
}

static uint64_t QuerySystemFileTimeNow100ns() {
  FILETIME ft{};
  GetSystemTimePreciseAsFileTime(&ft);
  return CombineFileTime(ft);
}

static uint64_t WindowDuration100ns(uint32_t windowMs) {
  const uint64_t safeWindowMs = std::max<uint32_t>(windowMs, 1);
  return safeWindowMs * 10000ULL;
}

static const wchar_t *LevelName(int level) {
  switch (level) {
  case FDVLOG_LevelTrace:
    return L"TRACE";
  case FDVLOG_LevelDebug:
    return L"DEBUG";
  case FDVLOG_LevelInfo:
    return L"INFO";
  case FDVLOG_LevelWarn:
    return L"WARN";
  case FDVLOG_LevelError:
    return L"ERROR";
  case FDVLOG_LevelFatal:
    return L"FATAL";
  default:
    return L"INFO";
  }
}

static const wchar_t *AggregationName(int aggregation) {
  switch (aggregation) {
  case FDVLOG_AggregationRate:
    return L"rate";
  case FDVLOG_AggregationAverage:
    return L"avg";
  case FDVLOG_AggregationSum:
    return L"sum";
  case FDVLOG_AggregationMin:
    return L"min";
  case FDVLOG_AggregationMax:
    return L"max";
  default:
    return L"rate";
  }
}

static UCHAR EtwLevelForLogLevel(int level) {
  switch (level) {
  case FDVLOG_LevelTrace:
  case FDVLOG_LevelDebug:
    return 5;
  case FDVLOG_LevelInfo:
    return 4;
  case FDVLOG_LevelWarn:
    return 3;
  case FDVLOG_LevelError:
    return 2;
  case FDVLOG_LevelFatal:
    return 1;
  default:
    return 4;
  }
}

static void CopyBoundedText(const wchar_t *source, wchar_t *destination,
                            size_t destinationChars) {
  if (!destination || destinationChars == 0)
    return;

  destination[0] = L'\0';
  if (!source)
    return;

  wcsncpy_s(destination, destinationChars, source, _TRUNCATE);
}

static std::wstring GetDirectoryPart(const std::wstring &path) {
  const auto index = path.find_last_of(L"\\/");
  if (index == std::wstring::npos)
    return std::wstring();
  return path.substr(0, index);
}

static std::wstring BuildRotatedPath(const std::wstring &basePath, int index) {
  return basePath + L"." + std::to_wstring(index);
}

static std::string WideToUtf8(const std::wstring &text) {
  if (text.empty())
    return {};

  const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1,
                                           nullptr, 0, nullptr, nullptr);
  if (required <= 1)
    return {};

  std::string utf8(static_cast<size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), required,
                      nullptr, nullptr);
  utf8.pop_back(); // Drop null terminator.
  return utf8;
}

class RotatingFileSink {
public:
  bool Initialize(const std::wstring &filePath, uint64_t maxBytes,
                  int maxFiles) {
    filePath_ = filePath;
    maxBytes_ = maxBytes > 0 ? maxBytes : static_cast<uint64_t>(kDefaultFileMaxBytes);
    maxFiles_ = std::max(maxFiles, 1);

    const std::wstring dir = GetDirectoryPart(filePath_);
    if (!dir.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
    }

    return OpenFile();
  }

  void WriteLine(const std::wstring &line) {
    if (!file_.is_open())
      return;

    RotateIfNeeded();
    const std::string utf8 = WideToUtf8(line);
    file_.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    file_.write("\n", 1);
  }

  void Flush() {
    if (file_.is_open())
      file_.flush();
  }

private:
  bool OpenFile() {
    file_.close();
    file_.open(std::filesystem::path(filePath_),
               std::ios::out | std::ios::app | std::ios::binary);
    return file_.is_open();
  }

  void RotateIfNeeded() {
    std::error_code ec;
    const auto path = std::filesystem::path(filePath_);
    const auto size = std::filesystem::exists(path, ec)
                          ? std::filesystem::file_size(path, ec)
                          : 0;
    if (ec || size < maxBytes_)
      return;

    file_.flush();
    file_.close();

    for (int i = maxFiles_ - 1; i >= 1; --i) {
      const std::filesystem::path src(BuildRotatedPath(filePath_, i));
      const std::filesystem::path dst(BuildRotatedPath(filePath_, i + 1));
      std::filesystem::remove(dst, ec);
      std::filesystem::rename(src, dst, ec);
    }

    const std::filesystem::path first(BuildRotatedPath(filePath_, 1));
    std::filesystem::remove(first, ec);
    std::filesystem::rename(path, first, ec);

    OpenFile();
  }

  std::wstring filePath_;
  uint64_t maxBytes_ = static_cast<uint64_t>(kDefaultFileMaxBytes);
  int maxFiles_ = kDefaultFileMaxFiles;
  std::ofstream file_;
};

class LoggerCore {
public:
  bool Initialize(const FDVLOG_Config *config) {
    LoggerConfig normalized{};
    NormalizeConfig(config, &normalized);

    const uint64_t freq = QueryQpcFrequency();
    if (freq == 0)
      return false;

    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      config_ = normalized;
      events_.assign(config_.ringBufferCapacity, LogEvent{});
      head_ = 0;
      tail_ = 0;
      count_ = 0;
      droppedTotal_.store(0);
    }

    qpcFrequency_ = freq;
    qpcBase_ = QueryQpcNow();
    fileTimeBase100ns_ = QuerySystemFileTimeNow100ns();
    lastFlushTick_ = std::chrono::steady_clock::now();

    if (config_.enableFileSink) {
      fileSink_ = std::make_unique<RotatingFileSink>();
      if (!fileSink_->Initialize(config_.filePath, config_.fileMaxBytes,
                                 config_.fileMaxFiles)) {
        fileSink_.reset();
      }
    }

    if (config_.enableEtw) {
      etwRegistered_ =
          EventRegister(&kFdvLogEtwProviderId, nullptr, nullptr, &etwHandle_) ==
          ERROR_SUCCESS;
    }

    running_.store(true);
    worker_ = std::thread([this]() { WorkerLoop(); });
    return true;
  }

  void Shutdown(uint32_t flushTimeoutMs) {
    Flush(flushTimeoutMs);

    running_.store(false);
    queueCv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }

    std::lock_guard<std::mutex> sinkLock(sinkMutex_);
    FlushSinksLocked();
    if (etwRegistered_) {
      EventUnregister(etwHandle_);
      etwHandle_ = 0;
      etwRegistered_ = false;
    }
    fileSink_.reset();
    metrics_.clear();
  }

  void Flush(uint32_t flushTimeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(flushTimeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (count_ == 0)
          break;
      }
      queueCv_.notify_all();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    std::lock_guard<std::mutex> sinkLock(sinkMutex_);
    FlushSinksLocked();
  }

  void Log(int level, const wchar_t *category, const wchar_t *message,
           bool direct) {
    LogEvent ev{};
    ev.type = EventType::Text;
    ev.qpcTicks = QueryQpcNow();
    ev.text.level = level;
    ev.text.threadId = GetCurrentThreadId();
    CopyBoundedText(category, ev.text.category, _countof(ev.text.category));
    CopyBoundedText(message, ev.text.message, _countof(ev.text.message));

    if (direct || level == FDVLOG_LevelFatal) {
      ProcessTextEvent(ev);
      return;
    }

    Enqueue(ev);
  }

  void Metric(uint32_t metricId, int64_t value, uint32_t windowMs,
              int aggregation) {
    LogEvent ev{};
    ev.type = EventType::Metric;
    ev.qpcTicks = QueryQpcNow();
    ev.metric.metricId = metricId;
    ev.metric.value = value;
    ev.metric.windowMs = std::max<uint32_t>(windowMs, 1);
    ev.metric.aggregation = aggregation;
    Enqueue(ev);
  }

  uint64_t DroppedTotal() const { return droppedTotal_.load(); }

private:
  static void NormalizeConfig(const FDVLOG_Config *input, LoggerConfig *output) {
    if (!output)
      return;

    if (!input)
      return;

    output->ringBufferCapacity =
        static_cast<size_t>(std::max(input->ringBufferCapacity, 128));
    output->flushIntervalMs = std::max(input->flushIntervalMs, 50);
    output->enableFileSink = input->enableFileSink;
    if (input->filePath && input->filePath[0] != L'\0')
      output->filePath = input->filePath;
    output->fileMaxBytes =
        static_cast<uint64_t>(std::max(input->fileMaxBytes, 1024 * 1024));
    output->fileMaxFiles = std::max(input->fileMaxFiles, 1);
    output->enableDebugOutput = input->enableDebugOutput;
    output->enableEtw = input->enableEtw;
  }

  void Enqueue(const LogEvent &event) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (events_.empty())
      return;

    if (count_ == events_.size()) {
      tail_ = (tail_ + 1) % events_.size();
      --count_;
      droppedTotal_.fetch_add(1);
    }

    events_[head_] = event;
    head_ = (head_ + 1) % events_.size();
    ++count_;
    queueCv_.notify_one();
  }

  bool TryDequeue(LogEvent *event) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (count_ == 0 || !event)
      return false;

    *event = events_[tail_];
    tail_ = (tail_ + 1) % events_.size();
    --count_;
    return true;
  }

  void WorkerLoop() {
    while (running_.load() || HasPendingEvents()) {
      LogEvent ev{};
      if (TryDequeue(&ev)) {
        if (ev.type == EventType::Text)
          ProcessTextEvent(ev);
        else
          ProcessMetricEvent(ev);
      } else {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueCv_.wait_for(lock, std::chrono::milliseconds(20),
                          [this]() { return count_ > 0 || !running_.load(); });
        lock.unlock();
        ProcessHeartbeat(QueryQpcNow());
      }

      const auto now = std::chrono::steady_clock::now();
      if (now - lastFlushTick_ >=
          std::chrono::milliseconds(config_.flushIntervalMs)) {
        std::lock_guard<std::mutex> sinkLock(sinkMutex_);
        FlushSinksLocked();
        lastFlushTick_ = now;
      }
    }

    ProcessHeartbeat(QueryQpcNow());
  }

  bool HasPendingEvents() {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return count_ > 0;
  }

  uint64_t QpcToFileTime100ns(uint64_t qpcTicks) const {
    if (qpcTicks <= qpcBase_)
      return fileTimeBase100ns_;
    const uint64_t delta = qpcTicks - qpcBase_;
    return fileTimeBase100ns_ + ((delta * 10000000ULL) / qpcFrequency_);
  }

  static std::wstring FormatLocalTimestamp(uint64_t fileTime100ns) {
    FILETIME utcFileTime{};
    ULARGE_INTEGER value{};
    value.QuadPart = fileTime100ns;
    utcFileTime.dwLowDateTime = value.LowPart;
    utcFileTime.dwHighDateTime = value.HighPart;

    FILETIME localFileTime{};
    FileTimeToLocalFileTime(&utcFileTime, &localFileTime);

    SYSTEMTIME localSystemTime{};
    FileTimeToSystemTime(&localFileTime, &localSystemTime);

    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
               localSystemTime.wYear, localSystemTime.wMonth,
               localSystemTime.wDay, localSystemTime.wHour,
               localSystemTime.wMinute, localSystemTime.wSecond,
               localSystemTime.wMilliseconds);
    return buffer;
  }

  void ProcessTextEvent(const LogEvent &ev) {
    const uint64_t ts100ns = QpcToFileTime100ns(ev.qpcTicks);
    const std::wstring timestamp = FormatLocalTimestamp(ts100ns);

    wchar_t lineBuffer[1024]{};
    swprintf_s(lineBuffer, L"[%s] [T%lu] [%s] [%s] %s", timestamp.c_str(),
               static_cast<unsigned long>(ev.text.threadId),
               LevelName(ev.text.level), ev.text.category, ev.text.message);

    const std::wstring line = lineBuffer;

    std::lock_guard<std::mutex> sinkLock(sinkMutex_);
    WriteTextLineLocked(line);

    if (etwRegistered_)
      EventWriteString(etwHandle_, EtwLevelForLogLevel(ev.text.level), 0,
                       line.c_str());
  }

  void ProcessMetricEvent(const LogEvent &ev) {
    const uint64_t ts100ns = QpcToFileTime100ns(ev.qpcTicks);
    auto &state = metrics_[ev.metric.metricId];

    if (state.windowEnd100ns == 0 || state.windowMs != ev.metric.windowMs ||
        state.aggregation != ev.metric.aggregation) {
      InitializeMetricState(state, ev.metric.windowMs, ev.metric.aggregation,
                            ts100ns);
    }

    AdvanceMetricWindow(state, ev.metric.metricId, ts100ns);
    AccumulateMetric(state, ev.metric.value);
  }

  void ProcessHeartbeat(uint64_t nowQpcTicks) {
    const uint64_t now100ns = QpcToFileTime100ns(nowQpcTicks);
    for (auto &entry : metrics_) {
      AdvanceMetricWindow(entry.second, entry.first, now100ns);
    }
  }

  static void InitializeMetricState(MetricState &state, uint32_t windowMs,
                                    int aggregation, uint64_t ts100ns) {
    const uint64_t duration = WindowDuration100ns(windowMs);
    const uint64_t start = (ts100ns / duration) * duration;

    state.windowMs = windowMs;
    state.aggregation = aggregation;
    state.windowStart100ns = start;
    state.windowEnd100ns = start + duration;
    state.sum = 0;
    state.minValue = 0;
    state.maxValue = 0;
    state.sampleCount = 0;
    state.hasSamples = false;
    state.emptyEmittedSinceLastSample = false;
  }

  static void AccumulateMetric(MetricState &state, int64_t value) {
    if (!state.hasSamples) {
      state.sum = value;
      state.minValue = value;
      state.maxValue = value;
      state.sampleCount = 1;
      state.hasSamples = true;
      state.emptyEmittedSinceLastSample = false;
      return;
    }

    state.sum += value;
    state.minValue = std::min(state.minValue, value);
    state.maxValue = std::max(state.maxValue, value);
    ++state.sampleCount;
  }

  void AdvanceMetricWindow(MetricState &state, uint32_t metricId,
                           uint64_t ts100ns) {
    const uint64_t duration = WindowDuration100ns(state.windowMs);
    if (duration == 0)
      return;

    while (ts100ns >= state.windowEnd100ns) {
      EmitMetricWindow(metricId, state);
      state.windowStart100ns = state.windowEnd100ns;
      state.windowEnd100ns += duration;
      state.sum = 0;
      state.sampleCount = 0;
      state.hasSamples = false;
      state.minValue = 0;
      state.maxValue = 0;

      if (!state.hasSamples && state.emptyEmittedSinceLastSample &&
          ts100ns >= state.windowEnd100ns) {
        const uint64_t windowsToSkip =
            (ts100ns - state.windowEnd100ns) / duration;
        state.windowStart100ns += windowsToSkip * duration;
        state.windowEnd100ns += windowsToSkip * duration;
      }
    }
  }

  void EmitMetricWindow(uint32_t metricId, MetricState &state) {
    bool hasValue = false;
    double value = 0.0;
    if (state.hasSamples) {
      switch (state.aggregation) {
      case FDVLOG_AggregationRate:
        value = static_cast<double>(state.sampleCount);
        hasValue = true;
        break;
      case FDVLOG_AggregationAverage:
        value = static_cast<double>(state.sum) /
                static_cast<double>(state.sampleCount);
        hasValue = true;
        break;
      case FDVLOG_AggregationSum:
        value = static_cast<double>(state.sum);
        hasValue = true;
        break;
      case FDVLOG_AggregationMin:
        value = static_cast<double>(state.minValue);
        hasValue = true;
        break;
      case FDVLOG_AggregationMax:
        value = static_cast<double>(state.maxValue);
        hasValue = true;
        break;
      default:
        break;
      }
      state.emptyEmittedSinceLastSample = false;
    } else {
      if (state.emptyEmittedSinceLastSample)
        return;

      if (state.aggregation == FDVLOG_AggregationRate ||
          state.aggregation == FDVLOG_AggregationSum) {
        value = 0.0;
        hasValue = true;
      }
      state.emptyEmittedSinceLastSample = true;
    }

    wchar_t lineBuffer[512]{};
    if (hasValue) {
      swprintf_s(lineBuffer,
                 L"[metric] id=%u windowMs=%u agg=%s value=%.3f samples=%llu",
                 metricId, state.windowMs, AggregationName(state.aggregation),
                 value, static_cast<unsigned long long>(state.sampleCount));
    } else {
      swprintf_s(lineBuffer,
                 L"[metric] id=%u windowMs=%u agg=%s value=null samples=0",
                 metricId, state.windowMs, AggregationName(state.aggregation));
    }

    std::lock_guard<std::mutex> sinkLock(sinkMutex_);
    WriteTextLineLocked(lineBuffer);

    if (etwRegistered_)
      EventWriteString(etwHandle_, 4, 0, lineBuffer);
  }

  void WriteTextLineLocked(const std::wstring &line) {
    if (fileSink_) {
      fileSink_->WriteLine(line);
    }

#if defined(_DEBUG)
    if (config_.enableDebugOutput) {
      std::wstring withBreak = line + L"\n";
      OutputDebugStringW(withBreak.c_str());
    }
#endif
  }

  void FlushSinksLocked() {
    if (fileSink_)
      fileSink_->Flush();
  }

private:
  LoggerConfig config_{};

  mutable std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::vector<LogEvent> events_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t count_ = 0;

  std::atomic<bool> running_{false};
  std::thread worker_;
  std::atomic<uint64_t> droppedTotal_{0};

  uint64_t qpcFrequency_ = 0;
  uint64_t qpcBase_ = 0;
  uint64_t fileTimeBase100ns_ = 0;

  std::chrono::steady_clock::time_point lastFlushTick_{};

  std::mutex sinkMutex_;
  std::unique_ptr<RotatingFileSink> fileSink_;
  bool etwRegistered_ = false;
  REGHANDLE etwHandle_ = 0;
  std::unordered_map<uint32_t, MetricState> metrics_;
};

std::mutex g_loggerGuard;
std::shared_ptr<LoggerCore> g_logger;

std::shared_ptr<LoggerCore> GetLogger() {
  std::lock_guard<std::mutex> lock(g_loggerGuard);
  return g_logger;
}

} // namespace

extern "C" {
bool __cdecl FDVLOG_Initialize(const FDVLOG_Config *config) {
  auto next = std::make_shared<LoggerCore>();
  if (!next->Initialize(config))
    return false;

  std::shared_ptr<LoggerCore> old;
  {
    std::lock_guard<std::mutex> lock(g_loggerGuard);
    old = std::move(g_logger);
    g_logger = std::move(next);
  }

  if (old)
    old->Shutdown(1000);
  return true;
}

void __cdecl FDVLOG_Shutdown(int flushTimeoutMs) {
  std::shared_ptr<LoggerCore> old;
  {
    std::lock_guard<std::mutex> lock(g_loggerGuard);
    old = std::move(g_logger);
  }

  if (old) {
    old->Shutdown(static_cast<uint32_t>(std::max(flushTimeoutMs, 0)));
  }
}

void __cdecl FDVLOG_Flush(int flushTimeoutMs) {
  auto logger = GetLogger();
  if (!logger)
    return;
  logger->Flush(static_cast<uint32_t>(std::max(flushTimeoutMs, 0)));
}

void __cdecl FDVLOG_Log(int level, const wchar_t *category,
                        const wchar_t *message, bool direct) {
  auto logger = GetLogger();
  if (!logger)
    return;
  logger->Log(level, category, message, direct);
}

void __cdecl FDVLOG_Metric(uint32_t metricId, int64_t value, uint32_t windowMs,
                           int aggregation) {
  auto logger = GetLogger();
  if (!logger)
    return;
  logger->Metric(metricId, value, windowMs, aggregation);
}

uint64_t __cdecl FDVLOG_GetDroppedTotal() {
  auto logger = GetLogger();
  if (!logger)
    return 0;
  return logger->DroppedTotal();
}
}
