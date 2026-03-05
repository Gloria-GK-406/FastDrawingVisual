#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <evntprov.h>

#include <string>

namespace fdvlog {

class LogEtwSink {
public:
  bool Initialize(bool enabled);
  void Shutdown();
  void WriteETW(const std::wstring &line, int level);

private:
  bool registered_ = false;
  REGHANDLE handle_ = 0;
};

} // namespace fdvlog
