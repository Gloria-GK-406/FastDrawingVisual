using FastDrawingVisual.Rendering.Composition;
using System;
using System.Threading;
using System.Windows.Media;

namespace FastDrawingVisual.Rendering.DComp.Clock
{
    /// <summary>
    /// 基于 WPF CompositionTarget.Rendering 的后备帧时钟。
    /// </summary>
    internal sealed class CompositionTargetFrameClock : ICompositionFrameClock
    {
        private long _sequence;
        private bool _isRunning;
        private bool _isDisposed;

        public event EventHandler<FrameClockTickEventArgs>? Tick;

        public bool IsRunning => _isRunning;

        public void Start()
        {
            ThrowIfDisposed();
            if (_isRunning) return;

            CompositionTarget.Rendering += OnRendering;
            _isRunning = true;
        }

        public void Stop()
        {
            if (!_isRunning) return;

            CompositionTarget.Rendering -= OnRendering;
            _isRunning = false;
        }

        private void OnRendering(object? sender, EventArgs e)
        {
            var args = new FrameClockTickEventArgs(
                Interlocked.Increment(ref _sequence),
                DateTime.UtcNow);

            Tick?.Invoke(this, args);
        }

        private void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(CompositionTargetFrameClock));
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            Stop();
        }
    }
}
