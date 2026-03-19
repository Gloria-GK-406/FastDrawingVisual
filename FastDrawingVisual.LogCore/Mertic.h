#pragma once
#include "FdvLogCoreExports.h"
#include <utility>
#include "LogTypes.h"

namespace fdvlog
{

    class Metric final {
    public:
        Metric() noexcept = default;

        explicit Metric(int metricId) noexcept : metricId_(metricId) {}

        Metric(const Metric&) = delete;
        Metric& operator=(const Metric&) = delete;

        Metric(Metric&& other) noexcept
            : metricId_(std::exchange(other.metricId_, 0)) {
        }

        Metric& operator=(Metric&& other) noexcept {
            if (this != &other) {
                Reset();
                metricId_ = std::exchange(other.metricId_, 0);
            }

            return *this;
        }

        ~Metric() { Reset(); }

        [[nodiscard]] static Metric Register(const ::MetricSpec& spec) noexcept {
            return Metric(FDVLOG_RegisterMetric(&spec));
        }

        [[nodiscard]] static Metric Create(const wchar_t* name, uint32_t periodSec = 1,
            const wchar_t* format = nullptr,
            int level = FDVLOG_LevelInfo) noexcept {
            ::MetricSpec spec{};
            spec.name = name;
            spec.periodSec = periodSec;
            spec.format = format;
            spec.level = level;
            return Register(spec);
        }

        void Add(double value) const noexcept {
            if (metricId_ > 0) {
                FDVLOG_LogMetric(metricId_, value);
            }
        }

        void Add(float value) const noexcept { Add(static_cast<double>(value)); }

        [[nodiscard]] bool IsValid() const noexcept { return metricId_ > 0; }

        [[nodiscard]] int Id() const noexcept { return metricId_; }

        [[nodiscard]] int Release() noexcept { return std::exchange(metricId_, 0); }

        void Reset() noexcept {
            const int metricId = Release();
            if (metricId > 0 && FDVLOG_IsInitialized()) {
                FDVLOG_UnregisterMetric(metricId);
            }
        }

    private:
        int metricId_ = 0;
    };
}

