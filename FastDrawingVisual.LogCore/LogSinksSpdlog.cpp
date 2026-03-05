#include "LogSinksSpdlog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace fdvlog {
namespace {

static spdlog::level::level_enum ToSpdlogLevel(int level) {
  switch (level) {
  case FDVLOG_LevelTrace:
    return spdlog::level::trace;
  case FDVLOG_LevelDebug:
    return spdlog::level::debug;
  case FDVLOG_LevelInfo:
    return spdlog::level::info;
  case FDVLOG_LevelWarn:
    return spdlog::level::warn;
  case FDVLOG_LevelError:
    return spdlog::level::err;
  case FDVLOG_LevelFatal:
    return spdlog::level::critical;
  default:
    return spdlog::level::info;
  }
}

static std::wstring GetDirectoryPart(const std::wstring &path) {
  const auto index = path.find_last_of(L"\\/");
  if (index == std::wstring::npos)
    return std::wstring();
  return path.substr(0, index);
}

static std::string WideToUtf8(const std::wstring &text) {
  if (text.empty())
    return {};

  const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1,
                                           nullptr, 0, nullptr, nullptr);
  if (required <= 1)
    return {};

  std::string utf8(static_cast<size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), required,
                      nullptr, nullptr);
  utf8.pop_back();
  return utf8;
}

static bool ParseBooleanValue(std::wstring text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  return text == L"1" || text == L"true" || text == L"on" || text == L"yes";
}

static bool IsConsoleSinkEnabledByEnv() {
  wchar_t buffer[16]{};
  const DWORD length = GetEnvironmentVariableW(
      L"FDVLOG_ENABLE_CONSOLE", buffer, static_cast<DWORD>(_countof(buffer)));
  if (length == 0 || length >= _countof(buffer))
    return false;
  return ParseBooleanValue(std::wstring(buffer, length));
}

} // namespace

bool LogSinksSpdlog::Initialize(const LoggerConfig &config) {
  std::vector<spdlog::sink_ptr> sinks;

  if (config.enableFileSink) {
    const std::wstring dir = GetDirectoryPart(config.filePath);
    if (!dir.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
    }

    try {
      auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          config.filePath, config.fileMaxBytes,
          static_cast<size_t>(std::max(config.fileMaxFiles, 1)));
      sinks.push_back(fileSink);
    } catch (...) {
      // Keep running with remaining sinks.
    }
  }

#if defined(_DEBUG)
  if (config.enableDebugOutput) {
    try {
      sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
    } catch (...) {
      // Keep running with remaining sinks.
    }
  }
#endif

  if (IsConsoleSinkEnabledByEnv()) {
    try {
      sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    } catch (...) {
      // Keep running with remaining sinks.
    }
  }

  if (sinks.empty()) {
    sinks.push_back(std::make_shared<spdlog::sinks::null_sink_mt>());
  }

  logger_ = std::make_shared<spdlog::logger>("fdvlog", sinks.begin(), sinks.end());
  logger_->set_level(spdlog::level::trace);
  logger_->set_pattern("%v");
  logger_->flush_on(spdlog::level::err);
  return true;
}

void LogSinksSpdlog::Shutdown() { logger_.reset(); }

void LogSinksSpdlog::Log(const std::wstring &line, int level) {
  if (!logger_)
    return;

  try {
    logger_->log(ToSpdlogLevel(level), WideToUtf8(line));
  } catch (...) {
    // Avoid surfacing sink failures to callers.
  }
}

void LogSinksSpdlog::Flush() {
  if (!logger_)
    return;

  try {
    logger_->flush();
  } catch (...) {
    // Best-effort flush.
  }
}

} // namespace fdvlog
