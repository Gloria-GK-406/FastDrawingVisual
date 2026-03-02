using FastDrawingVisual.Rendering;
using FastDrawingVisual.Rendering.D3D;
using System;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;

namespace FastDrawingVisual.SkiaSharp
{
    /// <summary>
    /// 基于 D3D11 共享纹理 + SkiaSharp CPU 光栅化的高性能渲染器。
    /// <para>
    /// 通过 <see cref="IRenderer.Visual"/> 暴露一个托管 <see cref="D3DImage"/> 的
    /// <see cref="DrawingVisual"/>，由宿主控件 <c>FastDrawingVisual</c> AddVisualChild 进视觉树。
    /// </para>
    /// <para>调度模型：</para>
    /// <list type="bullet">
    ///   <item>外部通过 <see cref="SubmitDrawing"/> 将绘制委托写入单槽 Replace 队列（任意线程）。</item>
    ///   <item>内部 <see cref="DrawingWorkerLoopAsync"/> 使用 PeriodicTimer（1 ms）轮询槽位，
    ///         取到委托后执行绘制并推进到 ReadyForPresent。</item>
    ///   <item>UI 线程通过 CompositionTarget.Rendering（主路径）或 DispatcherTimer（辅助路径）
    ///         调用 <see cref="TrySubmitFrame"/> 将帧提交到 D3DImage。</item>
    /// </list>
    /// </summary>
    public sealed class D3DSkiaRenderer : IRenderer
    {
        // ── D3D / WPF 基础设施 ───────────────────────────────────────────────
        private readonly D3DImage       _d3dImage;
        private readonly DrawingVisual  _visual;
        private readonly D3DDeviceManager _deviceManager;
        private readonly RenderFramePool  _pool;
        private readonly Dispatcher       _uiDispatcher;
        private IVisualHostElement? _attachedHost;

        // ── 单槽 Replace 队列 ────────────────────────────────────────────────
        private volatile Action<IDrawingContext>? _pendingDrawAction;

        // ── DrawingWorker 生命周期 ───────────────────────────────────────────
        private readonly object _workerLock = new();
        private CancellationTokenSource? _workerCts;
        private Task? _drawingWorkerTask;

        // ── 辅助重试计时器 ───────────────────────────────────────────────────
        private readonly DispatcherTimer _retryTimer;

        // ── 状态 ────────────────────────────────────────────────────────────
        private int  _width;
        private int  _height;
        private bool _isInitialized;
        private bool _isDeviceLost;
        private bool _isDisposed;

        private static readonly TimeSpan WorkerShutdownTimeout = TimeSpan.FromSeconds(2);

        public D3DSkiaRenderer()
        {
            _d3dImage     = new D3DImage();
            _visual       = new DrawingVisual();
            _deviceManager = new D3DDeviceManager();
            _pool          = new RenderFramePool(_deviceManager);
            _uiDispatcher  = Dispatcher.CurrentDispatcher;

            _d3dImage.IsFrontBufferAvailableChanged += OnFrontBufferAvailableChanged;

            _retryTimer = new DispatcherTimer(DispatcherPriority.Render, _uiDispatcher)
            {
                Interval = TimeSpan.FromMilliseconds(1)
            };
            _retryTimer.Tick += (_, _) => TrySubmitFrame();
        }

        #region IRenderer

        public bool AttachToElement(FrameworkElement element)
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
            return true;
        }

        /// <inheritdoc/>
        public bool Initialize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(D3DSkiaRenderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("宽高必须大于 0。");

            if (!_deviceManager.IsInitialized && !_deviceManager.Initialize())
                return false;

            _pool.CreateResources(width, height);
            _width  = width;
            _height = height;
            _isInitialized = true;
            _isDeviceLost  = false;

            // 将 D3DImage 放入 DrawingVisual（只需设置一次，后续只更新 D3DImage 本身）
            BindD3DImageToVisual(width, height);

            StartDrawingWorker();
            CompositionTarget.Rendering += OnCompositionTargetRendering;
            return true;
        }

        /// <inheritdoc/>
        public void Resize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(D3DSkiaRenderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("宽高必须大于 0。");
            if (width == _width && height == _height) return;
            if (_isDeviceLost) { _width = width; _height = height; return; }

            RebuildResources(width, height);
        }

