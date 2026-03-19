#include "LogClrProxy.h"

#include "../FastDrawingVisual.LogCore/FdvLogCoreExports.h"

#include <vcclr.h>

using namespace System;
using namespace System::Diagnostics;
using namespace System::Threading;

namespace {

String ^ Coalesce(String ^ value, String ^ fallback) {
  return String::IsNullOrWhiteSpace(value) ? fallback : value;
}

String ^ DescribeException(String ^ operation, Exception ^ ex) {
  operation = Coalesce(operation, "operation failed");
  if (ex == nullptr) {
    return operation;
  }

  return String::Format("{0}: {1}: {2}", operation, ex->GetType()->Name,
                        Coalesce(ex->Message, String::Empty));
}

void WriteFallback(String ^ category, String ^ channel, String ^ message) {
  Debug::WriteLine(String::Format("[{0}][{1}] {2}",
                                  Coalesce(category, "managed"),
                                  Coalesce(channel, "Fallback"),
                                  Coalesce(message, String::Empty)));
}

} // namespace

namespace FastDrawingVisual::Log {

void LogProxy::Log(LogLevel level, String ^ category, String ^ message) {
  Log(level, category, message, false);
}

void LogProxy::Log(LogLevel level, String ^ category, String ^ message,
                   bool isDirect) {
  category = Coalesce(category, "managed");
  message = Coalesce(message, String::Empty);

  pin_ptr<const wchar_t> c = PtrToStringChars(category);
  pin_ptr<const wchar_t> m = PtrToStringChars(message);
  FDVLOG_Log(static_cast<int>(level), c, m, isDirect);
}

void LogProxy::WriteETW(LogLevel level, String ^ category, String ^ message) {
  WriteETW(level, category, message, false);
}

void LogProxy::WriteETW(LogLevel level, String ^ category, String ^ message,
                        bool isDirect) {
  category = Coalesce(category, "managed");
  message = Coalesce(message, String::Empty);

  pin_ptr<const wchar_t> c = PtrToStringChars(category);
  pin_ptr<const wchar_t> m = PtrToStringChars(message);
  FDVLOG_WriteETW(static_cast<int>(level), c, m, isDirect);
}

int LogProxy::RegisterMetric(String ^ name, UInt32 periodSec, String ^ format,
                             LogLevel level) {
  name = Coalesce(name, String::Empty);
  format = Coalesce(format, String::Empty);

  pin_ptr<const wchar_t> n = PtrToStringChars(name);
  pin_ptr<const wchar_t> f = PtrToStringChars(format);

  MetricSpec spec{};
  spec.name = name->Length > 0 ? n : nullptr;
  spec.periodSec = Math::Max(periodSec, 1U);
  spec.format = format->Length > 0 ? f : nullptr;
  spec.level = static_cast<int>(level);
  return FDVLOG_RegisterMetric(&spec);
}

bool LogProxy::UnregisterMetric(int metricId) {
  return FDVLOG_UnregisterMetric(metricId);
}

void LogProxy::LogMetric(int metricId, double value) {
  FDVLOG_LogMetric(metricId, value);
}

bool LogProxy::IsInitialized() { return FDVLOG_IsInitialized(); }

Metric ^ LogProxy::CreateMetric(String ^ name, UInt32 periodSec,
                                String ^ format, LogLevel level) {
  return gcnew Metric("managed", name,
                      RegisterMetric(name, periodSec, format, level));
}

Metric::Metric(String ^ category, String ^ name, int metricId)
    : category_(Coalesce(category, "managed")),
      name_(Coalesce(name, String::Empty)), metricId_(metricId) {}

Metric::~Metric() {
  Release(true);
  GC::SuppressFinalize(this);
}

Metric::!Metric() { Release(false); }

void Metric::Add(double value) {
  const int metricId = metricId_;
  if (metricId <= 0) {
    return;
  }

  try {
    LogProxy::LogMetric(metricId, value);
  } catch (Exception ^ ex) {
    WriteFallback(category_, "MetricFallback",
                  DescribeException("metric add failed", ex));
  }
}

void Metric::Add(float value) { Add(static_cast<double>(value)); }

bool Metric::IsValid::get() { return metricId_ > 0; }

int Metric::Id::get() { return metricId_; }

String ^ Metric::Name::get() { return name_; }

void Metric::Release(bool reportFailure) {
  const int metricId = Interlocked::Exchange(metricId_, 0);
  if (metricId <= 0) {
    return;
  }

  if (!LogProxy::IsInitialized()) {
    return;
  }

  try {
    LogProxy::UnregisterMetric(metricId);
  } catch (Exception ^ ex) {
    if (reportFailure) {
      WriteFallback(category_, "MetricFallback",
                    DescribeException("metric unregister failed", ex));
    }
  } catch (...) {
  }
}

Logger::Logger(String ^ category) : category_(Coalesce(category, "managed")) {}

String ^ Logger::Category::get() { return category_; }

void Logger::Log(LogLevel level, String ^ message) {
  Write(level, message, false);
}

void Logger::LogEtw(LogLevel level, String ^ message) {
  Write(level, message, true);
}

void Logger::Debug(String ^ message) { Write(LogLevel::Debug, message, false); }

void Logger::Info(String ^ message) { Write(LogLevel::Info, message, false); }

void Logger::Warn(String ^ message) { Write(LogLevel::Warn, message, false); }

void Logger::Error(String ^ message) { Write(LogLevel::Error, message, false); }

void Logger::DebugEtw(String ^ message) {
  Write(LogLevel::Debug, message, true);
}

void Logger::InfoEtw(String ^ message) { Write(LogLevel::Info, message, true); }

void Logger::WarnEtw(String ^ message) { Write(LogLevel::Warn, message, true); }

void Logger::ErrorEtw(String ^ message) {
  Write(LogLevel::Error, message, true);
}

Metric ^ Logger::CreateMetric(String ^ name, UInt32 periodSec, String ^ format,
                              LogLevel level) {
  try {
    return gcnew Metric(category_, name,
                        LogProxy::RegisterMetric(name, periodSec, format, level));
  } catch (Exception ^ ex) {
    WriteFallback(category_, "MetricFallback",
                  DescribeException("create metric failed", ex));
    return gcnew Metric(category_, name, 0);
  }
}

int Logger::RegisterMetric(String ^ name, UInt32 periodSec, String ^ format,
                           LogLevel level) {
  try {
    return LogProxy::RegisterMetric(name, periodSec, format, level);
  } catch (Exception ^ ex) {
    WriteFallback(category_, "MetricFallback",
                  DescribeException("register failed", ex));
    return 0;
  }
}

bool Logger::UnregisterMetric(int metricId) {
  try {
    return LogProxy::UnregisterMetric(metricId);
  } catch (Exception ^ ex) {
    WriteFallback(category_, "MetricFallback",
                  DescribeException("unregister failed", ex));
    return false;
  }
}

void Logger::LogMetric(int metricId, double value) {
  try {
    LogProxy::LogMetric(metricId, value);
  } catch (Exception ^ ex) {
    WriteFallback(category_, "MetricFallback",
                  DescribeException("log metric failed", ex));
  }
}

void Logger::Write(LogLevel level, String ^ message, bool emitEtw) {
  try {
    LogProxy::Log(level, category_, message);
    if (emitEtw) {
      LogProxy::WriteETW(level, category_, message);
    }
  } catch (Exception ^ ex) {
    WriteFallback(category_, "LogFallback", DescribeException("log failed", ex));
  }
}

} // namespace FastDrawingVisual::Log
