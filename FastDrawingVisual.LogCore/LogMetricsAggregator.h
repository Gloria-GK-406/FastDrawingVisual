#pragma once

#include "FdvLogCoreExports.h"
#include "LogTypes.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fdvlog {

class LogMetricsAggregator {
public:
  using EmitCallback = std::function<void(const std::wstring &line, int level)>;

  int RegisterMetric(const FDVLOG_MetricSpec *spec);
  bool UnregisterMetric(int metricId);
  void Reset();
  void OnMetricEvent(const MetricPayload &payload, uint64_t timestamp100ns,
                     const EmitCallback &emit);
  void OnHeartbeat(uint64_t timestamp100ns, const EmitCallback &emit);

private:
  enum class FormatTokenType {
    Literal,
    Id,
    Name,
    WindowMs,
    Count,
    Avg,
    Min,
    Max
  };

  struct FormatPiece {
    FormatTokenType token = FormatTokenType::Literal;
    std::wstring literal;
  };

  struct MetricDefinition {
    int id = 0;
    std::wstring name;
    uint32_t windowMs = 1000;
    int aggregation = FDVLOG_AggregationCount;
    std::wstring idText;
    std::wstring windowMsText;
    std::vector<FormatPiece> formatPieces;
    size_t staticChars = 0;
    int level = FDVLOG_LevelInfo;
  };

  struct MetricState {
    uint64_t windowStart100ns = 0;
    uint64_t windowEnd100ns = 0;
    double sum = 0.0;
    double minValue = 0.0;
    double maxValue = 0.0;
    uint64_t sampleCount = 0;
    bool hasSamples = false;
  };

  struct MetricEntry {
    mutable std::mutex mutex;
    MetricDefinition definition;
    MetricState state;
  };

  static uint64_t WindowDuration100ns(uint32_t windowMs);
  static int NormalizeAggregation(int aggregation);
  static int NormalizeLevel(int level);
  static void ResetBucket(MetricState &state);
  static void InitializeWindow(MetricState &state, uint32_t windowMs,
                               uint64_t timestamp100ns);
  static void AccumulateMetric(MetricState &state, double value);
  static std::wstring FormatNumber(double value);
  static std::vector<FormatPiece>
  CompileFormatPieces(const std::wstring &format);
  static bool TryParseToken(const std::wstring &tokenName,
                            FormatTokenType *tokenType);
  static std::wstring BuildFormattedLine(const MetricDefinition &definition,
                                         uint64_t sampleCount, double avg,
                                         double minValue, double maxValue);
  static void EmitCurrentBucketIfAny(MetricEntry &entry,
                                     const EmitCallback &emit);
  static void AdvanceMetricWindow(MetricEntry &entry, uint64_t timestamp100ns,
                                  const EmitCallback &emit);
  std::shared_ptr<MetricEntry> FindEntry(int metricId);

  std::mutex metricsMutex_;
  int nextMetricId_ = 1;
  std::unordered_map<int, std::shared_ptr<MetricEntry>> metrics_;
};

} // namespace fdvlog
