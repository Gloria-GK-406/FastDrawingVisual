#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include "FdvLogCoreExports.h"
#include "LogEtwSink.h"
#include "LogMetricsAggregator.h"
#include "LogSinksSpdlog.h"
#include "LogTypes.h"
#include "RingBuffer.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#pragma comment(lib, "advapi32.lib")

namespace fdvlog {
namespace {

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

static void CopyBoundedText(const wchar_t *source, wchar_t *destination,
                            size_t destinationChars) {
  if (!destination || destinationChars == 0)
    return;

  destination[0] = L'\0';
  if (!source)
    return;

  wcsncpy_s(destination, destinationChars, source, _TRUNCATE);
}

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
      events_.Reset(config_.ringBufferCapacity);
      droppedTotal_.store(0, std::memory_order_relaxed);
    }

    qpcFrequency_ = freq;
    qpcBase_ = QueryQpcNow();
    fileTimeBase100ns_ = QuerySystemFileTimeNow100ns();
    lastFlushTick_ = std::chrono::steady_clock::now();

    textSinks_.Initialize(config_);
    etwSink_.Initialize(config_.enableEtw);

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
    textSinks_.Flush();
    textSinks_.Shutdown();
    etwSink_.Shutdown();
    metrics_.Reset();
  }

  void Flush(uint32_t flushTimeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(flushTimeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (events_.Empty())
          break;
      }
      queueCv_.notify_all();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    std::lock_guard<std::mutex> sinkLock(sinkMutex_);
    textSinks_.Flush();
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

  uint64_t DroppedTotal() const {
    return droppedTotal_.load(std::memory_order_relaxed);
  }

private:
  void Enqueue(const LogEvent &event) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    const bool overwritten = events_.PushOverwrite(event);
    if (overwritten) {
      droppedTotal_.fetch_add(1, std::memory_order_relaxed);
    }
    queueCv_.notify_one();
  }

  bool TryDequeue(LogEvent *event) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return events_.TryPop(event);
  }

  bool HasPendingEvents() {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return !events_.Empty();
  }

  uint64_t QpcToFileTime100ns(uint64_t qpcTicks) const {
    if (qpcTicks <= qpcBase_)
      return fileTimeBase100ns_;
    const uint64_t delta = qpcTicks - qpcBase_;
    return fileTimeBase100ns_ + ((delta * 10000000ULL) / qpcFrequency_);
  }

  void EmitLineLocked(const std::wstring &line, int level) {
    textSinks_.Log(line, level);
    etwSink_.WriteETW(line, level);
  }

  void EmitLine(const std::wstring &line, int level) {
    std::lock_guard<std::mutex> sinkLock(sinkMutex_);
    EmitLineLocked(line, level);
  }

  void ProcessTextEvent(const LogEvent &ev) {
    const uint64_t ts100ns = QpcToFileTime100ns(ev.qpcTicks);
    const std::wstring timestamp = FormatLocalTimestamp(ts100ns);

    wchar_t lineBuffer[1024]{};
    swprintf_s(lineBuffer, L"[%s] [T%lu] [%s] [%s] %s", timestamp.c_str(),
               static_cast<unsigned long>(ev.text.threadId),
               LevelName(ev.text.level), ev.text.category, ev.text.message);

    EmitLine(lineBuffer, ev.text.level);
  }

  void ProcessMetricEvent(const LogEvent &ev) {
    const uint64_t ts100ns = QpcToFileTime100ns(ev.qpcTicks);
    metrics_.OnMetricEvent(
        ev.metric, ts100ns,
        [this](const std::wstring &line, int level) { EmitLine(line, level); });
  }

  void ProcessHeartbeat(uint64_t nowQpcTicks) {
    const uint64_t now100ns = QpcToFileTime100ns(nowQpcTicks);
    metrics_.OnHeartbeat(
        now100ns,
        [this](const std::wstring &line, int level) { EmitLine(line, level); });
  }

  void WorkerLoop() {
    while (running_.load() || HasPendingEvents()) {
      LogEvent ev{};
      if (TryDequeue(&ev)) {
        if (ev.type == EventType::Text) {
          ProcessTextEvent(ev);
        } else {
          ProcessMetricEvent(ev);
        }
      } else {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueCv_.wait_for(lock, std::chrono::milliseconds(20), [this]() {
          return !events_.Empty() || !running_.load();
        });
        lock.unlock();
        ProcessHeartbeat(QueryQpcNow());
      }

      const auto now = std::chrono::steady_clock::now();
      if (now - lastFlushTick_ >=
          std::chrono::milliseconds(config_.flushIntervalMs)) {
        std::lock_guard<std::mutex> sinkLock(sinkMutex_);
        textSinks_.Flush();
        lastFlushTick_ = now;
      }
    }

    ProcessHeartbeat(QueryQpcNow());
  }

