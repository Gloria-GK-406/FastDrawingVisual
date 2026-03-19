using FastDrawingVisual.Rendering;
using FastDrawingVisualApp.Benchmark.Scenarios;
using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisualApp.Benchmark
{
    internal sealed class BenchmarkRunner : IBenchmarkRunController
    {
        private readonly FastDrawingVisual.Controls.FastDrawingVisual _canvas;
        private readonly BenchmarkConfig _config;
        private readonly IBenchmarkScenarioSession _scenarioSession;
        private readonly BenchmarkMetricsCollector _metrics;
        private readonly CancellationTokenSource _cts;
        private readonly Stopwatch _elapsed;
        private readonly Task _submitLoopTask;
        private readonly IPreparedBenchmarkScenarioSession? _preparedScenarioSession;

        private long _submittedSequence;
        private long _lastExecutedSequence;
        private long _executedFrameIndex;
        private int _surfaceWidth;
        private int _surfaceHeight;
        private bool _isDisposed;
        private volatile bool _loopCompleted;
        private BenchmarkStopReason _stopReason;

        public BenchmarkRunner(FastDrawingVisual.Controls.FastDrawingVisual canvas, BenchmarkConfig config)
        {
            _canvas = canvas ?? throw new ArgumentNullException(nameof(canvas));
            _config = config ?? throw new ArgumentNullException(nameof(config));
            _scenarioSession = config.Scenario.CreateSession(config.Scale, config.Seed);
            _preparedScenarioSession = _scenarioSession as IPreparedBenchmarkScenarioSession;
            _metrics = new BenchmarkMetricsCollector();
            _cts = new CancellationTokenSource();
            _elapsed = Stopwatch.StartNew();
            _stopReason = BenchmarkStopReason.Running;
            AttachCanvasSizeTracking();
            _submitLoopTask = Task.Run(() => SubmitLoop(_cts.Token), _cts.Token);
        }

        public BenchmarkConfig Config => _config;

        public string ScenarioSummary => _scenarioSession.Summary;

        public bool IsRunning => !_isDisposed && !_cts.IsCancellationRequested && !_loopCompleted;

        public BenchmarkMetricsSnapshot CreateSnapshot() => _metrics.CreateSnapshot(IsRunning, _stopReason, _elapsed.Elapsed);

        public void RequestManualStop()
        {
            if (_stopReason == BenchmarkStopReason.Running)
                _stopReason = BenchmarkStopReason.Manual;
        }

        public void Dispose()
        {
            if (_isDisposed)
                return;

            _isDisposed = true;
            RequestManualStop();
            _cts.Cancel();

            try
            {
                _submitLoopTask.Wait(TimeSpan.FromSeconds(2));
            }
            catch (AggregateException ex) when (IsCancellationOnly(ex))
            {
            }

            _cts.Dispose();
            DetachCanvasSizeTracking();
            _scenarioSession.Dispose();
            _elapsed.Stop();
        }

        private void SubmitLoop(CancellationToken token)
        {
            double intervalTicks = _config.Pacing.TargetHz.HasValue
                ? Stopwatch.Frequency / (double)_config.Pacing.TargetHz.Value
                : 0;
            long nextTarget = Stopwatch.GetTimestamp();

            while (!token.IsCancellationRequested)
            {
                if (_config.Duration.Duration.HasValue && _elapsed.Elapsed >= _config.Duration.Duration.Value)
                {
                    _stopReason = BenchmarkStopReason.DurationReached;
                    break;
                }

                if (!_canvas.IsReady)
                {
                    Thread.Sleep(15);
                    nextTarget = Stopwatch.GetTimestamp();
                    continue;
                }

                if (intervalTicks > 0)
                {
                    WaitUntil(nextTarget, token);
                    nextTarget += Math.Max(1, (long)intervalTicks);

                    long now = Stopwatch.GetTimestamp();
                    if (nextTarget < now - intervalTicks)
                        nextTarget = now;
                }

                long sequence = Interlocked.Increment(ref _submittedSequence);
                var submitFrame = new BenchmarkFrameContext(sequence, 0, _elapsed.Elapsed);
                IPreparedBenchmarkFrame? preparedFrame = null;

                if (_preparedScenarioSession != null)
                {
                    var surface = SnapshotRenderSurface();
                    if (surface.Width <= 0 || surface.Height <= 0)
                    {
                        Thread.Sleep(15);
                        nextTarget = Stopwatch.GetTimestamp();
                        continue;
                    }

                    long prepareStartedTicks = Stopwatch.GetTimestamp();
                    preparedFrame = _preparedScenarioSession.PrepareFrame(surface, submitFrame);
                    long prepareCompletedTicks = Stopwatch.GetTimestamp();
                    _metrics.RecordPreparation(prepareStartedTicks, prepareCompletedTicks);
                }

                long submitTicks = Stopwatch.GetTimestamp();
                _metrics.RecordSubmission();

                try
                {
                    _canvas.SubmitDrawing(ctx => ExecuteFrame(ctx, sequence, submitTicks, preparedFrame));
                }
                catch (ObjectDisposedException) when (token.IsCancellationRequested || _isDisposed)
                {
                    break;
                }

                if (intervalTicks <= 0)
                    Thread.Yield();
            }

            _loopCompleted = true;
        }

        private void ExecuteFrame(IDrawingContext ctx, long sequence, long submitTicks, IPreparedBenchmarkFrame? preparedFrame)
        {
            long startedTicks = Stopwatch.GetTimestamp();
            long previousSequence = Interlocked.Exchange(ref _lastExecutedSequence, sequence);
            long droppedSincePrevious = previousSequence == 0
                ? Math.Max(0, sequence - 1)
                : Math.Max(0, sequence - previousSequence - 1);

            long executedFrameIndex = Interlocked.Increment(ref _executedFrameIndex);
            var frameContext = new BenchmarkFrameContext(sequence, executedFrameIndex, _elapsed.Elapsed);
            if (_preparedScenarioSession != null && preparedFrame != null)
                _preparedScenarioSession.RenderPreparedFrame(ctx, preparedFrame);
            else
                _scenarioSession.RenderFrame(ctx, frameContext);
            long completedTicks = Stopwatch.GetTimestamp();
            _metrics.RecordExecution(droppedSincePrevious, submitTicks, startedTicks, completedTicks);
        }

        private void AttachCanvasSizeTracking()
        {
            _canvas.Dispatcher.Invoke(() =>
            {
                UpdateSurfaceSizeOnUiThread();
                _canvas.SizeChanged += OnCanvasSizeChanged;
            });
        }

        private void DetachCanvasSizeTracking()
        {
            var dispatcher = _canvas.Dispatcher;
            if (dispatcher.HasShutdownStarted || dispatcher.HasShutdownFinished)
                return;

            dispatcher.Invoke(() => _canvas.SizeChanged -= OnCanvasSizeChanged);
        }

        private void OnCanvasSizeChanged(object sender, SizeChangedEventArgs e)
        {
            UpdateSurfaceSizeOnUiThread();
        }

        private void UpdateSurfaceSizeOnUiThread()
        {
            var dpi = VisualTreeHelper.GetDpi(_canvas);
            _surfaceWidth = (int)Math.Round(_canvas.ActualWidth * dpi.DpiScaleX);
            _surfaceHeight = (int)Math.Round(_canvas.ActualHeight * dpi.DpiScaleY);
        }

        private BenchmarkRenderSurface SnapshotRenderSurface()
        {
            return new BenchmarkRenderSurface(
                Volatile.Read(ref _surfaceWidth),
                Volatile.Read(ref _surfaceHeight));
        }

        private static void WaitUntil(long targetTicks, CancellationToken token)
        {
            while (!token.IsCancellationRequested)
            {
                long remaining = targetTicks - Stopwatch.GetTimestamp();
                if (remaining <= 0)
                    return;

                double remainingMs = remaining * 1000d / Stopwatch.Frequency;
                if (remainingMs > 2.0)
                    Thread.Sleep(1);
                else
                    Thread.SpinWait(64);
            }
        }

        private static bool IsCancellationOnly(AggregateException ex)
        {
            foreach (var inner in ex.Flatten().InnerExceptions)
            {
                if (inner is not OperationCanceledException)
                    return false;
            }

            return true;
        }
    }
}
