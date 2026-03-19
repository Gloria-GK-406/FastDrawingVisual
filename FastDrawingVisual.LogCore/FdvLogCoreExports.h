#pragma once

#include <stdint.h>

#if defined(FDVLOGCORE_EXPORTS)
#define FDVLOG_API __declspec(dllexport)
#else
#define FDVLOG_API __declspec(dllimport)
#endif

extern "C" {
enum FDVLOG_Level {
  FDVLOG_LevelTrace = 0,
  FDVLOG_LevelDebug = 1,
  FDVLOG_LevelInfo = 2,
  FDVLOG_LevelWarn = 3,
  FDVLOG_LevelError = 4,
  FDVLOG_LevelFatal = 5
};

typedef struct MetricSpec {
  const wchar_t *name;
  uint32_t periodSec;
  const wchar_t *format;
  int level;
} MetricSpec;

typedef struct FDVLOG_Config {
  int ringBufferCapacity;
  int flushIntervalMs;
  bool enableFileSink;
  const wchar_t *filePath;
  int fileMaxBytes;
  int fileMaxFiles;
  bool enableDebugOutput;
  bool enableEtw;
} FDVLOG_Config;

FDVLOG_API bool __cdecl FDVLOG_Initialize(const FDVLOG_Config *config);
FDVLOG_API bool __cdecl FDVLOG_IsInitialized();
FDVLOG_API void __cdecl FDVLOG_Shutdown(int flushTimeoutMs);
FDVLOG_API void __cdecl FDVLOG_Flush(int flushTimeoutMs);
FDVLOG_API void __cdecl FDVLOG_Log(int level, const wchar_t *category,
                                   const wchar_t *message, bool isDirect);
FDVLOG_API void __cdecl FDVLOG_WriteETW(int level, const wchar_t *category,
                                        const wchar_t *message, bool isDirect);
FDVLOG_API int __cdecl FDVLOG_RegisterMetric(const MetricSpec *spec);
FDVLOG_API bool __cdecl FDVLOG_UnregisterMetric(int metricId);
FDVLOG_API void __cdecl FDVLOG_LogMetric(int metricId, double value);
FDVLOG_API uint64_t __cdecl FDVLOG_GetDroppedTotal();
}
