#include "LogMetricsAggregator.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <utility>

namespace fdvlog {
namespace {

constexpr const wchar_t *kDefaultMetricFormat =
    L"id={id} name={name} windowMs={windowMs} "
    L"count={count} avg={avg} min={min} max={max}";

static bool IsNullOrEmpty(const wchar_t *text) {
  return text == nullptr || text[0] == L'\0';
}

} // namespace

int LogMetricsAggregator::RegisterMetric(const FDVLOG_MetricSpec *spec) {
  if (!spec)
    return 0;

  int metricId = 0;
  {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    metricId = nextMetricId_++;
  }

  MetricDefinition definition{};
  definition.id = metricId;
  definition.windowMs = std::max<uint32_t>(spec->windowMs, 1000);
  definition.aggregation = NormalizeAggregation(spec->aggregation);
  definition.level = NormalizeLevel(spec->level);

  if (!IsNullOrEmpty(spec->name)) {
    definition.name = spec->name;
  } else {
    definition.name = L"metric_" + std::to_wstring(definition.id);
  }

  definition.idText = std::to_wstring(definition.id);
  definition.windowMsText = std::to_wstring(definition.windowMs);

  const std::wstring format =
      IsNullOrEmpty(spec->format) ? std::wstring(kDefaultMetricFormat)
                                  : std::wstring(spec->format);
  definition.formatPieces = CompileFormatPieces(format);

  for (const auto &piece : definition.formatPieces) {
    switch (piece.token) {
    case FormatTokenType::Literal:
      definition.staticChars += piece.literal.size();
      break;
    case FormatTokenType::Id:
      definition.staticChars += definition.idText.size();
      break;
    case FormatTokenType::Name:
      definition.staticChars += definition.name.size();
      break;
    case FormatTokenType::WindowMs:
      definition.staticChars += definition.windowMsText.size();
      break;
    default:
      break;
    }
  }

  auto entry = std::make_shared<MetricEntry>();
  entry->definition = std::move(definition);
  {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    metrics_.emplace(metricId, entry);
  }
  return metricId;
}

bool LogMetricsAggregator::UnregisterMetric(int metricId) {
  std::lock_guard<std::mutex> lock(metricsMutex_);
  return metrics_.erase(metricId) > 0;
}

void LogMetricsAggregator::Reset() {
  std::lock_guard<std::mutex> lock(metricsMutex_);
  metrics_.clear();
  nextMetricId_ = 1;
}

std::shared_ptr<LogMetricsAggregator::MetricEntry>
LogMetricsAggregator::FindEntry(int metricId) {
  std::lock_guard<std::mutex> lock(metricsMutex_);
  const auto it = metrics_.find(metricId);
  if (it == metrics_.end())
    return {};
  return it->second;
}

void LogMetricsAggregator::OnMetricEvent(const MetricPayload &payload,
                                         uint64_t timestamp100ns,
                                         const EmitCallback &emit) {
  auto entry = FindEntry(payload.metricId);
  if (!entry)
    return;

  std::lock_guard<std::mutex> lock(entry->mutex);
  if (entry->state.windowEnd100ns == 0) {
    InitializeWindow(entry->state, entry->definition.windowMs, timestamp100ns);
  }

  AdvanceMetricWindow(*entry, timestamp100ns, emit);
  AccumulateMetric(entry->state, payload.value);
}

void LogMetricsAggregator::OnHeartbeat(uint64_t timestamp100ns,
                                       const EmitCallback &emit) {
  std::vector<std::shared_ptr<MetricEntry>> snapshot;
  {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    snapshot.reserve(metrics_.size());
    for (const auto &item : metrics_) {
      snapshot.push_back(item.second);
    }
  }

  for (const auto &entry : snapshot) {
    std::lock_guard<std::mutex> entryLock(entry->mutex);
    if (entry->state.windowEnd100ns == 0)
      continue;
    AdvanceMetricWindow(*entry, timestamp100ns, emit);
  }
}

uint64_t LogMetricsAggregator::WindowDuration100ns(uint32_t windowMs) {
  const uint64_t safeWindowMs = std::max<uint32_t>(windowMs, 1);
  return safeWindowMs * 10000ULL;
}

int LogMetricsAggregator::NormalizeAggregation(int aggregation) {
  switch (aggregation) {
  case FDVLOG_AggregationCount:
  case FDVLOG_AggregationAverage:
  case FDVLOG_AggregationSum:
  case FDVLOG_AggregationMin:
  case FDVLOG_AggregationMax:
    return aggregation;
  default:
    return FDVLOG_AggregationAverage;
  }
}

int LogMetricsAggregator::NormalizeLevel(int level) {
  switch (level) {
  case FDVLOG_LevelTrace:
  case FDVLOG_LevelDebug:
  case FDVLOG_LevelInfo:
  case FDVLOG_LevelWarn:
  case FDVLOG_LevelError:
  case FDVLOG_LevelFatal:
    return level;
  default:
    return FDVLOG_LevelInfo;
  }
}

void LogMetricsAggregator::ResetBucket(MetricState &state) {
  state.sum = 0.0;
  state.minValue = 0.0;
  state.maxValue = 0.0;
  state.sampleCount = 0;
  state.hasSamples = false;
}

void LogMetricsAggregator::InitializeWindow(MetricState &state, uint32_t windowMs,
                                            uint64_t timestamp100ns) {
  const uint64_t duration = WindowDuration100ns(windowMs);
  const uint64_t start = (timestamp100ns / duration) * duration;
  state.windowStart100ns = start;
  state.windowEnd100ns = start + duration;
  ResetBucket(state);
}

void LogMetricsAggregator::AccumulateMetric(MetricState &state, double value) {
  if (!state.hasSamples) {
    state.sum = value;
    state.minValue = value;
    state.maxValue = value;
    state.sampleCount = 1;
    state.hasSamples = true;
    return;
  }

  state.sum += value;
  state.minValue = std::min(state.minValue, value);
  state.maxValue = std::max(state.maxValue, value);
  ++state.sampleCount;
}

std::wstring LogMetricsAggregator::FormatNumber(double value) {
  if (!std::isfinite(value))
    return L"nan";

  wchar_t buffer[64]{};
  swprintf_s(buffer, L"%.6f", value);
  std::wstring out = buffer;

  while (!out.empty() && out.back() == L'0') {
    out.pop_back();
  }
  if (!out.empty() && out.back() == L'.') {
    out.pop_back();
  }
  if (out.empty())
    out = L"0";
  return out;
}

std::vector<LogMetricsAggregator::FormatPiece>
LogMetricsAggregator::CompileFormatPieces(const std::wstring &format) {
  std::vector<FormatPiece> pieces;

  auto appendLiteral = [&pieces](const std::wstring &text) {
    if (text.empty())
      return;

    if (!pieces.empty() && pieces.back().token == FormatTokenType::Literal) {
      pieces.back().literal += text;
      return;
    }

    FormatPiece piece{};
    piece.token = FormatTokenType::Literal;
    piece.literal = text;
    pieces.push_back(std::move(piece));
  };

  size_t cursor = 0;
  while (cursor < format.size()) {
    const size_t open = format.find(L'{', cursor);
    if (open == std::wstring::npos) {
      appendLiteral(format.substr(cursor));
      break;
    }

    appendLiteral(format.substr(cursor, open - cursor));

    const size_t close = format.find(L'}', open + 1);
    if (close == std::wstring::npos) {
      appendLiteral(format.substr(open));
      break;
    }

    const std::wstring tokenText = format.substr(open + 1, close - open - 1);
    FormatTokenType tokenType{};
    if (TryParseToken(tokenText, &tokenType)) {
      FormatPiece piece{};
      piece.token = tokenType;
      pieces.push_back(std::move(piece));
    } else {
      appendLiteral(format.substr(open, close - open + 1));
    }

    cursor = close + 1;
  }

  if (pieces.empty()) {
    FormatPiece piece{};
    piece.token = FormatTokenType::Literal;
    pieces.push_back(std::move(piece));
  }

  return pieces;
}

bool LogMetricsAggregator::TryParseToken(const std::wstring &tokenName,
                                         FormatTokenType *tokenType) {
  if (!tokenType)
    return false;

  if (tokenName == L"id") {
    *tokenType = FormatTokenType::Id;
    return true;
  }
  if (tokenName == L"name") {
    *tokenType = FormatTokenType::Name;
    return true;
  }
  if (tokenName == L"windowMs") {
    *tokenType = FormatTokenType::WindowMs;
    return true;
  }
  if (tokenName == L"count" || tokenName == L"samples") {
    *tokenType = FormatTokenType::Count;
    return true;
  }
  if (tokenName == L"avg") {
    *tokenType = FormatTokenType::Avg;
    return true;
  }
  if (tokenName == L"min") {
    *tokenType = FormatTokenType::Min;
    return true;
  }
  if (tokenName == L"max") {
    *tokenType = FormatTokenType::Max;
    return true;
  }

  return false;
}

std::wstring
LogMetricsAggregator::BuildFormattedLine(const MetricDefinition &definition,
                                         uint64_t sampleCount, double avg,
                                         double minValue, double maxValue) {
  const std::wstring countText = std::to_wstring(sampleCount);
  const std::wstring avgText = FormatNumber(avg);
  const std::wstring minText = FormatNumber(minValue);
  const std::wstring maxText = FormatNumber(maxValue);

  std::wstring line;
  line.reserve(definition.staticChars + countText.size() + avgText.size() +
               minText.size() + maxText.size());

  for (const auto &piece : definition.formatPieces) {
    switch (piece.token) {
    case FormatTokenType::Literal:
      line += piece.literal;
      break;
    case FormatTokenType::Id:
      line += definition.idText;
      break;
    case FormatTokenType::Name:
      line += definition.name;
      break;
    case FormatTokenType::WindowMs:
      line += definition.windowMsText;
      break;
    case FormatTokenType::Count:
      line += countText;
      break;
    case FormatTokenType::Avg:
      line += avgText;
      break;
    case FormatTokenType::Min:
      line += minText;
      break;
    case FormatTokenType::Max:
      line += maxText;
      break;
    }
  }

  return line;
}

void LogMetricsAggregator::EmitCurrentBucketIfAny(MetricEntry &entry,
                                                  const EmitCallback &emit) {
  if (!entry.state.hasSamples || entry.state.sampleCount == 0)
    return;

  const double avg =
      entry.state.sum / static_cast<double>(entry.state.sampleCount);
  emit(BuildFormattedLine(entry.definition, entry.state.sampleCount, avg,
                          entry.state.minValue, entry.state.maxValue),
       entry.definition.level);
}

void LogMetricsAggregator::AdvanceMetricWindow(MetricEntry &entry,
                                               uint64_t timestamp100ns,
                                               const EmitCallback &emit) {
  const uint64_t duration = WindowDuration100ns(entry.definition.windowMs);
  if (duration == 0)
    return;

  while (timestamp100ns >= entry.state.windowEnd100ns) {
    EmitCurrentBucketIfAny(entry, emit);
    entry.state.windowStart100ns = entry.state.windowEnd100ns;
    entry.state.windowEnd100ns += duration;
    ResetBucket(entry.state);

    if (timestamp100ns >= entry.state.windowEnd100ns) {
      const uint64_t windowsToSkip =
          (timestamp100ns - entry.state.windowEnd100ns) / duration;
      entry.state.windowStart100ns += windowsToSkip * duration;
      entry.state.windowEnd100ns += windowsToSkip * duration;
    }
  }
}

} // namespace fdvlog
