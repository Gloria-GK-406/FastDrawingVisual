using FastDrawingVisual.Log;
using FastDrawingVisual.Rendering;
using FastDrawingVisual.Rendering.D3D;
using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;

namespace FastDrawingVisual.SkiaSharp
{
    /// <summary>
    /// SkiaSharp renderer backed by a D3DImage-presented shared surface.
    /// Drawing submission is latest-wins and fully event-driven.
    /// </summary>
    public sealed class D3DSkiaRenderer : IRenderer
    {
        private readonly D3DImage _d3dImage;
        private readonly DrawingVisual _visual;
        private readonly D3DDeviceManager _deviceManager;
        private readonly RenderFramePool _pool;
        private readonly Dispatcher _uiDispatcher;
        private readonly AutoResetEvent _drawingSignal;
        private readonly object _workerLock = new();

        private volatile Action<IDrawingContext>? _pendingDrawAction;
        private CancellationTokenSource? _workerCts;
        private Task? _drawingWorkerTask;
        private IVisualHostElement? _attachedHost;

        private int _width;
        private int _height;
        private int _renderRequestQueued;
        private bool _isInitialized;
        private bool _isDeviceLost;
        private bool _isBackBufferBound;
        private bool _isDisposed;
        private bool _isRenderingHooked;
        private int _drawDurationMetricId;
        private int _fpsMetricId;
        private long _lastFrameCompletedTimestamp;
        private long _submittedFrameCount;
        private static int s_nextRendererId;
        private readonly int _rendererId = Interlocked.Increment(ref s_nextRendererId);

        private static readonly TimeSpan WorkerShutdownTimeout = TimeSpan.FromSeconds(2);
        private const string LogCategory = "D3DSkiaRenderer";
        private const int MetricWindowSec = 1;
        private const string DrawMetricFormat = "name={name} periodSec={periodSec}s samples={count} avgMs={avg} minMs={min} maxMs={max}";
        private const string FpsMetricFormat = "name={name} periodSec={periodSec}s samples={count} avgFps={avg} minFps={min} maxFps={max}";
        private static readonly Logger s_logger = new(LogCategory);

        public D3DSkiaRenderer()
        {
            _d3dImage = new D3DImage();
            _visual = new DrawingVisual();
            _deviceManager = new D3DDeviceManager();
            _pool = new RenderFramePool(_deviceManager, SignalDrawingWorkerIfPending);
            _uiDispatcher = Dispatcher.CurrentDispatcher;
            _drawingSignal = new AutoResetEvent(false);

            _d3dImage.IsFrontBufferAvailableChanged += OnFrontBufferAvailableChanged;
        }

        public bool AttachToElement(ContentControl element)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(D3DSkiaRenderer));
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
            if (_isDisposed) throw new ObjectDisposedException(nameof(D3DSkiaRenderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");

            s_logger.InfoEtw($"initialize start width={width} height={height}.");

            if (!_deviceManager.IsInitialized && !_deviceManager.Initialize())
            {
                s_logger.ErrorEtw($"initialize failed: device manager initialization failed.");
                return false;
            }

            _pool.CreateResources(width, height);
            _width = width;
            _height = height;
            EnsureMetricsRegistered();
            _isInitialized = true;
            _isDeviceLost = false;
            _isBackBufferBound = false;
            _renderRequestQueued = 0;
            _isRenderingHooked = false;
            _lastFrameCompletedTimestamp = 0;
            _submittedFrameCount = 0;

            BindD3DImageToVisual(width, height);
            StartDrawingWorker();
            s_logger.InfoEtw($"initialize success width={width} height={height}.");
            return true;
        }

        public void Resize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(D3DSkiaRenderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");
            if (width == _width && height == _height) return;
            if (_isDeviceLost)
            {
                _width = width;
                _height = height;
                s_logger.Warn($"resize ignored during device lost width={width} height={height}.");
                return;
            }

            s_logger.InfoEtw($"resize start width={width} height={height}.");
            RebuildResources(width, height);
        }

        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(D3DSkiaRenderer));
            if (drawAction == null) throw new ArgumentNullException(nameof(drawAction));