        /// <inheritdoc/>
        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(D3DSkiaRenderer));
            if (!_isInitialized) throw new InvalidOperationException("请先调用 Initialize。");
            if (_isDeviceLost) return;
            if (drawAction == null) throw new ArgumentNullException(nameof(drawAction));

            Interlocked.Exchange(ref _pendingDrawAction, drawAction);
        }

        #endregion

        #region DrawingVisual 绑定

        /// <summary>
        /// 将 D3DImage 嵌入 DrawingVisual 的内容流（仅初始化和 Resize 时调用一次）。
        /// 帧更新通过 D3DImage.Lock/AddDirtyRect/Unlock 完成，DrawingVisual 内容保持不变。
        /// </summary>
        private void BindD3DImageToVisual(int width, int height)
        {
            using var dc = _visual.RenderOpen();
            dc.DrawImage(_d3dImage, new Rect(0, 0, width, height));
        }

        #endregion

        #region DrawingWorker 后台轮询

        private async Task DrawingWorkerLoopAsync(CancellationToken token)
        {
            using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(1));
            try
            {
                while (await timer.WaitForNextTickAsync(token).ConfigureAwait(false))
                {
                    if (_isDeviceLost || !_isInitialized) continue;

                    var action = Interlocked.Exchange(ref _pendingDrawAction, null);
                    if (action == null) continue;

                    var frame = _pool.AcquireForDrawing();
                    if (frame == null)
                    {
                        Interlocked.CompareExchange(ref _pendingDrawAction, action, null);
                        continue;
                    }

                    try
                    {
                        using var ctx = frame.OpenCanvas();
                        action(ctx);
                    }
                    catch
                    {
                        frame.TryTransitionTo(FrameState.Drawing, FrameState.Ready);
                    }
                }
            }
            catch (OperationCanceledException) { }
        }

        #endregion

        #region 呈现路径（UI 线程）

        private void OnCompositionTargetRendering(object? sender, EventArgs e)
        {
            if (_isDisposed || _isDeviceLost || !_isInitialized) return;
            TrySubmitFrame();
        }

        private void TrySubmitFrame()
        {
            if (_isDisposed || _isDeviceLost || !_isInitialized) return;

            var frame = _pool.TryAcquireForPresent();
            if (frame == null) { _retryTimer.Stop(); return; }

            if (!_d3dImage.TryLock(new Duration(TimeSpan.Zero)))
            {
                _d3dImage.Unlock(); // WPF D3DImage TryLock 失败时仍需 Unlock（已知 WPF bug）
                _pool.ResetToReadyForPresent(frame);
                if (!_retryTimer.IsEnabled) _retryTimer.Start();
                return;
            }

            _retryTimer.Stop();
            try
            {
                _pool.MarkPresentedFrameAsReady(frame);
                _d3dImage.SetBackBuffer(D3DResourceType.IDirect3DSurface9, frame.D3D9SurfacePointer);
                _d3dImage.AddDirtyRect(new Int32Rect(0, 0, _width, _height));
                frame.ForceSetState(FrameState.Presenting);
            }
            finally
            {
                _d3dImage.Unlock();
            }
        }

        #endregion

        #region 设备丢失处理

        private void OnFrontBufferAvailableChanged(object sender, DependencyPropertyChangedEventArgs e)
        {
            if (!(bool)e.NewValue)
            {
                _isDeviceLost = true;
                _uiDispatcher.BeginInvoke(() =>
                {
                    if (_isDisposed) return;
                    _retryTimer.Stop();
                    if (!StopDrawingWorker(WorkerShutdownTimeout)) return;
                    _isInitialized = false;
                    UnbindBackBuffer();
                    _pool.ReleaseResources();
                });
            }
            else
            {
                _uiDispatcher.BeginInvoke(() =>
                {
                    try
                    {
                        if (_isDisposed) return;
                        if (!_deviceManager.Initialize()) return;
                        if (_width > 0 && _height > 0) RebuildResources(_width, _height);
                    }
                    catch { _isDeviceLost = true; }
                });
            }
        }

        private void UnbindBackBuffer()
        {
            _d3dImage.Lock();
            try { _d3dImage.SetBackBuffer(D3DResourceType.IDirect3DSurface9, IntPtr.Zero); }
            finally { _d3dImage.Unlock(); }
        }

        #endregion

        #region Worker 生命周期

        private void RebuildResources(int width, int height)
        {
            if (!StopDrawingWorker(WorkerShutdownTimeout))
                throw new TimeoutException("绘制线程未在超时时间内退出，已中止 Resize。");

            _retryTimer.Stop();
            _isInitialized = false;
            UnbindBackBuffer();
            _pool.ReleaseResources();
            _pool.CreateResources(width, height);
            _width  = width;
            _height = height;
            _isInitialized = true;
            _isDeviceLost  = false;

            BindD3DImageToVisual(width, height);
            StartDrawingWorker();
        }

        private void StartDrawingWorker()
        {
            if (_isDisposed || _isDeviceLost || !_isInitialized) return;
            lock (_workerLock)
            {
                if (_isDisposed || _isDeviceLost || !_isInitialized) return;
                if (_drawingWorkerTask is { IsCompleted: false }) return;
                _workerCts?.Dispose();
                _workerCts = new CancellationTokenSource();
                var token = _workerCts.Token;
                _drawingWorkerTask = Task.Run(() => DrawingWorkerLoopAsync(token));
            }
        }

        private bool StopDrawingWorker(TimeSpan timeout)
        {
            Task? workerTask;
            CancellationTokenSource? workerCts;
            lock (_workerLock) { workerTask = _drawingWorkerTask; workerCts = _workerCts; }

            if (workerCts == null && workerTask == null) return true;
            workerCts?.Cancel();

            if (workerTask != null)
            {
                try
                {
                    if (timeout == Timeout.InfiniteTimeSpan) workerTask.Wait();
                    else if (!workerTask.Wait(timeout)) return false;
                }
                catch (AggregateException ex) when (IsCancellationOnly(ex)) { }
            }

            lock (_workerLock)
            {
                if (ReferenceEquals(_drawingWorkerTask, workerTask)) _drawingWorkerTask = null;
                if (ReferenceEquals(_workerCts, workerCts)) { _workerCts?.Dispose(); _workerCts = null; }
            }
            return true;
        }

        private static bool IsCancellationOnly(AggregateException ex)
        {
            foreach (var inner in ex.Flatten().InnerExceptions)
                if (inner is not OperationCanceledException) return false;
            return true;
        }

        #endregion

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            CompositionTarget.Rendering -= OnCompositionTargetRendering;
            _d3dImage.IsFrontBufferAvailableChanged -= OnFrontBufferAvailableChanged;
            _retryTimer.Stop();
            StopDrawingWorker(Timeout.InfiniteTimeSpan);
            UnbindBackBuffer();
            _pool.Dispose();
            _deviceManager.Dispose();
            DetachFromHost();
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
