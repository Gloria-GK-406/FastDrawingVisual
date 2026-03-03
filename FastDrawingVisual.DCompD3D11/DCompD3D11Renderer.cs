using FastDrawingVisual.Rendering;
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
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
        private DispatcherTimer? _presentationRetryTimer;

        private int _width;
        private int _height;
        private readonly object _workerLock = new();
        private volatile Action<IDrawingContext>? _pendingDrawAction;
        private CancellationTokenSource? _workerCts;
        private Task? _drawingWorkerTask;
        private bool _isInitialized;
        private bool _isRenderFaulted;
        private bool _isDisposed;

        private static readonly TimeSpan WorkerShutdownTimeout = TimeSpan.FromSeconds(2);

        public bool AttachToElement(ContentControl element)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            if (element == null) throw new ArgumentNullException(nameof(element));

            if (ReferenceEquals(_hostElement, element))
            {
                EnsurePresentationBindingOrScheduleRetry();
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
            _previousContent = _hostElement.Content;
            _hostElement.Content = _hwndHost;
            _contentInjected = true;

            EnsurePresentationBindingOrScheduleRetry();
            return true;
        }

        public bool Initialize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");

            _width = width;
            _height = height;

            try
            {
                EnsureNativeRenderer();
                EnsureProxyCreated();
                _isRenderFaulted = false;
                _presentationReady = false;
                _isInitialized = true;
                StartDrawingWorker();
                EnsurePresentationBindingOrScheduleRetry();
                return true;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"[DCompD3D11] Initialize failed: {ex}");
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

            EnsurePresentationBindingOrScheduleRetry();
            UpdateProxyRect();
        }

        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            if (!_isInitialized) throw new InvalidOperationException("Call Initialize first.");
            if (_isRenderFaulted || drawAction == null) return;

            Interlocked.Exchange(ref _pendingDrawAction, drawAction);
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            StopDrawingWorker(Timeout.InfiniteTimeSpan);
            DisposePresentationRetryTimer();

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

            WinRTProxyNative.EnsureResolverRegistered();

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
                }

                UpdateProxyRect();
                _presentationReady = true;
                return true;
            }
            catch (Exception ex)
            {
                _presentationReady = false;
                Debug.WriteLine($"[DCompD3D11] Presentation binding failed: {ex}");
                return false;
            }
        }

        private void EnsurePresentationBindingOrScheduleRetry()
        {
            if (TryEnsurePresentationBinding())
            {
                StopPresentationRetry();
                return;
            }

            StartPresentationRetry();
        }

        private void EnsurePresentationRetryTimer()
        {
            if (_presentationRetryTimer != null)
                return;

            _presentationRetryTimer = new DispatcherTimer(DispatcherPriority.Render)
            {
                Interval = TimeSpan.FromMilliseconds(16)
            };
            _presentationRetryTimer.Tick += OnPresentationRetryTick;
        }

        private void StartPresentationRetry()
        {
            EnsurePresentationRetryTimer();
            if (_presentationRetryTimer!.IsEnabled) return;
            _presentationRetryTimer.Start();
        }

        private void StopPresentationRetry()
        {
            if (_presentationRetryTimer?.IsEnabled == true)
                _presentationRetryTimer.Stop();
        }

        private void DisposePresentationRetryTimer()
        {
            if (_presentationRetryTimer == null)
                return;

            _presentationRetryTimer.Stop();
            _presentationRetryTimer.Tick -= OnPresentationRetryTick;
            _presentationRetryTimer = null;
        }

        private void OnPresentationRetryTick(object? sender, EventArgs e)
        {
            if (_isDisposed || !_isInitialized)
            {
                StopPresentationRetry();
                return;
            }

            if (TryEnsurePresentationBinding())
                StopPresentationRetry();
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

        private async Task DrawingWorkerLoopAsync(CancellationToken token)
        {
            using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(1));

            try
            {
                while (await timer.WaitForNextTickAsync(token).ConfigureAwait(false))
                {
                    if (_isDisposed || !_isInitialized || _nativeRenderer == IntPtr.Zero || _isRenderFaulted)
                        continue;

                    // Keep latest-wins action pending until swapchain is actually bound to DComp target.
                    if (!_desktopTargetBound || !_swapChainBound || !_presentationReady)
                        continue;

                    var action = Interlocked.Exchange(ref _pendingDrawAction, null);
                    if (action == null) continue;

                    try
                    {
                        using var context = new DCompDrawingContext(_width, _height, SubmitCommandsToNative);
                        action(context);
                    }
                    catch (Exception ex)
                    {
                        _isRenderFaulted = true;
                        Debug.WriteLine($"[DCompD3D11] Draw worker failed: {ex}");
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

            var span = commandData.Span;
            fixed (byte* ptr = span)
            {
                if (!Proxy.SubmitCommands(_nativeRenderer, (IntPtr)ptr, span.Length))
                    ThrowNativeFailure("FDV_SubmitCommands");
            }
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
            EnsurePresentationBindingOrScheduleRetry();
            StartDrawingWorker();
        }

        private void OnHostUnloaded(object? sender, RoutedEventArgs e)
        {
            // Keep resources alive; attach can recover when host loads again.
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
                _hwndHost.Dispose();
                _hwndHost = null;
            }

            _hostElement = null;
            _previousContent = null;
            _boundHwnd = IntPtr.Zero;
            _desktopTargetBound = false;
            _swapChainBound = false;
            _presentationReady = false;
            StopPresentationRetry();
        }

        private void ThrowProxyFailure(string operation)
        {
            var hr = _proxyHandle != IntPtr.Zero
                ? WinRTProxyNative.FDV_WinRTProxy_GetLastErrorHr(_proxyHandle)
                : unchecked((int)0x80004005);

            if (hr >= 0)
                hr = unchecked((int)0x80004005);

            throw new COMException($"{operation} failed with HRESULT=0x{hr:X8}", hr);
        }

        private void ThrowNativeFailure(string operation)
        {
            var hr = _nativeRenderer != IntPtr.Zero
                ? Proxy.GetLastErrorHr(_nativeRenderer)
                : unchecked((int)0x80004005);

            if (hr >= 0)
                hr = unchecked((int)0x80004005);

            throw new COMException($"{operation} failed with HRESULT=0x{hr:X8}", hr);
        }
    }
}