            Interlocked.Exchange(ref _pendingDrawAction, drawAction);
            SignalDrawingWorker();
        }

        private void BindD3DImageToVisual(int width, int height)
        {
            using var dc = _visual.RenderOpen();
            dc.DrawImage(_d3dImage, new Rect(0, 0, width, height));
        }

        private void DrawingWorkerLoop(CancellationToken token)
        {
            try
            {
                while (TryWaitForDrawingWork(token, allowImmediatePending: true))
                {
                    if (_isDeviceLost || !_isInitialized)
                        continue;

                    var action = Interlocked.Exchange(ref _pendingDrawAction, null);
                    if (action == null)
                        continue;

                    var frame = _pool.AcquireForDrawing();
                    if (frame == null)
                    {
                        Interlocked.CompareExchange(ref _pendingDrawAction, action, null);
                        if (!TryWaitForDrawingWork(token, allowImmediatePending: false))
                            break;
                        continue;
                    }

                    var drawStarted = Stopwatch.GetTimestamp();
                    try
                    {
                        using (var ctx = frame.OpenCanvas())
                        {
                            action(ctx);
                        }

                        RequestFrameSubmission();
                        RecordFrameMetrics(drawStarted);
                    }
                    catch
                    {
                        frame.TryTransitionTo(FrameState.Drawing, FrameState.Ready);
                        SignalDrawingWorkerIfPending();
                    }
                }
            }
            catch (OperationCanceledException)
            {
            }
        }

        private void OnCompositionTargetRendering(object? sender, EventArgs e)
        {
            if (!TrySubmitFrame())
                StopRenderingHook();
        }

        private bool TrySubmitFrame()
        {
            if (_isDisposed || _isDeviceLost || !_isInitialized)
                return false;

            if (!_d3dImage.TryLock(new Duration(TimeSpan.Zero)))
            {
                // Known WPF bug: TryLock failure still needs a balancing Unlock.
                _d3dImage.Unlock();
                return true;
            }

            try
            {
                if (!EnsureBackBufferBound())
                    return true;

                if (!_pool.CopyReadyToPresenting())
                    return false;

                _d3dImage.AddDirtyRect(new Int32Rect(0, 0, _width, _height));
                return false;
            }
            finally
            {
                _d3dImage.Unlock();
            }
        }

        private bool EnsureBackBufferBound()
        {
            if (_isBackBufferBound)
                return true;

            var surface = _pool.GetPresentingSurfacePointer();
            if (surface == IntPtr.Zero)
                return false;

            _d3dImage.SetBackBuffer(D3DResourceType.IDirect3DSurface9, surface);
            _isBackBufferBound = true;
            return true;
        }

        private void OnFrontBufferAvailableChanged(object sender, DependencyPropertyChangedEventArgs e)
        {
            if (!(bool)e.NewValue)
            {
                s_logger.WarnEtw($"front buffer unavailable; device lost.");
                _isDeviceLost = true;
                _uiDispatcher.BeginInvoke(new Action(() =>
                {
                    if (_isDisposed)
                        return;

                    StopRenderingHook();
                    Interlocked.Exchange(ref _renderRequestQueued, 0);
                    if (!StopDrawingWorker(WorkerShutdownTimeout))
                        return;

                    _isInitialized = false;
                    UnbindBackBuffer();
                    _pool.ReleaseResources();
                    s_logger.Info($"resources released after device lost.");
                }));
            }
            else
            {
                s_logger.InfoEtw($"front buffer available; attempting recovery.");
                _uiDispatcher.BeginInvoke(new Action(() =>
                {
                    try
                    {
                        if (_isDisposed)
                            return;

                        if (!_deviceManager.Initialize())
                        {
                            s_logger.Error($"device manager reinitialization failed.");
                            return;
                        }

                        if (_width > 0 && _height > 0)
                            RebuildResources(_width, _height);
                        s_logger.InfoEtw($"device recovery successful.");
                    }
                    catch (Exception ex)
                    {
                        s_logger.ErrorEtw($"device recovery failed. {ex}");
                        _isDeviceLost = true;
                    }
                }));
            }
        }

        private void UnbindBackBuffer()
        {
            _d3dImage.Lock();
            try
            {
                _d3dImage.SetBackBuffer(D3DResourceType.IDirect3DSurface9, IntPtr.Zero);
                _isBackBufferBound = false;
            }
            finally
            {
                _d3dImage.Unlock();
            }
        }

        private void RebuildResources(int width, int height)
        {
            s_logger.Debug($"rebuild resources start width={width} height={height}.");
            if (!StopDrawingWorker(WorkerShutdownTimeout))
            {
                s_logger.ErrorEtw($"drawing worker stop timeout during rebuild.");
                throw new TimeoutException("Drawing worker did not stop within the timeout during resize.");
            }

            StopRenderingHook();
            Interlocked.Exchange(ref _renderRequestQueued, 0);
            _isInitialized = false;
            UnbindBackBuffer();
            _pool.ReleaseResources();
            _pool.CreateResources(width, height);
            _width = width;
            _height = height;
            _isInitialized = true;
            _isDeviceLost = false;
            _isBackBufferBound = false;

            BindD3DImageToVisual(width, height);
            StartDrawingWorker();
            SignalDrawingWorkerIfPending();
            s_logger.InfoEtw($"rebuild resources success width={width} height={height}.");
        }

        private void StartDrawingWorker()
        {
            if (_isDisposed || _isDeviceLost || !_isInitialized)
                return;

            lock (_workerLock)
            {
                if (_isDisposed || _isDeviceLost || !_isInitialized)
                    return;

                if (_drawingWorkerTask is { IsCompleted: false })
                    return;

                _workerCts?.Dispose();
                _workerCts = new CancellationTokenSource();
                var token = _workerCts.Token;
                _drawingWorkerTask = Task.Run(() => DrawingWorkerLoop(token), token);
            }
        }

        private bool StopDrawingWorker(TimeSpan timeout)
        {
            Task? workerTask;
            CancellationTokenSource? workerCts;
            lock (_workerLock)
            {
                workerTask = _drawingWorkerTask;
                workerCts = _workerCts;
            }

            if (workerCts == null && workerTask == null)
                return true;

            workerCts?.Cancel();
            _drawingSignal.Set();

            if (workerTask != null)
            {
                try
                {
                    if (timeout == Timeout.InfiniteTimeSpan)
                        workerTask.Wait();
                    else if (!workerTask.Wait(timeout))
                        return false;
                }
                catch (AggregateException ex) when (IsCancellationOnly(ex))
                {
                }
            }

            lock (_workerLock)
            {
                if (ReferenceEquals(_drawingWorkerTask, workerTask))
                    _drawingWorkerTask = null;

                if (ReferenceEquals(_workerCts, workerCts))
                {
                    _workerCts?.Dispose();
                    _workerCts = null;
                }
            }

            return true;
        }

        private bool TryWaitForDrawingWork(CancellationToken token, bool allowImmediatePending)
        {
            if (token.IsCancellationRequested)
                return false;

            if (allowImmediatePending && Volatile.Read(ref _pendingDrawAction) != null)
                return true;

            var signaled = WaitHandle.WaitAny(new WaitHandle[] { token.WaitHandle, _drawingSignal });
            return signaled != 0 && !token.IsCancellationRequested;
        }

        private void SignalDrawingWorker()
        {
            if (_isDisposed)
                return;

            _drawingSignal.Set();
        }

        private void SignalDrawingWorkerIfPending()
        {
            if (Volatile.Read(ref _pendingDrawAction) != null)
                SignalDrawingWorker();
        }

        private void RequestFrameSubmission()
        {
            if (_isDisposed || _isDeviceLost || !_isInitialized)
                return;

            if (_uiDispatcher.CheckAccess())
            {
                EnsureRenderingHook();
                return;
            }

            if (Interlocked.CompareExchange(ref _renderRequestQueued, 1, 0) != 0)
                return;

            _uiDispatcher.BeginInvoke(DispatcherPriority.Render, new Action(() =>
            {
                Interlocked.Exchange(ref _renderRequestQueued, 0);
                EnsureRenderingHook();
            }));
        }

        private void EnsureRenderingHook()
        {
            if (_isDisposed || _isDeviceLost || !_isInitialized || _isRenderingHooked)
                return;

            CompositionTarget.Rendering += OnCompositionTargetRendering;
            _isRenderingHooked = true;
        }

        private void StopRenderingHook()
        {
            if (!_isRenderingHooked)
                return;

            CompositionTarget.Rendering -= OnCompositionTargetRendering;
            _isRenderingHooked = false;
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

        public void Dispose()
        {
            if (_isDisposed)
                return;

            s_logger.Info($"dispose start.");
            _isDisposed = true;

            StopRenderingHook();
            _d3dImage.IsFrontBufferAvailableChanged -= OnFrontBufferAvailableChanged;
            Interlocked.Exchange(ref _renderRequestQueued, 0);
            StopDrawingWorker(Timeout.InfiniteTimeSpan);
            UnbindBackBuffer();
            _pool.Dispose();
            _deviceManager.Dispose();
            _drawingSignal.Dispose();
            DetachFromHost();
            UnregisterMetrics();
            s_logger.Info($"dispose complete.");
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
                    $"skia.d3d.r{_rendererId}.draw_ms",
                    MetricWindowSec,
                    DrawMetricFormat,
                    LogLevel.Info);
            }

            if (_fpsMetricId <= 0)
            {
                _fpsMetricId = s_logger.RegisterMetric(
                    $"skia.d3d.r{_rendererId}.fps",
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
