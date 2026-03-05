#pragma once

#include "LogTypes.h"

#include <memory>
#include <string>

namespace spdlog {
class logger;
}

namespace fdvlog {

class LogSinksSpdlog {
public:
  bool Initialize(const LoggerConfig &config);
  void Shutdown();
  void Log(const std::wstring &line, int level);
  void Flush();

private:
  std::shared_ptr<spdlog::logger> logger_;
};

} // namespace fdvlog
