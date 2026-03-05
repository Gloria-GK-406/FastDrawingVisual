#include "LogClrProxy.h"

#include "../FastDrawingVisual.LogCore/FdvLogCoreExports.h"

#include <vcclr.h>

using namespace System;
using namespace System::Collections::Generic;
using namespace System::IO;
using namespace System::Text::Json;
using namespace System::Text::Json::Nodes;

namespace {
String ^ DefaultConfigJson() {
  return "{"
         "\"version\":1,"
         "\"ringBuffer\":{\"capacity\":8192},"
         "\"flush\":{\"intervalMs\":200},"
         "\"sinks\":{"
         "\"file\":{\"enabled\":true,\"path\":\"logs/fastdrawingvisual.log\","
         "\"maxFileSizeMB\":30,\"maxFiles\":10},"
         "\"outputDebugString\":{\"enabled\":true},"
         "\"etw\":{\"enabled\":true}"
         "},"
         "\"shutdown\":{\"flushTimeoutMs\":2000}"
         "}";
}

ref class ProxyState abstract sealed {
public:
  static String ^ EffectiveConfigJson = nullptr;
};

ref class RingBufferSection {
public:
  property int Capacity;
};

ref class FlushSection {
public:
  property int IntervalMs;
};

ref class FileSinkSection {
public:
  property bool Enabled;
  property String ^ Path;
  property int MaxFileSizeMB;
  property int MaxFiles;
};

ref class DebugOutputSection {
public:
  property bool Enabled;
};

ref class EtwSection {
public:
  property bool Enabled;
};

ref class SinksSection {
public:
  property FileSinkSection ^ File;
  property DebugOutputSection ^ OutputDebugString;
  property EtwSection ^ Etw;
};

ref class ShutdownSection {
public:
  property int FlushTimeoutMs;
};

ref class RootConfig {
public:
  property RingBufferSection ^ RingBuffer;
  property FlushSection ^ Flush;
  property SinksSection ^ Sinks;
  property ShutdownSection ^ Shutdown;
};

JsonNode ^ MergeJsonNode(JsonNode ^ baseline, JsonNode ^ overrideNode) {
  auto cloneNode = [](JsonNode ^ node) -> JsonNode ^ {
    if (node == nullptr)
      return nullptr;
    return JsonNode::Parse(node->ToJsonString());
  };

  if (overrideNode == nullptr) {
    return cloneNode(baseline);
  }

  auto baselineObject = dynamic_cast<JsonObject ^>(baseline);
  auto overrideObject = dynamic_cast<JsonObject ^>(overrideNode);
  if (baselineObject != nullptr && overrideObject != nullptr) {
    auto merged = gcnew JsonObject();
    for each (KeyValuePair<String ^, JsonNode ^> kvp in baselineObject) {
      merged[kvp.Key] = cloneNode(kvp.Value);
    }
    for each (KeyValuePair<String ^, JsonNode ^> kvp in overrideObject) {
      JsonNode ^ existing = nullptr;
      merged->TryGetPropertyValue(kvp.Key, existing);
      merged[kvp.Key] = MergeJsonNode(existing, kvp.Value);
    }
    return merged;
  }

  return cloneNode(overrideNode);
}

String ^ MergeConfigJson(String ^ userConfigPath) {
  JsonNode ^ defaults = JsonNode::Parse(DefaultConfigJson());
  if (defaults == nullptr)
    throw gcnew InvalidOperationException("Invalid default logger config.");

  if (String::IsNullOrWhiteSpace(userConfigPath)) {
    userConfigPath =
        Path::Combine(AppContext::BaseDirectory, "fastdrawingvisual.log.json");
  }

  if (!File::Exists(userConfigPath)) {
    auto serializeOptions = gcnew JsonSerializerOptions();
    serializeOptions->WriteIndented = true;
    return defaults->ToJsonString(serializeOptions);
  }

  String ^ userJson = File::ReadAllText(userConfigPath);
  if (String::IsNullOrWhiteSpace(userJson)) {
    auto serializeOptions = gcnew JsonSerializerOptions();
    serializeOptions->WriteIndented = true;
    return defaults->ToJsonString(serializeOptions);
  }

  JsonNode ^ userNode = JsonNode::Parse(userJson);
  JsonNode ^ merged = MergeJsonNode(defaults, userNode);
  auto serializeOptions = gcnew JsonSerializerOptions();
  serializeOptions->WriteIndented = true;
  return merged->ToJsonString(serializeOptions);
}

RootConfig ^ ParseConfig(String ^ json) {
  auto options = gcnew JsonSerializerOptions();
  options->PropertyNameCaseInsensitive = true;
  RootConfig ^ parsed = JsonSerializer::Deserialize<RootConfig ^>(json, options);
  if (parsed == nullptr)
    parsed = gcnew RootConfig();
  return parsed;
}

String ^ Coalesce(String ^ value, String ^ fallback) {
  return String::IsNullOrWhiteSpace(value) ? fallback : value;
}

bool InitializeCoreFromConfig(RootConfig ^ cfg) {
  auto ring = cfg->RingBuffer;
  auto flush = cfg->Flush;
  auto sinks = cfg->Sinks;
  auto file = sinks == nullptr ? nullptr : sinks->File;
  auto debugOut = sinks == nullptr ? nullptr : sinks->OutputDebugString;
  auto etw = sinks == nullptr ? nullptr : sinks->Etw;

  String ^ filePath =
      file == nullptr ? "logs/fastdrawingvisual.log"
                      : Coalesce(file->Path, "logs/fastdrawingvisual.log");

  FDVLOG_Config nativeCfg{};
  nativeCfg.ringBufferCapacity = ring == nullptr ? 8192 : Math::Max(ring->Capacity, 128);
  nativeCfg.flushIntervalMs = flush == nullptr ? 200 : Math::Max(flush->IntervalMs, 50);
  nativeCfg.enableFileSink = file == nullptr ? true : file->Enabled;
  nativeCfg.fileMaxBytes =
      file == nullptr ? (30 * 1024 * 1024) : Math::Max(file->MaxFileSizeMB, 1) * 1024 * 1024;
  nativeCfg.fileMaxFiles = file == nullptr ? 10 : Math::Max(file->MaxFiles, 1);
  nativeCfg.enableDebugOutput = debugOut == nullptr ? true : debugOut->Enabled;
  nativeCfg.enableEtw = etw == nullptr ? true : etw->Enabled;

  pin_ptr<const wchar_t> pinnedPath = PtrToStringChars(filePath);
  nativeCfg.filePath = pinnedPath;

  return FDVLOG_Initialize(&nativeCfg);
}

} // namespace

