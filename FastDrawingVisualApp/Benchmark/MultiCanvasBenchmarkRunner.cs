using FastDrawingVisualApp.Benchmark.Scenarios;
using System;
using System.Collections.Generic;

namespace FastDrawingVisualApp.Benchmark
{
    internal sealed class MultiCanvasBenchmarkRunner : IBenchmarkRunController
    {
        private readonly BenchmarkConfig _config;
        private readonly BenchmarkRunner[] _runners;
        private bool _isDisposed;

        public MultiCanvasBenchmarkRunner(
            IReadOnlyList<FastDrawingVisual.Controls.FastDrawingVisual> canvases,
            BenchmarkConfig config)
        {
            if (canvases == null)
                throw new ArgumentNullException(nameof(canvases));
            if (config == null)
                throw new ArgumentNullException(nameof(config));
            if (canvases.Count == 0)
                throw new ArgumentException("At least one canvas is required.", nameof(canvases));

            _config = config;
            _runners = new BenchmarkRunner[canvases.Count];

            for (int i = 0; i < canvases.Count; i++)
            {
                var childConfig = new BenchmarkConfig(
                    config.Renderer,
                    config.Scenario,
                    config.Scale,
                    config.Pacing,
                    config.Duration,
                    config.Seed + (i * 7_919));
                _runners[i] = new BenchmarkRunner(canvases[i], childConfig);
            }
        }

        public BenchmarkConfig Config => _config;

        public string ScenarioSummary
            => $"{_runners.Length} tiles | {_runners[0].ScenarioSummary}";

        public bool IsRunning
        {
            get
            {
                if (_isDisposed)
                    return false;

                for (int i = 0; i < _runners.Length; i++)
                {
                    if (_runners[i].IsRunning)
                        return true;
                }

                return false;
            }
        }

        public BenchmarkMetricsSnapshot CreateSnapshot()
        {
            var snapshots = new BenchmarkMetricsSnapshot[_runners.Length];
            for (int i = 0; i < _runners.Length; i++)
                snapshots[i] = _runners[i].CreateSnapshot();

            return Aggregate(snapshots);
        }

        public void RequestManualStop()
        {
            for (int i = 0; i < _runners.Length; i++)
                _runners[i].RequestManualStop();
        }

        public void Dispose()
        {
            if (_isDisposed)
                return;

            _isDisposed = true;

            for (int i = 0; i < _runners.Length; i++)
                _runners[i].Dispose();
        }

        private static BenchmarkMetricsSnapshot Aggregate(BenchmarkMetricsSnapshot[] snapshots)
        {
            bool isRunning = false;
            var stopReason = BenchmarkStopReason.Manual;
            TimeSpan elapsed = TimeSpan.Zero;
            long submittedTotal = 0;
            long executedTotal = 0;
            long droppedTotal = 0;
            long pendingTotal = 0;
            double submitHz = 0;
            double executeHz = 0;
            double dropRatePercent = 0;

            for (int i = 0; i < snapshots.Length; i++)
            {
                var snapshot = snapshots[i];
                isRunning |= snapshot.IsRunning;
                if (snapshot.IsRunning)
                    stopReason = BenchmarkStopReason.Running;
                else if (stopReason != BenchmarkStopReason.Running && snapshot.StopReason == BenchmarkStopReason.DurationReached)
                    stopReason = BenchmarkStopReason.DurationReached;

                if (snapshot.Elapsed > elapsed)
                    elapsed = snapshot.Elapsed;

                submittedTotal += snapshot.SubmittedTotal;
                executedTotal += snapshot.ExecutedTotal;
                droppedTotal += snapshot.DroppedTotal;
                pendingTotal += snapshot.PendingTotal;
                submitHz += snapshot.SubmitHz;
                executeHz += snapshot.ExecuteHz;
                dropRatePercent += snapshot.RecentDropRatePercent;
            }

            return new BenchmarkMetricsSnapshot(
                isRunning,
                stopReason,
                elapsed,
                submittedTotal,
                executedTotal,
                droppedTotal,
                pendingTotal,
                submitHz,
                executeHz,
                snapshots.Length > 0 ? dropRatePercent / snapshots.Length : 0,
                Merge(snapshots, static snapshot => snapshot.PrepareDuration),
                Merge(snapshots, static snapshot => snapshot.QueueDelay),
                Merge(snapshots, static snapshot => snapshot.DrawDuration),
                Merge(snapshots, static snapshot => snapshot.EndToEndLatency));
        }

        private static RollingValueStatistics Merge(
            BenchmarkMetricsSnapshot[] snapshots,
            Func<BenchmarkMetricsSnapshot, RollingValueStatistics> selector)
        {
            int count = 0;
            double averageWeighted = 0;
            double p95Weighted = 0;
            double max = 0;

            for (int i = 0; i < snapshots.Length; i++)
            {
                var stats = selector(snapshots[i]);
                count += stats.Count;
                averageWeighted += stats.Average * stats.Count;
                p95Weighted += stats.P95 * stats.Count;
                if (stats.Max > max)
                    max = stats.Max;
            }

            if (count == 0)
                return new RollingValueStatistics(0, 0, 0, 0);

            return new RollingValueStatistics(
                count,
                averageWeighted / count,
                p95Weighted / count,
                max);
        }
    }
}
