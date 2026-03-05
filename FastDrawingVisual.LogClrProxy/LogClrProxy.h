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
ref class LogProxy abstract sealed {
public:
  static bool Initialize();
  static bool Initialize(System::String ^ userConfigPath);
  static void Shutdown(int flushTimeoutMs);
  static void Flush(int flushTimeoutMs);
  static void Log(LogLevel level, System::String ^ category,
                  System::String ^ message);
  static void Log(LogLevel level, System::String ^ category,
                  System::String ^ message, bool isDirect);
  static void WriteETW(LogLevel level, System::String ^ category,
                       System::String ^ message);
  static void WriteETW(LogLevel level, System::String ^ category,
                       System::String ^ message, bool isDirect);
  /// <summary>Registers a periodic metric bucket.</summary>
  /// <param name="name">Metric name; empty uses an auto-generated name.</param>
  /// <param name="periodSec">Bucket period in seconds; minimum is 1.</param>
  /// <param name="format">
  /// Supported placeholders: {id}, {name}, {periodSec}, {windowMs},
  /// {count}/{samples}, {avg}, {min}, {max}.
  /// </param>
  /// <param name="level">Log level used when a bucket is emitted.</param>
  static int RegisterMetric(System::String ^ name, System::UInt32 periodSec,
                            System::String ^ format, LogLevel level);
  static bool UnregisterMetric(int metricId);
  static void LogMetric(int metricId, double value);
  static System::UInt64 GetDroppedTotal();
  static System::String ^ GetEffectiveConfigJson();
};
} // namespace FastDrawingVisual::Log
