#include "LogClrProxy.h"

#include "../FastDrawingVisual.LogCore/FdvLogCoreExports.h"

#include <vcclr.h>

using namespace System;

namespace {

String ^ Coalesce(String ^ value, String ^ fallback) {
  return String::IsNullOrWhiteSpace(value) ? fallback : value;
}

} // namespace

namespace FastDrawingVisual::Log {

bool LogProxy::Initialize() { return FDVLOG_Initialize(nullptr); }

bool LogProxy::Initialize(String ^ userConfigPath) {
  (void)userConfigPath;
  return FDVLOG_Initialize(nullptr);
}

void LogProxy::Shutdown(int flushTimeoutMs) {
  FDVLOG_Shutdown(Math::Max(flushTimeoutMs, 0));
}

void LogProxy::Flush(int flushTimeoutMs) {
  FDVLOG_Flush(Math::Max(flushTimeoutMs, 0));
}

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

  FDVLOG_MetricSpec spec{};
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

UInt64 LogProxy::GetDroppedTotal() { return FDVLOG_GetDroppedTotal(); }

String ^ LogProxy::GetEffectiveConfigJson() { return String::Empty; }

} // namespace FastDrawingVisual::Log
