using FastDrawingVisual.Rendering;
using System;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using NativeD3D9BridgeProxy = FastDrawingVisual.NativeD3D9Bridge.NativeD3D9BridgeProxy;

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

        private static readonly TimeSpan WorkerShutdownTimeout = TimeSpan.FromSeconds(2);

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
            return true;
        }

        public bool Initialize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(NativeD3D9Renderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");

            var renderer = CreateNativeRenderer(width, height);
            if (renderer == IntPtr.Zero)
                return false;

            _nativeRenderer = renderer;
            _width = width;
            _height = height;
            _isInitialized = true;
            _isDeviceLost = false;
            _isBackBufferBound = false;

            BindD3DImageToVisual(width, height);
            StartDrawingWorker();
            CompositionTarget.Rendering += OnCompositionTargetRendering;
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
                return;
            }

            UnbindBackBuffer();

            if (!SafeResizeNative(width, height))
                return;

            _width = width;
            _height = height;
            BindD3DImageToVisual(width, height);
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

                    try
                    {
                        using var ctx = new NativeDrawingContext(_width, _height, SubmitCommandsToNative);
                        action(ctx);
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
                    NativeD3D9BridgeProxy.SubmitCommands(_nativeRenderer, (IntPtr)ptr, span.Length);
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

                if (!NativeD3D9BridgeProxy.CopyReadyToPresentSurface(_nativeRenderer))
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
                _isDeviceLost = true;
                _uiDispatcher.BeginInvoke(() =>
                {
                    if (_isDisposed) return;
                    _retryTimer.Stop();
                    StopDrawingWorker(WorkerShutdownTimeout);
                    _isInitialized = false;
                    UnbindBackBuffer();
                    TryNotifyFrontBufferAvailable(false);
                });
            }
            else
            {
                _uiDispatcher.BeginInvoke(() =>
                {
                    if (_isDisposed) return;
                    if (_width <= 0 || _height <= 0) return;

                    if (_nativeRenderer == IntPtr.Zero)
                    {
                        _nativeRenderer = CreateNativeRenderer(_width, _height);
                        if (_nativeRenderer == IntPtr.Zero) return;
                    }

                    TryNotifyFrontBufferAvailable(true);
                    if (!SafeResizeNative(_width, _height)) return;

                    _isDeviceLost = false;
                    _isInitialized = true;
                    _isBackBufferBound = false;
                    BindD3DImageToVisual(_width, _height);
                    StartDrawingWorker();
                });
            }
        }

        private bool EnsureBackBufferBound()
        {
            if (_isBackBufferBound)
                return true;

            var surface = IntPtr.Zero;
            if (!NativeD3D9BridgeProxy.TryAcquirePresentSurface(_nativeRenderer, ref surface) || surface == IntPtr.Zero)
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
                return NativeD3D9BridgeProxy.CreateRenderer(hwnd, width, height);
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
                if (!NativeD3D9BridgeProxy.Resize(_nativeRenderer, width, height))
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
                NativeD3D9BridgeProxy.OnFrontBufferAvailable(_nativeRenderer, available);
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
                NativeD3D9BridgeProxy.DestroyRenderer(_nativeRenderer);
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
    }
}
