using System;
using System.Diagnostics;

namespace FastDrawingVisualApp.Benchmark
{
    internal enum BenchmarkStopReason
    {
        Running = 0,
        Manual = 1,
        DurationReached = 2,
    }

    internal sealed class BenchmarkMetricsCollector
    {
        private readonly object _gate = new();
        private readonly RollingValueWindow _prepareMs = new(1_024);
        private readonly RollingValueWindow _queueDelayMs = new(1_024);
        private readonly RollingValueWindow _drawMs = new(1_024);
        private readonly RollingValueWindow _endToEndMs = new(1_024);

        private long _submitted;
        private long _executed;
        private long _dropped;
        private long _lastSnapshotTicks;
        private long _lastSnapshotSubmitted;
        private long _lastSnapshotExecuted;
        private long _lastSnapshotDropped;

        public void RecordSubmission()
        {
            lock (_gate)
            {
                _submitted++;
            }
        }

        public void RecordPreparation(long startedTicks, long completedTicks)
        {
            double prepareMs = ToMilliseconds(completedTicks - startedTicks);

            lock (_gate)
            {
                _prepareMs.Add(prepareMs);
            }
        }

        public void RecordExecution(long droppedSincePrevious, long submitTicks, long startedTicks, long completedTicks)
        {
            double queueDelayMs = ToMilliseconds(startedTicks - submitTicks);
            double drawMs = ToMilliseconds(completedTicks - startedTicks);
            double endToEndMs = ToMilliseconds(completedTicks - submitTicks);

            lock (_gate)
            {
                _executed++;
                _dropped += Math.Max(0, droppedSincePrevious);
                _queueDelayMs.Add(queueDelayMs);
                _drawMs.Add(drawMs);
                _endToEndMs.Add(endToEndMs);
            }
        }

        public BenchmarkMetricsSnapshot CreateSnapshot(bool isRunning, BenchmarkStopReason stopReason, TimeSpan elapsed)
        {
            lock (_gate)
            {
                long now = Stopwatch.GetTimestamp();
                if (_lastSnapshotTicks == 0)
                {
                    _lastSnapshotTicks = now;
                    _lastSnapshotSubmitted = _submitted;
                    _lastSnapshotExecuted = _executed;
                    _lastSnapshotDropped = _dropped;
                }

                double elapsedSec = Math.Max(0.0001, ToSeconds(now - _lastSnapshotTicks));
                long submittedDelta = _submitted - _lastSnapshotSubmitted;
                long executedDelta = _executed - _lastSnapshotExecuted;
                long droppedDelta = _dropped - _lastSnapshotDropped;
                long pending = Math.Max(0, _submitted - _executed - _dropped);

                var snapshot = new BenchmarkMetricsSnapshot(
                    isRunning,
                    stopReason,
                    elapsed,
                    _submitted,
                    _executed,
                    _dropped,
                    pending,
                    submittedDelta / elapsedSec,
                    executedDelta / elapsedSec,
                    submittedDelta > 0 ? droppedDelta * 100d / submittedDelta : 0,
                    _prepareMs.CreateSnapshot(),
                    _queueDelayMs.CreateSnapshot(),
                    _drawMs.CreateSnapshot(),
                    _endToEndMs.CreateSnapshot());

                _lastSnapshotTicks = now;
                _lastSnapshotSubmitted = _submitted;
                _lastSnapshotExecuted = _executed;
                _lastSnapshotDropped = _dropped;
                return snapshot;
            }
        }

        private static double ToMilliseconds(long ticks) => ticks * 1000d / Stopwatch.Frequency;

        private static double ToSeconds(long ticks) => ticks / (double)Stopwatch.Frequency;
    }

    internal sealed class BenchmarkMetricsSnapshot
    {
        public BenchmarkMetricsSnapshot(
            bool isRunning,
            BenchmarkStopReason stopReason,
            TimeSpan elapsed,
            long submittedTotal,
            long executedTotal,
            long droppedTotal,
            long pendingTotal,
            double submitHz,
            double executeHz,
            double recentDropRatePercent,
            RollingValueStatistics prepareDuration,
            RollingValueStatistics queueDelay,
            RollingValueStatistics drawDuration,
            RollingValueStatistics endToEndLatency)
        {
            IsRunning = isRunning;
            StopReason = stopReason;
            Elapsed = elapsed;
            SubmittedTotal = submittedTotal;
            ExecutedTotal = executedTotal;
            DroppedTotal = droppedTotal;
            PendingTotal = pendingTotal;
            SubmitHz = submitHz;
            ExecuteHz = executeHz;
            RecentDropRatePercent = recentDropRatePercent;
            PrepareDuration = prepareDuration;
            QueueDelay = queueDelay;
            DrawDuration = drawDuration;
            EndToEndLatency = endToEndLatency;
        }

        public bool IsRunning { get; }

        public BenchmarkStopReason StopReason { get; }

        public TimeSpan Elapsed { get; }

        public long SubmittedTotal { get; }

        public long ExecutedTotal { get; }

        public long DroppedTotal { get; }

        public long PendingTotal { get; }

        public double SubmitHz { get; }

        public double ExecuteHz { get; }

        public double RecentDropRatePercent { get; }

        public RollingValueStatistics PrepareDuration { get; }

        public RollingValueStatistics QueueDelay { get; }

        public RollingValueStatistics DrawDuration { get; }

        public RollingValueStatistics EndToEndLatency { get; }
    }

    internal readonly struct RollingValueStatistics
    {
        public RollingValueStatistics(int count, double average, double p95, double max)
        {
            Count = count;
            Average = average;
            P95 = p95;
            Max = max;
        }

        public int Count { get; }

        public double Average { get; }

        public double P95 { get; }

        public double Max { get; }
    }

    internal sealed class RollingValueWindow
    {
        private readonly double[] _values;
        private int _count;
        private int _nextIndex;
        private double _sum;

        public RollingValueWindow(int capacity)
        {
            _values = new double[capacity];
        }

        public void Add(double value)
        {
            if (_count == _values.Length)
                _sum -= _values[_nextIndex];
            else
                _count++;

            _values[_nextIndex] = value;
            _sum += value;
            _nextIndex = (_nextIndex + 1) % _values.Length;
        }

        public RollingValueStatistics CreateSnapshot()
        {
            if (_count == 0)
                return new RollingValueStatistics(0, 0, 0, 0);

            var copy = new double[_count];
            Array.Copy(_values, copy, _count);
            Array.Sort(copy);

            int p95Index = (int)Math.Ceiling((_count - 1) * 0.95);
            return new RollingValueStatistics(
                _count,
                _sum / _count,
                copy[p95Index],
                copy[_count - 1]);
        }
    }
}
