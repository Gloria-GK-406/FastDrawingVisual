#pragma once

#include "LogTypes.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace fdvlog {

class LogMetricsAggregator {
public:
  using EmitCallback = std::function<void(const std::wstring &line, int level)>;

  void Reset();
  void OnMetricEvent(const MetricPayload &payload, uint64_t timestamp100ns,
                     const EmitCallback &emit);
  void OnHeartbeat(uint64_t timestamp100ns, const EmitCallback &emit);

private:
  struct MetricState {
    uint32_t windowMs = 1000;
    int aggregation = FDVLOG_AggregationRate;
    uint64_t windowStart100ns = 0;
    uint64_t windowEnd100ns = 0;
    int64_t sum = 0;
    int64_t minValue = 0;
    int64_t maxValue = 0;
    uint64_t sampleCount = 0;
    bool hasSamples = false;
    bool emptyEmittedSinceLastSample = false;
  };

  static uint64_t WindowDuration100ns(uint32_t windowMs);
  static const wchar_t *AggregationName(int aggregation);
  static void InitializeMetricState(MetricState &state, uint32_t windowMs,
                                    int aggregation, uint64_t timestamp100ns);
  static void AccumulateMetric(MetricState &state, int64_t value);
  void AdvanceMetricWindow(MetricState &state, uint32_t metricId,
                           uint64_t timestamp100ns, const EmitCallback &emit);
  static void EmitMetricWindow(uint32_t metricId, MetricState &state,
                               const EmitCallback &emit);

  std::unordered_map<uint32_t, MetricState> metrics_;
};

} // namespace fdvlog
