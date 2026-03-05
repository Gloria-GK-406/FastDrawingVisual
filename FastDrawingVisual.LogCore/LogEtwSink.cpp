#include "LogEtwSink.h"

#include "FdvLogCoreExports.h"

namespace fdvlog {
namespace {

const GUID kFdvLogEtwProviderId = {
    0x5f0add12,
    0x61d2,
    0x4ee9,
    {0x84, 0x71, 0x76, 0x8f, 0x2d, 0x6d, 0xa2, 0x0b},
};

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

} // namespace

bool LogEtwSink::Initialize(bool enabled) {
  Shutdown();
  if (!enabled)
    return true;

  registered_ =
      EventRegister(&kFdvLogEtwProviderId, nullptr, nullptr, &handle_) ==
      ERROR_SUCCESS;
  return registered_;
}

void LogEtwSink::Shutdown() {
  if (!registered_)
    return;

  EventUnregister(handle_);
  handle_ = 0;
  registered_ = false;
}

void LogEtwSink::WriteETW(const std::wstring &line, int level) {
  if (!registered_)
    return;

  EventWriteString(handle_, EtwLevelForLogLevel(level), 0, line.c_str());
}

} // namespace fdvlog
