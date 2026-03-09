using FastDrawingVisual.Log;
using FastDrawingVisual.Rendering;
using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using Proxy = FastDrawingVisual.NativeProxy.NativeProxy;

namespace FastDrawingVisual.Rendering.NativeD3D9
{
    internal sealed class NativeD3D9Renderer : IRenderer
    {
        private readonly D3DImage _d3dImage;
        private readonly DrawingVisual _visual;
        private readonly Dispatcher _uiDispatcher;
        private readonly DispatcherTimer _retryTimer;
        private readonly object _workerLock = new();
        private IVisualHostElement? _attachedHost;

        private volatile Action<IDrawingContext>? _pendingDrawAction;
        private CancellationTokenSource? _workerCts;
        private Task? _drawingWorkerTask;
        private HwndSource? _fallbackHwndSource;

        private IntPtr _nativeRenderer;
        private int _width;
        private int _height;
        private bool _isInitialized;
        private bool _isDeviceLost;
        private bool _isBackBufferBound;
        private bool _isDisposed;
        private int _drawDurationMetricId;
        private int _fpsMetricId;
        private long _lastFrameCompletedTimestamp;
        private long _submittedFrameCount;
        private static int s_nextRendererId;
        private readonly int _rendererId = Interlocked.Increment(ref s_nextRendererId);

        private static readonly TimeSpan WorkerShutdownTimeout = TimeSpan.FromSeconds(2);
        private const string LogCategory = "NativeD3D9Renderer";
        private const int MetricWindowSec = 1;
        private const string DrawMetricFormat = "name={name} periodSec={periodSec}s samples={count} avgMs={avg} minMs={min} maxMs={max}";
        private const string FpsMetricFormat = "name={name} periodSec={periodSec}s samples={count} avgFps={avg} minFps={min} maxFps={max}";
        private static readonly Logger s_logger = new(LogCategory);
        public NativeD3D9Renderer()
        {
            _d3dImage = new D3DImage();
            _visual = new DrawingVisual();
            _uiDispatcher = Dispatcher.CurrentDispatcher;
            _retryTimer = new DispatcherTimer(DispatcherPriority.Render, _uiDispatcher)
            {
                Interval = TimeSpan.FromMilliseconds(1)
            };
            _retryTimer.Tick += (_, _) => TrySubmitFrame();
            _d3dImage.IsFrontBufferAvailableChanged += OnFrontBufferAvailableChanged;
        }

        public bool AttachToElement(ContentControl element)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(NativeD3D9Renderer));
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
            if (_isDisposed) throw new ObjectDisposedException(nameof(NativeD3D9Renderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");

            s_logger.InfoEtw($"initialize start width={width} height={height}.");
            var renderer = CreateNativeRenderer(width, height);
            if (renderer == IntPtr.Zero)
            {
                s_logger.ErrorEtw($"initialize failed: native renderer creation returned null.");
                return false;
            }

            _nativeRenderer = renderer;
            _width = width;
            _height = height;
            EnsureMetricsRegistered();
            _isInitialized = true;
            _isDeviceLost = false;
            _isBackBufferBound = false;
            _lastFrameCompletedTimestamp = 0;
            _submittedFrameCount = 0;

            BindD3DImageToVisual(width, height);
            StartDrawingWorker();
            CompositionTarget.Rendering += OnCompositionTargetRendering;
            s_logger.InfoEtw($"initialize success width={width} height={height} native=0x{_nativeRenderer.ToInt64():X}.");
            return true;
        }

        public void Resize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(NativeD3D9Renderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");
            if (!_isInitialized) return;
            if (width == _width && height == _height) return;
            if (_isDeviceLost)
            {
                _width = width;
                _height = height;
                s_logger.Warn($"resize ignored during device lost width={width} height={height}.");
                return;
            }

            s_logger.InfoEtw($"resize start width={width} height={height}.");
            UnbindBackBuffer();

            if (!SafeResizeNative(width, height))
            {
                s_logger.ErrorEtw($"resize failed width={width} height={height}.");
                return;
            }

            _width = width;
            _height = height;
            BindD3DImageToVisual(width, height);
            s_logger.InfoEtw($"resize success width={width} height={height}.");
        }

        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(NativeD3D9Renderer));
            if (!_isInitialized) throw new InvalidOperationException("Call Initialize first.");
            if (_isDeviceLost || drawAction == null) return;

            Interlocked.Exchange(ref _pendingDrawAction, drawAction);
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            s_logger.Info($"dispose start.");
            _isDisposed = true;

