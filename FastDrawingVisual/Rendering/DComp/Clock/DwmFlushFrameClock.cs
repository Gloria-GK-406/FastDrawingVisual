using FastDrawingVisual.Rendering.Composition;
using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Threading;

namespace FastDrawingVisual.Rendering.DComp.Clock
{
    /// <summary>
    /// 基于 DwmFlush 的帧时钟。Tick 与 DWM 合成节奏对齐。
    /// </summary>
    internal sealed class DwmFlushFrameClock : ICompositionFrameClock
    {
        private readonly Dispatcher _dispatcher;

        private CancellationTokenSource? _cts;
        private Task? _clockTask;
        private long _sequence;
        private bool _isDisposed;

        public DwmFlushFrameClock(Dispatcher dispatcher)
        {
            _dispatcher = dispatcher ?? throw new ArgumentNullException(nameof(dispatcher));
        }

        public event EventHandler<FrameClockTickEventArgs>? Tick;

        public bool IsRunning => _clockTask is { IsCompleted: false };

        public void Start()
        {
            ThrowIfDisposed();
            if (IsRunning) return;

            _cts = new CancellationTokenSource();
            var token = _cts.Token;
            _clockTask = Task.Run(() => ClockLoopAsync(token), token);
        }

        public void Stop()
        {
            var cts = _cts;
            var task = _clockTask;

            if (cts == null && task == null)
                return;

            try
            {
                cts?.Cancel();
                task?.Wait(TimeSpan.FromMilliseconds(250));
            }
            catch (AggregateException ex) when (IsCancellationOnly(ex))
            {
            }
            finally
            {
                cts?.Dispose();
                _cts = null;
                _clockTask = null;
            }
        }

        private async Task ClockLoopAsync(CancellationToken token)
        {
            while (!token.IsCancellationRequested)
            {
                var hr = DwmFlush();
                if (hr != 0)
                {
                    await Task.Delay(16, token).ConfigureAwait(false);
                }

                var args = new FrameClockTickEventArgs(
                    Interlocked.Increment(ref _sequence),
                    DateTime.UtcNow);

                _ = _dispatcher.BeginInvoke(DispatcherPriority.Render, new Action(() =>
                {
                    if (_isDisposed) return;
                    Tick?.Invoke(this, args);
                }));
            }
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
                throw new ObjectDisposedException(nameof(DwmFlushFrameClock));
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            Stop();
        }

        [DllImport("dwmapi.dll")]
        private static extern int DwmFlush();
    }
}
