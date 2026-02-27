using FastDrawingVisual.Rendering;
using FastDrawingVisual.Rendering.D3D;
using System;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;

namespace FastDrawingVisual.Controls
{
    /// <summary>
    /// 基于 D3DImage 的高性能渲染图像控件。
    /// <para>
    /// 调度模型：
    /// <list type="bullet">
    ///   <item>外部通过 <see cref="SubmitDrawing"/> 将绘制委托写入单槽 Replace 队列（任意线程，原子操作）。</item>
    ///   <item>内部 <see cref="DrawingWorkerLoopAsync"/> 使用 <see cref="PeriodicTimer"/>（1 ms）轮询槽位，
    ///         取到委托后立即执行绘制并将帧推进到 ReadyForPresent。</item>
    ///   <item>UI 线程通过 <c>CompositionTarget.Rendering</c>（主路径）或 <see cref="DispatcherTimer"/>（辅助路径）
    ///         调用 <see cref="TrySubmitFrame"/>，在 TryLock 可用时将帧提交到 D3DImage。</item>
    /// </list>
    /// DrawingWorker 与呈现侧完全解耦：Worker 持续轮询，呈现侧独立重试 TryLock，
    /// 无需任何跨线程信号量即可协同工作。
    /// </para>
    /// </summary>
    internal sealed class D3DFastImage : IFastImage, IDisposable
    {
        // ── D3D / WPF 基础设施 ───────────────────────────────────────────────
        private readonly D3DImage _d3dImage;
        private readonly D3DDeviceManager _deviceManager;
        private readonly RenderFramePool _pool;
        private readonly Dispatcher _uiDispatcher;

        // ── 单槽 Replace 队列（任意线程写，DrawingWorker 读）────────────────
        // volatile 保证跨线程可见性；Interlocked.Exchange 保证写原子性。
        private volatile Action<IDrawingContext>? _pendingDrawAction;

        // ── DrawingWorker 生命周期 ───────────────────────────────────────────
        private readonly object _workerLock = new object();
        private CancellationTokenSource? _workerCts;
        private Task? _drawingWorkerTask;

        // ── 辅助提交路径：DispatcherTimer ───────────────────────────────────
        // 当 Rendering 回调里 TryLock 失败时启动，在 UI 线程以较高频率重试，
        // 利用合成器完成合成后产生的 TryLock 可用窗口（约 t≈1~16 ms）。
        private readonly DispatcherTimer _retryTimer;

        // ── 状态 ─────────────────────────────────────────────────────────────
        private int _width;
        private int _height;
        private bool _isInitialized;
        private bool _isDeviceLost;
        private bool _isDisposed;
        private static readonly TimeSpan WorkerShutdownTimeout = TimeSpan.FromSeconds(2);

        public ImageSource Source => _d3dImage;
        public double Width => _width;
        public double Height => _height;
        public bool IsInitialized => _isInitialized;

        public D3DFastImage()
        {
            _d3dImage = new D3DImage();
            _deviceManager = new D3DDeviceManager();
            _pool = new RenderFramePool(_deviceManager);
            _uiDispatcher = Dispatcher.CurrentDispatcher;

            _d3dImage.IsFrontBufferAvailableChanged += OnFrontBufferAvailableChanged;

            // ── 辅助重试计时器（UI 线程，间隔 1 ms） ────────────────────────
            // 仅在 TryLock 失败后临时启动，成功提交后立即停止。
            _retryTimer = new DispatcherTimer(DispatcherPriority.Render, _uiDispatcher)
            {
                Interval = TimeSpan.FromMilliseconds(1)
            };
            _retryTimer.Tick += (_, _) => TrySubmitFrame();
        }

        #region 初始化 / 调整大小

        /// <summary>初始化设备和所有渲染帧资源。</summary>
        public bool Initialize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(D3DFastImage));
            if (width <= 0 || height <= 0) throw new ArgumentException("宽高必须大于 0。");

            if (!_deviceManager.IsInitialized && !_deviceManager.Initialize())
                return false;