private:
  LoggerConfig config_{};

  mutable std::mutex queueMutex_;
  std::condition_variable queueCv_;
  RingBuffer<LogEvent> events_;

  std::atomic<bool> running_{false};
  std::thread worker_;
  std::atomic<uint64_t> droppedTotal_{0};

  uint64_t qpcFrequency_ = 0;
  uint64_t qpcBase_ = 0;
  uint64_t fileTimeBase100ns_ = 0;

  std::chrono::steady_clock::time_point lastFlushTick_{};

  std::mutex sinkMutex_;
  LogSinksSpdlog textSinks_;
  LogEtwSink etwSink_;
  LogMetricsAggregator metrics_;
};

std::mutex g_loggerGuard;
std::shared_ptr<LoggerCore> g_logger;

std::shared_ptr<LoggerCore> GetLogger() {
  std::lock_guard<std::mutex> lock(g_loggerGuard);
  return g_logger;
}

} // namespace
} // namespace fdvlog

extern "C" {

bool __cdecl FDVLOG_Initialize(const FDVLOG_Config *config) {
  auto next = std::make_shared<fdvlog::LoggerCore>();
  if (!next->Initialize(config))
    return false;

  std::shared_ptr<fdvlog::LoggerCore> old;
  {
    std::lock_guard<std::mutex> lock(fdvlog::g_loggerGuard);
    old = std::move(fdvlog::g_logger);
    fdvlog::g_logger = std::move(next);
  }

  if (old)
    old->Shutdown(1000);
  return true;
}

void __cdecl FDVLOG_Shutdown(int flushTimeoutMs) {
  std::shared_ptr<fdvlog::LoggerCore> old;
  {
    std::lock_guard<std::mutex> lock(fdvlog::g_loggerGuard);
    old = std::move(fdvlog::g_logger);
  }

  if (old) {
    old->Shutdown(static_cast<uint32_t>(std::max(flushTimeoutMs, 0)));
  }
}

void __cdecl FDVLOG_Flush(int flushTimeoutMs) {
  auto logger = fdvlog::GetLogger();
  if (!logger)
    return;
  logger->Flush(static_cast<uint32_t>(std::max(flushTimeoutMs, 0)));
}

void __cdecl FDVLOG_Log(int level, const wchar_t *category,
                        const wchar_t *message, bool direct) {
  auto logger = fdvlog::GetLogger();
  if (!logger)
    return;
  logger->Log(level, category, message, direct);
}

void __cdecl FDVLOG_Metric(uint32_t metricId, int64_t value, uint32_t windowMs,
                           int aggregation) {
  auto logger = fdvlog::GetLogger();
  if (!logger)
    return;
  logger->Metric(metricId, value, windowMs, aggregation);
}

uint64_t __cdecl FDVLOG_GetDroppedTotal() {
  auto logger = fdvlog::GetLogger();
  if (!logger)
    return 0;
  return logger->DroppedTotal();
}

} // extern "C"
