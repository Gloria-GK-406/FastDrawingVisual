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

ref class LogProxy abstract sealed {
public:
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
};

public
/// <summary>
/// Category-bound managed logger. Native LogCore owns lazy initialization and
/// process-wide lifetime.
/// </summary>
ref class Logger sealed {
public:
  Logger(System::String ^ category);

  property System::String ^ Category {
    System::String ^ get();
  }

  void Log(LogLevel level, System::String ^ message);
  void LogEtw(LogLevel level, System::String ^ message);

  void Debug(System::String ^ message);
  void Info(System::String ^ message);
  void Warn(System::String ^ message);
  void Error(System::String ^ message);

  void DebugEtw(System::String ^ message);
  void InfoEtw(System::String ^ message);
  void WarnEtw(System::String ^ message);
  void ErrorEtw(System::String ^ message);

  /// <summary>Registers a periodic metric bucket.</summary>
  /// <param name="name">Metric name; empty uses an auto-generated name.</param>
  /// <param name="periodSec">Bucket period in seconds; minimum is 1.</param>
  /// <param name="format">
  /// Supported placeholders: {id}, {name}, {periodSec}, {windowMs},
  /// {count}/{samples}, {avg}, {min}, {max}.
  /// </param>
  /// <param name="level">Log level used when a bucket is emitted.</param>
  int RegisterMetric(System::String ^ name, System::UInt32 periodSec,
                     System::String ^ format, LogLevel level);
  bool UnregisterMetric(int metricId);
  void LogMetric(int metricId, double value);

private:
  void Write(LogLevel level, System::String ^ message, bool emitEtw);

  initonly System::String ^ category_;
};
} // namespace FastDrawingVisual::Log
