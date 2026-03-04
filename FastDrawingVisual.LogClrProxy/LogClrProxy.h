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
  Rate = 0,
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
  static void Metric(System::UInt32 metricId, System::Int64 value,
                     System::UInt32 windowMs, MetricAggregation aggregation);
  static System::UInt64 GetDroppedTotal();
  static System::String ^ GetEffectiveConfigJson();
};
} // namespace FastDrawingVisual::Log
