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
  void OnHeartbeat(const EmitCallback &emit);

private:
  enum class FormatTokenType {
    Literal,
    Id,
    Name,
    PeriodSec,
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
    uint32_t periodSec = 1;
    int aggregation = FDVLOG_AggregationCount;
    std::wstring idText;
    std::wstring periodSecText;
    std::vector<FormatPiece> formatPieces;
    size_t staticChars = 0;
    int level = FDVLOG_LevelInfo;
  };

  struct MetricState {
    uint64_t currentPeriod = 0;
    double sum = 0.0;
    double minValue = 0.0;
    double maxValue = 0.0;
    uint64_t sampleCount = 0;
  };

  struct MetricStateSnapshot {
    uint64_t sampleCount = 0;
    double avg = 0.0;
    double minValue = 0.0;
    double maxValue = 0.0;
  };

  struct MetricEntry {
    mutable std::mutex mutex;
    MetricDefinition definition;
    MetricState state;
  };

  static int NormalizeAggregation(int aggregation);
  static int NormalizeLevel(int level);
  static void ResetBucket(MetricState &state);
  static void AccumulateMetric(MetricState &state, double value);
  static std::wstring FormatNumber(double value);
  static std::vector<FormatPiece>
  CompileFormatPieces(const std::wstring &format);
  static bool TryParseToken(const std::wstring &tokenName,
                            FormatTokenType *tokenType);
  static std::wstring BuildFormattedLine(const MetricDefinition &definition,
                                         const MetricStateSnapshot &snapshot);
  static MetricStateSnapshot BuildSnapshot(const MetricState &state);
  static void EmitCurrentBucketIfAny(MetricEntry &entry,
                                     const EmitCallback &emit);
  std::shared_ptr<MetricEntry> FindEntry(int metricId);

  std::mutex metricsMutex_;
  int nextMetricId_ = 1;
  std::unordered_map<int, std::shared_ptr<MetricEntry>> metrics_;
};

} // namespace fdvlog
