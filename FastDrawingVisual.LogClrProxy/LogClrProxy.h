#pragma once

namespace FastDrawingVisual::Log {
public
enum class LogLevel : int {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
  Fatal = 5
};

public
enum class MetricAggregation : int {
  Count = 0,
  Rate = Count,
  Average = 1,
  Sum = 2,
  Min = 3,
  Max = 4
};

public
ref class LogProxy abstract sealed {
public:
  static bool Initialize();
  static bool Initialize(System::String ^ userConfigPath);
  static void Shutdown(int flushTimeoutMs);
  static void Flush(int flushTimeoutMs);
  static void Write(LogLevel level, System::String ^ category,
                    System::String ^ message);
  static void WriteDirect(LogLevel level, System::String ^ category,
                          System::String ^ message);
  static int RegisterMetric(System::String ^ name, System::UInt32 windowMs,
                            MetricAggregation aggregation,
                            System::String ^ format, LogLevel level);
  static bool UnregisterMetric(int metricId);
  static void LogMetric(int metricId, double value);
  static System::UInt64 GetDroppedTotal();
  static System::String ^ GetEffectiveConfigJson();
};
} // namespace FastDrawingVisual::Log