            _pool.CreateResources(width, height);
            _width = width;
            _height = height;
            _isInitialized = true;
            _isDeviceLost = false;

            // 启动 DrawingWorker（Task 生命周期与此实例绑定）
            StartDrawingWorker();

            CompositionTarget.Rendering += OnCompositionTargetRendering;
            return true;
        }

        /// <summary>调整渲染尺寸，重建所有帧资源。</summary>
        public void Resize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(D3DFastImage));
            if (width <= 0 || height <= 0) throw new ArgumentException("宽高必须大于 0。");
            if (width == _width && height == _height) return;
            if (_isDeviceLost)
            {
                // 设备丢失期间仅记录目标尺寸，待恢复时按最新尺寸重建。
                _width = width;
                _height = height;
                return;
            }

            RebuildResources(width, height);
        }

        #endregion

        #region 公共绘制 API

        /// <summary>
        /// 向内部调度器提交一个绘制委托，可在任意线程调用。
        /// <para>
        /// 新委托原子写入单槽队列；若槽中已有旧委托尚未执行，旧委托被替换（丢弃时新委托是其后继，语义安全）。
        /// DrawingWorker 的 1 ms 轮询将在至多 1 ms 后取走并执行最新委托。
        /// </para>
        /// </summary>
        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(D3DFastImage));
            if (!_isInitialized) throw new InvalidOperationException("请先调用 Initialize。");
            if (_isDeviceLost) return;
            if (drawAction == null) throw new ArgumentNullException(nameof(drawAction));

            // 原子替换槽位：旧委托（若有）被丢弃时，新委托已是其后继，丢弃语义安全。
            Interlocked.Exchange(ref _pendingDrawAction, drawAction);
            // 无需唤醒信号：DrawingWorker 每 1 ms 自行轮询，无额外线程压力。
        }

        #endregion

        #region DrawingWorker 后台轮询

        /// <summary>
        /// DrawingWorker 主循环（在 ThreadPool 线程上运行）。
        /// 使用 <see cref="PeriodicTimer"/>（1 ms）轮询 <see cref="_pendingDrawAction"/> 槽：
        /// 有委托则取出绘制；无委托则等到下次 Tick，线程在等待期间被挂起（不自旋）。
        /// </summary>
        private async Task DrawingWorkerLoopAsync(CancellationToken token)
        {
            using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(1));
            try
            {
                // 等待下一个 1 ms Tick；取消时返回 false 并退出循环。
                // 若本次绘制耗时超过 1 ms，WaitForNextTickAsync 在调用时立即返回（不会堆积 Tick）。
                while (await timer.WaitForNextTickAsync(token).ConfigureAwait(false))
                {
                    if (_isDeviceLost || !_isInitialized) continue;

                    // 原子取出最新委托（若未到新的提交，槽为空则跳过）
                    var action = Interlocked.Exchange(ref _pendingDrawAction, null);
                    if (action == null) continue;

                    // 申请一个可绘制 RenderFrame
                    var frame = _pool.AcquireForDrawing();
                    if (frame == null)
                    {
                        // 帧池暂时无可用帧（三重缓冲下极少发生）：归还委托，下次 Tick 重试。
                        Interlocked.CompareExchange(ref _pendingDrawAction, action, null);
                        continue;
                    }

                    // 执行绘制委托
                    try
                    {
                        using var ctx = frame.OpenCanvas();
                        action(ctx);
                    } // ctx.Dispose() 内完成 Skia Flush + GPU 上传 + Pool 通知 → ReadyForPresent
                    catch
                    {
                        // 委托内部异常：强制归还帧，避免帧池泄漏
                        frame.TryTransitionTo(FrameState.Drawing, FrameState.Ready);
                    }
                }
            }catch(OperationCanceledException)
            {
                //什么都不用做，正常退出即可
            }
        }

        #endregion

        #region 呈现路径（UI 线程）

        /// <summary>
        /// CompositionTarget.Rendering 回调（UI 线程，~每 16.6 ms）。
        /// 主提交路径。
        /// </summary>
        private void OnCompositionTargetRendering(object? sender, EventArgs e)
        {
            if (_isDisposed || _isDeviceLost || !_isInitialized) return;
            TrySubmitFrame();
        }

        /// <summary>
        /// 尝试将 ReadyForPresent 帧提交到 D3DImage（UI 线程）。
        /// 由 <see cref="OnCompositionTargetRendering"/>（主路径）和 <see cref="_retryTimer"/>（辅助路径）共同调用。
        /// </summary>
        private void TrySubmitFrame()
        {
            if (_isDisposed || _isDeviceLost || !_isInitialized) return;

            var frame = _pool.TryAcquireForPresent();
            if (frame == null)
            {
                _retryTimer.Stop(); // 无帧可提交，停止辅助重试
                return;
            }

            if (!_d3dImage.TryLock(new Duration(TimeSpan.Zero)))
            {
                // D3D9 表面仍被合成器占用，归还帧并启动辅助重试
                _d3dImage.Unlock(); //这是WPF的一个bug，D3DIamge在TryLock失败时也必须调用Unlock，否则会导致后续TryLock永久失败
                _pool.ResetToReadyForPresent(frame);
                if (!_retryTimer.IsEnabled)
                    _retryTimer.Start();
                return;
            }

            // ── TryLock 成功，进入安全窗口 ──────────────────────────────────
            _retryTimer.Stop();
            try
            {
                // 回收上一帧（SetBackBuffer 后 WPF 释放对旧 D3D9 表面的引用）
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
            bool isAvailable = (bool)e.NewValue;

            if (!isAvailable)
            {
                _isDeviceLost = true;
                _uiDispatcher.BeginInvoke(() =>
                {
                    if (_isDisposed) return;
                    _retryTimer.Stop();
                    if (!StopDrawingWorker(WorkerShutdownTimeout))
                        return;

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
                        if (!_deviceManager.Initialize())
                            return;

                        if (_width > 0 && _height > 0)
                        {
                            RebuildResources(_width, _height);
                        }
                    }
                    catch
                    {
                        // 恢复失败，保持 DeviceLost 状态，等待下次机会
                        _isDeviceLost = true;
                    }
                });
            }
        }

        private void UnbindBackBuffer()
        {
            _d3dImage.Lock();
            try
            {
                _d3dImage.SetBackBuffer(D3DResourceType.IDirect3DSurface9, IntPtr.Zero);
            }
            finally
            {
                _d3dImage.Unlock();
            }
        }

        #endregion

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            CompositionTarget.Rendering -= OnCompositionTargetRendering;
            _d3dImage.IsFrontBufferAvailableChanged -= OnFrontBufferAvailableChanged;

            _retryTimer.Stop();

            // Dispose 阶段必须保证 Worker 彻底停机，避免后续释放资源时并发访问。
            StopDrawingWorker(Timeout.InfiniteTimeSpan);

            UnbindBackBuffer();

            _pool.Dispose();
            _deviceManager.Dispose();
        }

        private void RebuildResources(int width, int height)
        {
            if (!StopDrawingWorker(WorkerShutdownTimeout))
                throw new TimeoutException("绘制线程未在超时时间内退出，已中止 Resize 以避免并发释放资源。");

            _retryTimer.Stop();
            _isInitialized = false;

            UnbindBackBuffer();
            _pool.ReleaseResources();
            _pool.CreateResources(width, height);

            _width = width;
            _height = height;
            _isInitialized = true;
            _isDeviceLost = false;

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

            lock (_workerLock)
            {
                workerTask = _drawingWorkerTask;
                workerCts = _workerCts;
            }

            if (workerCts == null && workerTask == null)
                return true;

            workerCts?.Cancel();

            if (workerTask != null)
            {
                try
                {
                    if (timeout == Timeout.InfiniteTimeSpan)
                    {
                        workerTask.Wait();
                    }
                    else if (!workerTask.Wait(timeout))
                    {
                        return false;
                    }
                }
                catch (AggregateException ex) when (IsCancellationOnly(ex))
                {
                    // 取消导致的退出是预期路径。
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
