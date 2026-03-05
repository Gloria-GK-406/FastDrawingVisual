#include "LogMetricsAggregator.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cwchar>

namespace fdvlog {

void LogMetricsAggregator::Reset() { metrics_.clear(); }

void LogMetricsAggregator::OnMetricEvent(const MetricPayload &payload,
                                         uint64_t timestamp100ns,
                                         const EmitCallback &emit) {
  auto &state = metrics_[payload.metricId];

  if (state.windowEnd100ns == 0 || state.windowMs != payload.windowMs ||
      state.aggregation != payload.aggregation) {
    InitializeMetricState(state, payload.windowMs, payload.aggregation,
                          timestamp100ns);
  }

  AdvanceMetricWindow(state, payload.metricId, timestamp100ns, emit);
  AccumulateMetric(state, payload.value);
}

void LogMetricsAggregator::OnHeartbeat(uint64_t timestamp100ns,
                                       const EmitCallback &emit) {
  for (auto &entry : metrics_) {
    AdvanceMetricWindow(entry.second, entry.first, timestamp100ns, emit);
  }
}

uint64_t LogMetricsAggregator::WindowDuration100ns(uint32_t windowMs) {
  const uint64_t safeWindowMs = std::max<uint32_t>(windowMs, 1);
  return safeWindowMs * 10000ULL;
}

const wchar_t *LogMetricsAggregator::AggregationName(int aggregation) {
  switch (aggregation) {
  case FDVLOG_AggregationRate:
    return L"rate";
  case FDVLOG_AggregationAverage:
    return L"avg";
  case FDVLOG_AggregationSum:
    return L"sum";
  case FDVLOG_AggregationMin:
    return L"min";
  case FDVLOG_AggregationMax:
    return L"max";
  default:
    return L"rate";
  }
}

void LogMetricsAggregator::InitializeMetricState(MetricState &state,
                                                 uint32_t windowMs,
                                                 int aggregation,
                                                 uint64_t timestamp100ns) {
  const uint64_t duration = WindowDuration100ns(windowMs);
  const uint64_t start = (timestamp100ns / duration) * duration;

  state.windowMs = std::max<uint32_t>(windowMs, 1);
  state.aggregation = aggregation;
  state.windowStart100ns = start;
  state.windowEnd100ns = start + duration;
  state.sum = 0;
  state.minValue = 0;
  state.maxValue = 0;
  state.sampleCount = 0;
  state.hasSamples = false;
  state.emptyEmittedSinceLastSample = false;
}

void LogMetricsAggregator::AccumulateMetric(MetricState &state, int64_t value) {
  if (!state.hasSamples) {
    state.sum = value;
    state.minValue = value;
    state.maxValue = value;
    state.sampleCount = 1;
    state.hasSamples = true;
    state.emptyEmittedSinceLastSample = false;
    return;
  }

  state.sum += value;
  state.minValue = std::min(state.minValue, value);
  state.maxValue = std::max(state.maxValue, value);
  ++state.sampleCount;
}

void LogMetricsAggregator::AdvanceMetricWindow(MetricState &state,
                                               uint32_t metricId,
                                               uint64_t timestamp100ns,
                                               const EmitCallback &emit) {
  const uint64_t duration = WindowDuration100ns(state.windowMs);
  if (duration == 0)
    return;

  while (timestamp100ns >= state.windowEnd100ns) {
    EmitMetricWindow(metricId, state, emit);
    state.windowStart100ns = state.windowEnd100ns;
    state.windowEnd100ns += duration;
    state.sum = 0;
    state.sampleCount = 0;
    state.hasSamples = false;
    state.minValue = 0;
    state.maxValue = 0;

    if (!state.hasSamples && state.emptyEmittedSinceLastSample &&
        timestamp100ns >= state.windowEnd100ns) {
      const uint64_t windowsToSkip =
          (timestamp100ns - state.windowEnd100ns) / duration;
      state.windowStart100ns += windowsToSkip * duration;
      state.windowEnd100ns += windowsToSkip * duration;
    }
  }
}

void LogMetricsAggregator::EmitMetricWindow(uint32_t metricId,
                                            MetricState &state,
                                            const EmitCallback &emit) {
  bool hasValue = false;
  double value = 0.0;
  if (state.hasSamples) {
    switch (state.aggregation) {
    case FDVLOG_AggregationRate:
      value = static_cast<double>(state.sampleCount);
      hasValue = true;
      break;
    case FDVLOG_AggregationAverage:
      value = static_cast<double>(state.sum) /
              static_cast<double>(state.sampleCount);
      hasValue = true;
      break;
    case FDVLOG_AggregationSum:
      value = static_cast<double>(state.sum);
      hasValue = true;
      break;
    case FDVLOG_AggregationMin:
      value = static_cast<double>(state.minValue);
      hasValue = true;
      break;
    case FDVLOG_AggregationMax:
      value = static_cast<double>(state.maxValue);
      hasValue = true;
      break;
    default:
      break;
    }
    state.emptyEmittedSinceLastSample = false;
  } else {
    if (state.emptyEmittedSinceLastSample)
      return;

    if (state.aggregation == FDVLOG_AggregationRate ||
        state.aggregation == FDVLOG_AggregationSum) {
      value = 0.0;
      hasValue = true;
    }
    state.emptyEmittedSinceLastSample = true;
  }

  wchar_t lineBuffer[512]{};
  if (hasValue) {
    swprintf_s(lineBuffer,
               L"[metric] id=%u windowMs=%u agg=%s value=%.3f samples=%llu",
               metricId, state.windowMs, AggregationName(state.aggregation),
               value, static_cast<unsigned long long>(state.sampleCount));
  } else {
    swprintf_s(lineBuffer, L"[metric] id=%u windowMs=%u agg=%s value=null samples=0",
               metricId, state.windowMs, AggregationName(state.aggregation));
  }

  emit(lineBuffer, FDVLOG_LevelInfo);
}

} // namespace fdvlog
