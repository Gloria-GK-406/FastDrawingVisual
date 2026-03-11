using System;
using System.Threading;
using System.Threading.Tasks;

namespace FastDrawingVisual.Rendering
{
    public sealed class LatestWinsRenderWorker : IDisposable
    {
        private readonly Func<bool> _canExecute;
        private readonly Func<IDrawingContext?> _contextFactory;
        private readonly Action<Exception> _onExecutionFault;
        private readonly object _sync = new();
        private readonly SemaphoreSlim _signal = new(0, 1);
        private volatile Action<IDrawingContext>? _pendingDrawAction;
        private CancellationTokenSource? _cts;
        private Task? _workerTask;
        private int _signalState;
        private bool _isDisposed;

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

            lock (_sync)
            {
                if (_workerTask is { IsCompleted: false })
                    return;

                _cts?.Dispose();
                _cts = new CancellationTokenSource();
                _workerTask = Task.Run(() => WorkerLoopAsync(_cts.Token));
            }

            SignalIfPending();
        }

        public bool Stop(TimeSpan timeout)
        {
            Task? workerTask;
            CancellationTokenSource? cts;

            lock (_sync)
            {
                workerTask = _workerTask;
                cts = _cts;
            }

            if (workerTask == null && cts == null)
                return true;

            cts?.Cancel();

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

            lock (_sync)
            {
                if (ReferenceEquals(_workerTask, workerTask))
                    _workerTask = null;

                if (ReferenceEquals(_cts, cts))
                {
                    _cts?.Dispose();
                    _cts = null;
                }
            }

            return true;
        }

        public void Submit(Action<IDrawingContext> drawAction)
        {
            ThrowIfDisposed();
            if (drawAction == null)
                return;

            Interlocked.Exchange(ref _pendingDrawAction, drawAction);
            Signal();
        }

        public void SignalIfPending()
        {
            if (_pendingDrawAction != null)
                Signal();
        }

        public void Dispose()
        {
            if (_isDisposed)
                return;

            _isDisposed = true;
            Stop(Timeout.InfiniteTimeSpan);
            _signal.Dispose();
        }

        private async Task WorkerLoopAsync(CancellationToken token)
        {
            try
            {
                while (true)
                {
                    await _signal.WaitAsync(token).ConfigureAwait(false);
                    Interlocked.Exchange(ref _signalState, 0);

                    if (!_canExecute())
                        continue;

                    IDrawingContext? context;
                    try
                    {
                        context = _contextFactory();
                    }
                    catch (Exception ex)
                    {
                        SafeReportFault(ex);
                        continue;
                    }

                    if (context == null)
                        continue;

                    try
                    {
                        using (context)
                        {
                            var action = Interlocked.Exchange(ref _pendingDrawAction, null);
                            if (action == null)
                                continue;

                            action(context);
                        }
                    }
                    catch (Exception ex)
                    {
                        SafeReportFault(ex);
                    }
                }
            }
            catch (OperationCanceledException)
            {
            }
        }

        private void Signal()
        {
            if (Interlocked.Exchange(ref _signalState, 1) != 0)
                return;

            try
            {
                _signal.Release();
            }
            catch (SemaphoreFullException)
            {
            }
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
