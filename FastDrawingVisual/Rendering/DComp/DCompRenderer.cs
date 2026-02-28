using FastDrawingVisual.Rendering.Composition;
using System;
using System.Threading;
using System.Threading.Tasks;

namespace FastDrawingVisual.Rendering.DComp
{
    internal sealed class DCompRenderer : ICompositionRenderer
    {
        private readonly IGraphicsCompositionBackend _backend;
        private readonly ICompositionFrameClock _frameClock;
        private readonly object _workerLock = new();

        private volatile Action<IDrawingContext>? _pendingDrawAction;
        private CancellationTokenSource? _workerCts;
        private Task? _drawingWorkerTask;

        private long _presentFrameId;
        private bool _isInitialized;
        private bool _isDisposed;
        private int _width;
        private int _height;

        public DCompRenderer(IGraphicsCompositionBackend backend, ICompositionFrameClock frameClock)
        {
            _backend = backend ?? throw new ArgumentNullException(nameof(backend));
            _frameClock = frameClock ?? throw new ArgumentNullException(nameof(frameClock));
        }

        public bool Initialize(IntPtr hostHwnd, int width, int height)
        {
            ThrowIfDisposed();
            if (hostHwnd == IntPtr.Zero) return false;
            if (width <= 0 || height <= 0) return false;

            if (!_backend.Initialize(hostHwnd, width, height, bufferCount: 3))
                return false;

            _width = width;
            _height = height;
            _isInitialized = true;

            _frameClock.Tick += OnFrameClockTick;
            _frameClock.Start();
            StartDrawingWorker();
            return true;
        }

        public void Resize(int width, int height)
        {
            ThrowIfDisposed();
            if (!_isInitialized) return;
            if (width <= 0 || height <= 0) return;
            if (_width == width && _height == height) return;

            _width = width;
            _height = height;
            _backend.Resize(width, height);
        }

        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            ThrowIfDisposed();
            if (!_isInitialized) throw new InvalidOperationException("Call Initialize first.");
            if (drawAction == null) throw new ArgumentNullException(nameof(drawAction));

            Interlocked.Exchange(ref _pendingDrawAction, drawAction);
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            _frameClock.Tick -= OnFrameClockTick;
            _frameClock.Stop();
            StopDrawingWorker(Timeout.InfiniteTimeSpan);
            _frameClock.Dispose();
            _backend.Dispose();
        }

        private void OnFrameClockTick(object? sender, FrameClockTickEventArgs e)
        {
            if (_isDisposed || !_isInitialized)
                return;

            TryPresentFrame(e);
        }

        private void TryPresentFrame(FrameClockTickEventArgs e)
        {
            var frame = _backend.TryAcquireForPresent();
            if (frame == null)
                return;

            try
            {
                var info = new FramePresentationInfo(
                    Interlocked.Increment(ref _presentFrameId),
                    e.TimestampUtc);
                _backend.Present(frame, info);
            }
            finally
            {
                _backend.CompletePresent(frame);
            }
        }

        private async Task DrawingWorkerLoopAsync(CancellationToken token)
        {
            using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(1));

            try
            {
                while (await timer.WaitForNextTickAsync(token).ConfigureAwait(false))
                {
                    if (_isDisposed || !_isInitialized)
                        continue;

                    var action = Interlocked.Exchange(ref _pendingDrawAction, null);
                    if (action == null)
                        continue;

                    var frame = _backend.TryAcquireForDrawing();
                    if (frame == null)
                    {
                        Interlocked.CompareExchange(ref _pendingDrawAction, action, null);
                        continue;
                    }

                    var success = false;
                    try
                    {
                        using var ctx = frame.OpenDrawingContext();
                        action(ctx);
                        success = true;
                    }
                    catch
                    {
                    }
                    finally
                    {
                        _backend.CompleteDrawing(frame, success);
                    }
                }
            }
            catch (OperationCanceledException)
            {
            }
        }

        private void StartDrawingWorker()
        {
            lock (_workerLock)
            {
                if (_drawingWorkerTask is { IsCompleted: false })
                    return;

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
            {
                if (inner is not OperationCanceledException)
                    return false;
            }

            return true;
        }

        private void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(DCompRenderer));
        }
    }
}