namespace FastDrawingVisual::Log {
bool LogProxy::Initialize() { return Initialize(nullptr); }

bool LogProxy::Initialize(String ^ userConfigPath) {
  String ^ merged = MergeConfigJson(userConfigPath);
  RootConfig ^ cfg = ParseConfig(merged);
  bool ok = InitializeCoreFromConfig(cfg);
  if (ok) {
    ProxyState::EffectiveConfigJson = merged;
  }
  return ok;
}

void LogProxy::Shutdown(int flushTimeoutMs) {
  FDVLOG_Shutdown(Math::Max(flushTimeoutMs, 0));
}

void LogProxy::Flush(int flushTimeoutMs) {
  FDVLOG_Flush(Math::Max(flushTimeoutMs, 0));
}

void LogProxy::Log(LogLevel level, String ^ category, String ^ message) {
  Log(level, category, message, false);
}

void LogProxy::Log(LogLevel level, String ^ category, String ^ message,
                   bool isDirect) {
  category = Coalesce(category, "managed");
  message = Coalesce(message, String::Empty);

  pin_ptr<const wchar_t> c = PtrToStringChars(category);
  pin_ptr<const wchar_t> m = PtrToStringChars(message);
  FDVLOG_Log(static_cast<int>(level), c, m, isDirect);
}

void LogProxy::WriteETW(LogLevel level, String ^ category, String ^ message) {
  WriteETW(level, category, message, false);
}

void LogProxy::WriteETW(LogLevel level, String ^ category, String ^ message,
                        bool isDirect) {
  category = Coalesce(category, "managed");
  message = Coalesce(message, String::Empty);

  pin_ptr<const wchar_t> c = PtrToStringChars(category);
  pin_ptr<const wchar_t> m = PtrToStringChars(message);
  FDVLOG_WriteETW(static_cast<int>(level), c, m, isDirect);
}

int LogProxy::RegisterMetric(String ^ name, UInt32 periodSec, String ^ format,
                             LogLevel level) {
  name = Coalesce(name, String::Empty);
  format = Coalesce(format, String::Empty);

  pin_ptr<const wchar_t> n = PtrToStringChars(name);
  pin_ptr<const wchar_t> f = PtrToStringChars(format);

  FDVLOG_MetricSpec spec{};
  spec.name = name->Length > 0 ? n : nullptr;
  spec.periodSec = Math::Max(periodSec, 1U);
  spec.format = format->Length > 0 ? f : nullptr;
  spec.level = static_cast<int>(level);
  return FDVLOG_RegisterMetric(&spec);
}

bool LogProxy::UnregisterMetric(int metricId) {
  return FDVLOG_UnregisterMetric(metricId);
}

void LogProxy::LogMetric(int metricId, double value) {
  FDVLOG_LogMetric(metricId, value);
}

UInt64 LogProxy::GetDroppedTotal() { return FDVLOG_GetDroppedTotal(); }

String ^ LogProxy::GetEffectiveConfigJson() {
  return ProxyState::EffectiveConfigJson == nullptr
             ? DefaultConfigJson()
             : ProxyState::EffectiveConfigJson;
}
} // namespace FastDrawingVisual::Log