            CompositionTarget.Rendering -= OnCompositionTargetRendering;
            _d3dImage.IsFrontBufferAvailableChanged -= OnFrontBufferAvailableChanged;
            _retryTimer.Stop();
            StopDrawingWorker(Timeout.InfiniteTimeSpan);
            UnbindBackBuffer();
            DestroyNativeRenderer();
            _fallbackHwndSource?.Dispose();
            _fallbackHwndSource = null;
            DetachFromHost();
            UnregisterMetrics();
            s_logger.Info($"dispose complete.");
        }

        private void BindD3DImageToVisual(int width, int height)
        {
            using var dc = _visual.RenderOpen();
            dc.DrawImage(_d3dImage, new Rect(0, 0, width, height));
        }

        private async Task DrawingWorkerLoopAsync(CancellationToken token)
        {
            using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(1));

            try
            {
                while (await timer.WaitForNextTickAsync(token).ConfigureAwait(false))
                {
                    if (_isDisposed || _isDeviceLost || !_isInitialized || _nativeRenderer == IntPtr.Zero)
                        continue;

                    var action = Interlocked.Exchange(ref _pendingDrawAction, null);
                    if (action == null) continue;

                    var drawStarted = Stopwatch.GetTimestamp();
                    try
                    {
                        using var ctx = new NativeDrawingContext(_width, _height, SubmitCommandsToNative);
                        action(ctx);
                        RecordFrameMetrics(drawStarted);
                    }
                    catch
                    {
                        // MVP behavior: drop the failed frame and keep running.
                    }
                }
            }
            catch (OperationCanceledException)
            {
            }
        }

        private unsafe void SubmitCommandsToNative(ReadOnlyMemory<byte> commandData)
        {
            if (_nativeRenderer == IntPtr.Zero || commandData.Length == 0)
                return;

            try
            {
                var span = commandData.Span;
                fixed (byte* ptr = span)
                {
                    Proxy.SubmitCommands(_nativeRenderer, (IntPtr)ptr, span.Length);
                }
            }
            catch
            {
                _isDeviceLost = true;
            }
        }

        private void OnCompositionTargetRendering(object? sender, EventArgs e)
        {
            if (_isDisposed || _isDeviceLost || !_isInitialized || _nativeRenderer == IntPtr.Zero)
                return;

            TrySubmitFrame();
        }

        private void TrySubmitFrame()
        {
            if (_nativeRenderer == IntPtr.Zero || !_isInitialized || _isDeviceLost || _isDisposed)
                return;

            if (!_d3dImage.TryLock(new Duration(TimeSpan.Zero)))
            {
                // Known WPF bug:D3DImage TryLock(false) still need a balancing Unlock.
                _d3dImage.Unlock();
                if (!_retryTimer.IsEnabled) _retryTimer.Start();
                return;
            }

            _retryTimer.Stop();
            try
            {
                if (!EnsureBackBufferBound())
                    return;

                if (!Proxy.CopyReadyToPresentSurface(_nativeRenderer))
                    return;

                _d3dImage.AddDirtyRect(new Int32Rect(0, 0, _width, _height));
            }
            catch
            {
                _isDeviceLost = true;
            }
            finally
            {
                _d3dImage.Unlock();
            }
        }

        private void OnFrontBufferAvailableChanged(object sender, DependencyPropertyChangedEventArgs e)
        {
            var available = (bool)e.NewValue;
            if (!available)
            {
                s_logger.WarnEtw($"front buffer unavailable; device lost.");
                _isDeviceLost = true;
                _uiDispatcher.BeginInvoke(() =>
                {
                    if (_isDisposed) return;
                    _retryTimer.Stop();
                    StopDrawingWorker(WorkerShutdownTimeout);
                    _isInitialized = false;
                    UnbindBackBuffer();
                    TryNotifyFrontBufferAvailable(false);
                    s_logger.Info($"resources released after device lost.");
                });
            }
            else
            {
                s_logger.InfoEtw($"front buffer available; attempting recovery.");
                _uiDispatcher.BeginInvoke(() =>
                {
                    if (_isDisposed) return;
                    if (_width <= 0 || _height <= 0) return;

                    if (_nativeRenderer == IntPtr.Zero)
                    {
                        _nativeRenderer = CreateNativeRenderer(_width, _height);
                        if (_nativeRenderer == IntPtr.Zero)
                        {
                            s_logger.Error($"device recovery failed: native renderer creation returned null.");
                            return;
                        }
                    }

                    TryNotifyFrontBufferAvailable(true);
                    if (!SafeResizeNative(_width, _height))
                    {
                        s_logger.ErrorEtw($"device recovery failed: resize failed.");
                        return;
                    }

                    _isDeviceLost = false;
                    _isInitialized = true;
                    _isBackBufferBound = false;
                    BindD3DImageToVisual(_width, _height);
                    StartDrawingWorker();
                    s_logger.InfoEtw($"device recovery successful.");
                });
            }
        }

        private bool EnsureBackBufferBound()
        {
            if (_isBackBufferBound)
                return true;

            var surface = IntPtr.Zero;
            if (!Proxy.TryAcquirePresentSurface(_nativeRenderer, ref surface) || surface == IntPtr.Zero)
                return false;

            _d3dImage.SetBackBuffer(D3DResourceType.IDirect3DSurface9, surface);
            _isBackBufferBound = true;
            return true;
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

        private void StartDrawingWorker()
        {
            if (_isDisposed || _isDeviceLost || !_isInitialized || _nativeRenderer == IntPtr.Zero)
                return;

            lock (_workerLock)
            {
                if (_drawingWorkerTask is { IsCompleted: false }) return;

                _workerCts?.Dispose();
                _workerCts = new CancellationTokenSource();
                _drawingWorkerTask = Task.Run(() => DrawingWorkerLoopAsync(_workerCts.Token));
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

            if (workerTask == null && workerCts == null)
                return true;

            workerCts?.Cancel();

            if (workerTask != null)
            {
                try
                {
                    if (timeout == Timeout.InfiniteTimeSpan) workerTask.Wait();
                    else if (!workerTask.Wait(timeout)) return false;
                }
                catch (AggregateException ex) when (IsCancellationOnly(ex))
                {
                }
            }

            lock (_workerLock)
            {
                if (ReferenceEquals(_drawingWorkerTask, workerTask)) _drawingWorkerTask = null;
                if (ReferenceEquals(_workerCts, workerCts))
                {
                    _workerCts?.Dispose();
                    _workerCts = null;
                }
            }

            return true;
        }

        private static bool IsCancellationOnly(AggregateException ex)
        {
            foreach (var inner in ex.Flatten().InnerExceptions)
                if (inner is not OperationCanceledException)
                    return false;
            return true;
        }

        private IntPtr CreateNativeRenderer(int width, int height)
        {
            var hwnd = GetOrCreateDeviceHwnd();
            if (hwnd == IntPtr.Zero)
                return IntPtr.Zero;

            try
            {
                return Proxy.CreateRenderer(hwnd, width, height);
            }
            catch
            {
                return IntPtr.Zero;
            }
        }

        private bool SafeResizeNative(int width, int height)
        {
            if (_nativeRenderer == IntPtr.Zero)
                return false;

            try
            {
                if (!Proxy.Resize(_nativeRenderer, width, height))
                {
                    _isDeviceLost = true;
                    return false;
                }
                _isBackBufferBound = false;
                return true;
            }
            catch
            {
                _isDeviceLost = true;
                return false;
            }
        }

        private void TryNotifyFrontBufferAvailable(bool available)
        {
            if (_nativeRenderer == IntPtr.Zero)
                return;

            try
            {
                Proxy.OnFrontBufferAvailable(_nativeRenderer, available);
            }
            catch
            {
                _isDeviceLost = true;
            }
        }

        private void DestroyNativeRenderer()
        {
            if (_nativeRenderer == IntPtr.Zero)
                return;

            try
            {
                Proxy.DestroyRenderer(_nativeRenderer);
            }
            catch
            {
            }
            finally
            {
                _nativeRenderer = IntPtr.Zero;
            }
        }

        private IntPtr GetOrCreateDeviceHwnd()
        {
            var mainWindow = Application.Current?.MainWindow;
            if (mainWindow != null)
            {
                var mainHwnd = new WindowInteropHelper(mainWindow).Handle;
                if (mainHwnd != IntPtr.Zero)
                    return mainHwnd;
            }

            if (_fallbackHwndSource == null)
            {
                var p = new HwndSourceParameters("FastDrawingVisual.NativeD3D9Host")
                {
                    Width = 1,
                    Height = 1,
                    WindowStyle = unchecked((int)0x80000000) // WS_POPUP
                };
                _fallbackHwndSource = new HwndSource(p);
            }

            return _fallbackHwndSource.Handle;
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
                    $"native.d3d9.r{_rendererId}.draw_ms",
                    MetricWindowSec,
                    DrawMetricFormat,
                    LogLevel.Info);
            }

            if (_fpsMetricId <= 0)
            {
                _fpsMetricId = s_logger.RegisterMetric(
                    $"native.d3d9.r{_rendererId}.fps",
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
