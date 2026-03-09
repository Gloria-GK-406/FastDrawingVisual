using FastDrawingVisual.Log;
using FastDrawingVisual.Rendering;
using System;
using System.Diagnostics;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace FastDrawingVisual.WpfRenderer
{
    public sealed class WpfFallbackRenderer : IRenderer
    {
        private readonly DrawingVisual _visual;
        private readonly Dispatcher _uiDispatcher;

        private IVisualHostElement? _attachedHost;
        private volatile Action<IDrawingContext>? _pendingDrawAction;
        private int _callbackQueued;

        private int _width;
        private int _height;
        private bool _isInitialized;
        private bool _isDisposed;
        private int _drawDurationMetricId;
        private int _fpsMetricId;
        private long _lastFrameCompletedTimestamp;
        private long _submittedFrameCount;
        private static int s_nextRendererId;
        private readonly int _rendererId = Interlocked.Increment(ref s_nextRendererId);
        private const string LogCategory = "WpfFallbackRenderer";
        private const int MetricWindowSec = 1;
        private const string DrawMetricFormat = "name={name} periodSec={periodSec}s samples={count} avgMs={avg} minMs={min} maxMs={max}";
        private const string FpsMetricFormat = "name={name} periodSec={periodSec}s samples={count} avgFps={avg} minFps={min} maxFps={max}";
        private static readonly Logger s_logger = new(LogCategory);

        public WpfFallbackRenderer()
        {
            _uiDispatcher = Dispatcher.CurrentDispatcher;
            _visual = new DrawingVisual();
        }

        public bool AttachToElement(ContentControl element)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(WpfFallbackRenderer));
            if (element == null) throw new ArgumentNullException(nameof(element));

            if (element is not IVisualHostElement host)
                return false;

            if (ReferenceEquals(_attachedHost, host))
                return true;

            DetachFromHost();
            if (!host.AttachVisual(_visual))
                return false;

            _attachedHost = host;
            s_logger.Info($"attached to host element.");
            return true;
        }

        public bool Initialize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(WpfFallbackRenderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");

            s_logger.InfoEtw($"initialize start width={width} height={height}.");
            _width = width;
            _height = height;
            EnsureMetricsRegistered();
            _isInitialized = true;
            _lastFrameCompletedTimestamp = 0;
            _submittedFrameCount = 0;
            s_logger.InfoEtw($"initialize success width={width} height={height}.");
            return true;
        }

        public void Resize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(WpfFallbackRenderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");

            s_logger.InfoEtw($"resize width={width} height={height}.");
            _width = width;
            _height = height;
        }

        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(WpfFallbackRenderer));
            if (!_isInitialized) throw new InvalidOperationException("Call Initialize first.");
            if (drawAction == null) throw new ArgumentNullException(nameof(drawAction));

            Interlocked.Exchange(ref _pendingDrawAction, drawAction);

            if (Interlocked.CompareExchange(ref _callbackQueued, 1, 0) == 0)
                _uiDispatcher.InvokeAsync(ExecutePendingDraw, DispatcherPriority.Render);
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            s_logger.Info($"dispose start.");
            _isDisposed = true;
            DetachFromHost();
            UnregisterMetrics();
            s_logger.Info($"dispose complete.");
        }

        private void ExecutePendingDraw()
        {
            Interlocked.Exchange(ref _callbackQueued, 0);

            if (_isDisposed || !_isInitialized)
                return;

            var action = Interlocked.Exchange(ref _pendingDrawAction, null);
            if (action == null)
                return;

            var drawStarted = Stopwatch.GetTimestamp();
            using var dc = _visual.RenderOpen();
            using var ctx = new WpfDrawingContext(dc, _width, _height);
            action(ctx);
            RecordFrameMetrics(drawStarted);
        }

        private void DetachFromHost()
        {
            if (_attachedHost == null)
                return;

            _attachedHost.DetachVisual(_visual);
            _attachedHost = null;
        }

        private void EnsureMetricsRegistered()
        {
            if (_drawDurationMetricId <= 0)
            {
                _drawDurationMetricId = s_logger.RegisterMetric(
                    $"wpf.fallback.r{_rendererId}.draw_ms",
                    MetricWindowSec,
                    DrawMetricFormat,
                    LogLevel.Info);
            }

            if (_fpsMetricId <= 0)
            {
                _fpsMetricId = s_logger.RegisterMetric(
                    $"wpf.fallback.r{_rendererId}.fps",
                    MetricWindowSec,
                    FpsMetricFormat,
                    LogLevel.Info);
            }

            s_logger.Debug($"rid={_rendererId} metric registration drawMetricId={_drawDurationMetricId} fpsMetricId={_fpsMetricId}.");
        }

        private void UnregisterMetrics()
        {
            if (_drawDurationMetricId > 0)
                s_logger.UnregisterMetric(_drawDurationMetricId);
            if (_fpsMetricId > 0)
                s_logger.UnregisterMetric(_fpsMetricId);

            _drawDurationMetricId = 0;
            _fpsMetricId = 0;
        }

        private void RecordFrameMetrics(long drawStartedTimestamp)
        {
            _submittedFrameCount++;

            var nowTicks = Stopwatch.GetTimestamp();
            var drawDurationMs = (nowTicks - drawStartedTimestamp) * 1000d / Stopwatch.Frequency;
            if (_drawDurationMetricId > 0)
                s_logger.LogMetric(_drawDurationMetricId, drawDurationMs);

            if (_lastFrameCompletedTimestamp > 0)
            {
                var deltaTicks = nowTicks - _lastFrameCompletedTimestamp;
                if (deltaTicks > 0)
                {
                    var fps = Stopwatch.Frequency / (double)deltaTicks;
                    if (_fpsMetricId > 0)
                        s_logger.LogMetric(_fpsMetricId, fps);
                }
            }
            _lastFrameCompletedTimestamp = nowTicks;

            if (drawDurationMs >= 33d && (_submittedFrameCount % 120 == 0))
                s_logger.WarnEtw($"rid={_rendererId} slow frame drawMs={drawDurationMs:F3} frame={_submittedFrameCount} size={_width}x{_height}.");
        }
    }
}
