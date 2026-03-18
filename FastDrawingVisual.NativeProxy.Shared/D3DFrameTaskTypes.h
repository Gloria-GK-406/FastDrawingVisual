#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <condition_variable>
#include <mutex>

namespace fdv::nativeproxy::shared {

struct D3DFrameTaskResult;

struct D3DFrameTaskCompletion {
  std::mutex mutex;
  std::condition_variable condition;
  bool completed = false;
  D3DFrameTaskResult* resultSink = nullptr;
};

struct D3DFrameExecuteStats {
  double shapeDrawMs = 0.0;
  double imageDrawMs = 0.0;
  double imageFlushMs = 0.0;
  double imageRecordMs = 0.0;
  double imageEndDrawMs = 0.0;
  double textDrawMs = 0.0;
  double textFlushMs = 0.0;
  double textRecordMs = 0.0;
  double textEndDrawMs = 0.0;
};

struct D3DFrameTaskResult {
  HRESULT hr = S_OK;
  D3DFrameExecuteStats stats{};
};

} // namespace fdv::nativeproxy::shared
