using FastDrawingVisual.CommandRuntime;
using FastDrawingVisual.Rendering;
using FastDrawingVisual.Log;
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using Proxy = FastDrawingVisual.NativeProxy.NativeProxy;

namespace FastDrawingVisual.DCompD3D11
{
    public sealed class DCompD3D11Renderer : IRenderer
    {
        private ContentControl? _hostElement;
        private DCompHostHwnd? _hwndHost;
        private object? _previousContent;
        private bool _contentInjected;
        private IntPtr _boundHwnd;

        private IntPtr _nativeRenderer;
        private IntPtr _proxyHandle;
        private volatile bool _desktopTargetBound;
        private volatile bool _swapChainBound;
        private volatile bool _presentationReady;

        private int _width;
        private int _height;
        private readonly object _workerLock = new();
        private readonly SemaphoreSlim _drawSignal = new(0, 1);
        private readonly BridgeCommandWriter _commandWriter = new();
        private volatile Action<IDrawingContext>? _pendingDrawAction;
        private int _drawSignalState;
        private CancellationTokenSource? _workerCts;
        private Task? _drawingWorkerTask;
        private bool _isInitialized;
        private bool _isRenderFaulted;
        private bool _isDisposed;
        private int _drawDurationMetricId;
        private int _fpsMetricId;
        private long _lastFrameCompletedTimestamp;
        private long _submittedFrameCount;
        private static int s_nextRendererId;
        private readonly int _rendererId = Interlocked.Increment(ref s_nextRendererId);

        private static readonly TimeSpan WorkerShutdownTimeout = TimeSpan.FromSeconds(2);
        private const string LogCategory = "DCompD3D11Renderer";
        private const int MetricWindowSec = 1;
        private const string DrawMetricFormat = "name={name} periodSec={periodSec}s samples={count} avgMs={avg} minMs={min} maxMs={max}";
        private const string FpsMetricFormat = "name={name} periodSec={periodSec}s samples={count} avgFps={avg} minFps={min} maxFps={max}";
        private static readonly Logger s_logger = new(LogCategory);

        public bool AttachToElement(ContentControl element)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            if (element == null) throw new ArgumentNullException(nameof(element));

            if (ReferenceEquals(_hostElement, element))
            {
                TryEnsurePresentationBinding();
                return true;
            }

            UnhookHostElement();

            _hostElement = element;
            _hostElement.Loaded += OnHostLoaded;
            _hostElement.Unloaded += OnHostUnloaded;

            _hwndHost = new DCompHostHwnd
            {
                HorizontalAlignment = HorizontalAlignment.Stretch,
                VerticalAlignment = VerticalAlignment.Stretch
            };
            _hwndHost.HandleCreated += OnHostHandleCreated;
            _hwndHost.HandleDestroyed += OnHostHandleDestroyed;
            _previousContent = _hostElement.Content;
            _hostElement.Content = _hwndHost;
            _contentInjected = true;
            s_logger.Info($"rid={_rendererId} attach host; injected HWND host wrapper.");

            TryEnsurePresentationBinding();
            return true;
        }

        public bool Initialize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");

            _width = width;
            _height = height;
            s_logger.InfoEtw($"rid={_rendererId} initialize start width={_width} height={_height}.");

