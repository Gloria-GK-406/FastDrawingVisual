using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Threading;

namespace FastDrawingVisual.Rendering
{
    public sealed class LatestWinsRenderWorker : IDisposable
    {
        private static readonly SharedRenderScheduler s_scheduler = new();

        private readonly Func<bool> _canExecute;
        private readonly Func<IDrawingContext?> _contextFactory;
        private readonly Action<Exception> _onExecutionFault;
        private readonly ManualResetEventSlim _idleEvent = new(initialState: true);
        private volatile Action<IDrawingContext>? _pendingDrawAction;
        private int _queuedState;
        private int _executingState;
        private volatile bool _isStarted;
        private volatile bool _isDisposed;

        public LatestWinsRenderWorker(
            Func<bool> canExecute,
            Func<IDrawingContext?> contextFactory,
            Action<Exception> onExecutionFault)
        {
            _canExecute = canExecute ?? throw new ArgumentNullException(nameof(canExecute));
            _contextFactory = contextFactory ?? throw new ArgumentNullException(nameof(contextFactory));
            _onExecutionFault = onExecutionFault ?? throw new ArgumentNullException(nameof(onExecutionFault));
        }

        public void Start()
        {
            ThrowIfDisposed();
            _isStarted = true;
            SignalIfPending();
        }

        public bool Stop(TimeSpan timeout)
        {
            _isStarted = false;
            return WaitForIdle(timeout);
        }

        public void Submit(Action<IDrawingContext> drawAction)
        {
            ThrowIfDisposed();
            if (drawAction == null)
                return;

            Interlocked.Exchange(ref _pendingDrawAction, drawAction);
            QueueIfNeeded();
        }

        public void SignalIfPending()
        {
            if (_pendingDrawAction != null)
                QueueIfNeeded();
        }

        public void Dispose()
        {
            if (_isDisposed)
                return;

            _isDisposed = true;
            Stop(Timeout.InfiniteTimeSpan);
            _idleEvent.Dispose();
        }

        private void ExecuteScheduled()
        {
            Interlocked.Exchange(ref _queuedState, 0);

            if (_isDisposed || !_isStarted)
                return;

            if (Interlocked.CompareExchange(ref _executingState, 1, 0) != 0)
            {
                QueueIfNeeded();
                return;
            }

            _idleEvent.Reset();

            try
            {
                if (!_canExecute())
                    return;

                IDrawingContext? context;
                try
                {
                    context = _contextFactory();
                }
                catch (Exception ex)
                {
                    SafeReportFault(ex);
                    return;
                }

                if (context == null)
                    return;

                try
                {
                    using (context)
                    {
                        var action = Interlocked.Exchange(ref _pendingDrawAction, null);
                        if (action == null)
                            return;

                        action(context);
                    }
                }
                catch (Exception ex)
                {
                    SafeReportFault(ex);
                }
            }
            finally
            {
                Interlocked.Exchange(ref _executingState, 0);
                _idleEvent.Set();

                if (!_isDisposed && _isStarted && _pendingDrawAction != null)
                    QueueIfNeeded();
            }
        }

        private void QueueIfNeeded()
        {
            if (_isDisposed || !_isStarted)
                return;

            if (Interlocked.Exchange(ref _queuedState, 1) != 0)
                return;

            s_scheduler.Enqueue(this);
        }

        private void SafeReportFault(Exception ex)
        {
            try
            {
                _onExecutionFault(ex);
            }
            catch
            {
            }
        }

        private void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(LatestWinsRenderWorker));
        }

        private bool WaitForIdle(TimeSpan timeout)
        {
            if (timeout == Timeout.InfiniteTimeSpan)
            {
                while (Volatile.Read(ref _executingState) != 0)
                    _idleEvent.Wait();
                return true;
            }

            long timeoutMs = (long)timeout.TotalMilliseconds;
            if (timeoutMs < -1)
                throw new ArgumentOutOfRangeException(nameof(timeout));

            int boundedTimeoutMs = timeoutMs > int.MaxValue ? int.MaxValue : (int)timeoutMs;
            var stopwatch = Stopwatch.StartNew();

            while (Volatile.Read(ref _executingState) != 0)
            {
                int remainingMs = boundedTimeoutMs - (int)stopwatch.ElapsedMilliseconds;
                if (remainingMs <= 0)
                    return false;

                if (!_idleEvent.Wait(remainingMs))
                    return false;
            }

            return true;
        }

        private sealed class SharedRenderScheduler
        {
            private readonly ConcurrentQueue<LatestWinsRenderWorker> _queue = new();
            private readonly SemaphoreSlim _signal = new(0);

            public SharedRenderScheduler()
            {
                int workerCount = Math.Clamp(Environment.ProcessorCount, 1, 8);
                for (int i = 0; i < workerCount; i++)
                {
                    var thread = new Thread(WorkerLoop)
                    {
                        IsBackground = true,
                        Name = $"FDV.RenderScheduler.{i + 1}"
                    };
                    thread.Start();
                }
            }

            public void Enqueue(LatestWinsRenderWorker worker)
            {
                _queue.Enqueue(worker);
                _signal.Release();
            }

            private void WorkerLoop()
            {
                while (true)
                {
                    _signal.Wait();

                    if (!_queue.TryDequeue(out var worker))
                        continue;

                    worker.ExecuteScheduled();
                }
            }
        }
    }
}