            try
            {
                EnsureNativeRenderer();
                EnsureProxyCreated();
                EnsureMetricsRegistered();
                _isRenderFaulted = false;
                _presentationReady = false;
                _isInitialized = true;
                _lastFrameCompletedTimestamp = 0;
                _submittedFrameCount = 0;
                StartDrawingWorker();
                TryEnsurePresentationBinding();
                s_logger.InfoEtw($"rid={_rendererId} initialize success width={_width} height={_height} native=0x{_nativeRenderer.ToInt64():X} proxy=0x{_proxyHandle.ToInt64():X}.");
                return true;
            }
            catch (Exception ex)
            {
                s_logger.ErrorEtw($"rid={_rendererId} initialize failed width={_width} height={_height}. {ex}");
                StopDrawingWorker(WorkerShutdownTimeout);
                _isInitialized = false;
                return false;
            }
        }

        public void Resize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");

            _width = width;
            _height = height;

            if (!_isInitialized || _nativeRenderer == IntPtr.Zero)
                return;

            if (!Proxy.Resize(_nativeRenderer, _width, _height))
                ThrowNativeFailure("FDV_Resize");
            s_logger.InfoEtw($"rid={_rendererId} resize success width={_width} height={_height}.");

            TryEnsurePresentationBinding();
            UpdateProxyRect();
        }

        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            if (!_isInitialized) throw new InvalidOperationException("Call Initialize first.");
            if (_isRenderFaulted || drawAction == null) return;

            Interlocked.Exchange(ref _pendingDrawAction, drawAction);
            SignalDrawingWorker();
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            StopDrawingWorker(Timeout.InfiniteTimeSpan);
            UnregisterMetrics();
            _commandWriter.Dispose();

            UnhookHostElement();

            DestroyProxy();
            DestroyNativeRenderer();
        }

        private void EnsureNativeRenderer()
        {
            if (_nativeRenderer != IntPtr.Zero)
                return;

            if (!Proxy.IsBridgeReady())
                throw new InvalidOperationException("FDV_IsBridgeReady returned false.");

            _nativeRenderer = Proxy.CreateRenderer(IntPtr.Zero, _width, _height);
            if (_nativeRenderer == IntPtr.Zero)
                throw new InvalidOperationException("FDV_CreateRenderer returned null handle.");
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
                // Suppress teardown errors.
            }
            finally
            {
                _nativeRenderer = IntPtr.Zero;
                _swapChainBound = false;
                _presentationReady = false;
            }
        }

        private void EnsureProxyCreated()
        {
            if (_proxyHandle != IntPtr.Zero)
                return;

            if (!WinRTProxyNative.FDV_WinRTProxy_IsReady())
                throw new InvalidOperationException("FDV_WinRTProxy_IsReady returned false.");

            _proxyHandle = WinRTProxyNative.FDV_WinRTProxy_Create();
            if (_proxyHandle == IntPtr.Zero)
                throw new InvalidOperationException("FDV_WinRTProxy_Create returned null handle.");

            if (!WinRTProxyNative.FDV_WinRTProxy_EnsureDispatcherQueue(_proxyHandle))
                ThrowProxyFailure("FDV_WinRTProxy_EnsureDispatcherQueue");
        }

        private void DestroyProxy()
        {
            if (_proxyHandle == IntPtr.Zero)
                return;

            try
            {
                WinRTProxyNative.FDV_WinRTProxy_Destroy(_proxyHandle);
            }
            catch
            {
                // Suppress teardown errors.
            }
            finally
            {
                _proxyHandle = IntPtr.Zero;
                _desktopTargetBound = false;
                _swapChainBound = false;
                _presentationReady = false;
                _boundHwnd = IntPtr.Zero;
            }
        }

        private bool TryEnsurePresentationBinding()
        {
            if (_isDisposed || !_isInitialized || _nativeRenderer == IntPtr.Zero || _proxyHandle == IntPtr.Zero)
                return false;

            if (_hostElement == null || _hwndHost == null)
                return false;

            var hwnd = _hwndHost.HostHandle;
            if (hwnd == IntPtr.Zero)
                return false;

            try
            {
                if (!_desktopTargetBound || _boundHwnd != hwnd)
                {
                    if (!WinRTProxyNative.FDV_WinRTProxy_EnsureDesktopTarget(_proxyHandle, hwnd, false))
                        ThrowProxyFailure("FDV_WinRTProxy_EnsureDesktopTarget");

                    _desktopTargetBound = true;
                    _swapChainBound = false;
                    _presentationReady = false;
                    _boundHwnd = hwnd;
                    s_logger.InfoEtw($"rid={_rendererId} desktop target bound hwnd=0x{hwnd.ToInt64():X}.");
                }

                if (!_swapChainBound)
                {
                    IntPtr swapChain = IntPtr.Zero;
                    if (!Proxy.TryGetSwapChain(_nativeRenderer, ref swapChain) || swapChain == IntPtr.Zero)
                        ThrowNativeFailure("FDV_TryGetSwapChain");

                    if (!WinRTProxyNative.FDV_WinRTProxy_BindSwapChain(_proxyHandle, swapChain))
                        ThrowProxyFailure("FDV_WinRTProxy_BindSwapChain");

                    _swapChainBound = true;
                    _presentationReady = false;
                    s_logger.InfoEtw($"rid={_rendererId} swapchain bound swapchain=0x{swapChain.ToInt64():X}.");
                }

                UpdateProxyRect();
                _presentationReady = true;
                SignalDrawingWorkerIfPending();
                return true;
            }
            catch (Exception ex)
            {
                _presentationReady = false;
                s_logger.ErrorEtw($"rid={_rendererId} presentation binding failed. {ex}");
                return false;
            }
        }

        private void UpdateProxyRect()
        {
            if (_proxyHandle == IntPtr.Zero || !_desktopTargetBound || !_swapChainBound)
                return;

            var w = Math.Max(1, _width);
            var h = Math.Max(1, _height);

            if (!WinRTProxyNative.FDV_WinRTProxy_UpdateSpriteRect(_proxyHandle, 0f, 0f, w, h))
                ThrowProxyFailure("FDV_WinRTProxy_UpdateSpriteRect");
        }

        private void StartDrawingWorker()
        {
            if (_isDisposed || !_isInitialized || _nativeRenderer == IntPtr.Zero || _isRenderFaulted)
                return;

            lock (_workerLock)
            {
                if (_drawingWorkerTask is { IsCompleted: false }) return;

                _workerCts?.Dispose();
                _workerCts = new CancellationTokenSource();
                _drawingWorkerTask = Task.Run(() => DrawingWorkerLoopAsync(_workerCts.Token));
                s_logger.Info($"rid={_rendererId} drawing worker started.");
            }

            SignalDrawingWorkerIfPending();
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
            s_logger.Info($"rid={_rendererId} drawing worker stopped.");

            return true;
        }

        private async Task DrawingWorkerLoopAsync(CancellationToken token)
        {
            try
            {
                while (true)
                {
                    await _drawSignal.WaitAsync(token).ConfigureAwait(false);
                    Interlocked.Exchange(ref _drawSignalState, 0);

                    if (_isDisposed || !_isInitialized || _nativeRenderer == IntPtr.Zero || _isRenderFaulted)
                        continue;

                    // Keep latest-wins action pending until swapchain is actually bound to DComp target.
                    if (!_desktopTargetBound || !_swapChainBound || !_presentationReady)
                        continue;

                    var action = Interlocked.Exchange(ref _pendingDrawAction, null);
                    if (action == null) continue;

                    var drawStarted = Stopwatch.GetTimestamp();
                    try
                    {
                        using var context = new DCompDrawingContext(_width, _height, _commandWriter, SubmitCommandsToNative);
                        action(context);
                    }
                    catch (Exception ex)
                    {
                        _isRenderFaulted = true;
                        s_logger.ErrorEtw($"rid={_rendererId} draw worker failed after {_submittedFrameCount} frames. {ex}");
                    }
                    finally
                    {
                        RecordFrameMetrics(drawStarted);
                    }
                }
            }
            catch (OperationCanceledException)
            {
            }
        }

        private void SignalDrawingWorker()
        {
            if (Interlocked.Exchange(ref _drawSignalState, 1) != 0)
                return;

            try
            {
                _drawSignal.Release();
            }
            catch (SemaphoreFullException)
            {
            }
        }

        private void SignalDrawingWorkerIfPending()
        {
            if (_pendingDrawAction != null)
                SignalDrawingWorker();
        }

        private void SubmitCommandsToNative(BridgeCommandPacket packet)
        {
            if (_nativeRenderer == IntPtr.Zero || packet.CommandBytes == 0)
                return;

            if (!Proxy.SubmitCommands(
                    _nativeRenderer,
                    packet.CommandPointer,
                    packet.CommandBytes,
                    packet.BlobPointer,
                    packet.BlobBytes))
                ThrowNativeFailure("FDV_SubmitCommands");
        }

        private static bool IsCancellationOnly(AggregateException ex)
        {
            foreach (var inner in ex.Flatten().InnerExceptions)
                if (inner is not OperationCanceledException)
                    return false;
            return true;
        }

        private void OnHostLoaded(object? sender, RoutedEventArgs e)
        {
            TryEnsurePresentationBinding();
            StartDrawingWorker();
        }

        private void OnHostUnloaded(object? sender, RoutedEventArgs e)
        {
            // Keep resources alive; attach can recover when host loads again.
        }

        private void OnHostHandleCreated(IntPtr hwnd)
        {
            _ = hwnd;
            TryEnsurePresentationBinding();
            StartDrawingWorker();
        }

        private void OnHostHandleDestroyed()
        {
            _boundHwnd = IntPtr.Zero;
            _desktopTargetBound = false;
            _swapChainBound = false;
            _presentationReady = false;
            s_logger.Info($"rid={_rendererId} host hwnd destroyed; presentation state reset.");
        }

        private void UnhookHostElement()
        {
            if (_hostElement != null)
            {
                _hostElement.Loaded -= OnHostLoaded;
                _hostElement.Unloaded -= OnHostUnloaded;
            }

            if (_contentInjected && _hostElement != null && _hwndHost != null)
            {
                if (ReferenceEquals(_hostElement.Content, _hwndHost))
                    _hostElement.Content = _previousContent;

                _contentInjected = false;
            }

            if (_hwndHost != null)
            {
                _hwndHost.HandleCreated -= OnHostHandleCreated;
                _hwndHost.HandleDestroyed -= OnHostHandleDestroyed;
                _hwndHost.Dispose();
                _hwndHost = null;
            }

            _hostElement = null;
            _previousContent = null;
            _boundHwnd = IntPtr.Zero;
            _desktopTargetBound = false;
            _swapChainBound = false;
            _presentationReady = false;
        }

        private void EnsureMetricsRegistered()
        {
            if (_drawDurationMetricId <= 0)
            {
                _drawDurationMetricId = s_logger.RegisterMetric(
                    $"dcomp.d3d11.r{_rendererId}.draw_ms",
                    MetricWindowSec,
                    DrawMetricFormat,
                    LogLevel.Info);
            }

            if (_fpsMetricId <= 0)
            {
                _fpsMetricId = s_logger.RegisterMetric(
                    $"dcomp.d3d11.r{_rendererId}.fps",
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

        private void ThrowProxyFailure(string operation)
        {
            var hr = _proxyHandle != IntPtr.Zero
                ? WinRTProxyNative.FDV_WinRTProxy_GetLastErrorHr(_proxyHandle)
                : unchecked((int)0x80004005);

            if (hr >= 0)
                hr = unchecked((int)0x80004005);

            var message = $"{operation} failed with HRESULT=0x{hr:X8}";
            s_logger.ErrorEtw($"rid={_rendererId} {message}");
            throw new COMException(message, hr);
        }

        private void ThrowNativeFailure(string operation)
        {
            var hr = _nativeRenderer != IntPtr.Zero
                ? Proxy.GetLastErrorHr(_nativeRenderer)
                : unchecked((int)0x80004005);

            if (hr >= 0)
                hr = unchecked((int)0x80004005);

            var message = $"{operation} failed with HRESULT=0x{hr:X8}";
            s_logger.ErrorEtw($"rid={_rendererId} {message}");
            throw new COMException(message, hr);
        }
    }
}
