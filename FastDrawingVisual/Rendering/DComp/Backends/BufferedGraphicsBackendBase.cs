using FastDrawingVisual.Rendering.Composition;
using System;
using System.Linq;

namespace FastDrawingVisual.Rendering.DComp.Backends
{
    internal abstract class BufferedGraphicsBackendBase : IGraphicsCompositionBackend
    {
        private readonly object _sync = new();
        private BackendFrame[] _frames = Array.Empty<BackendFrame>();
        private int _currentPresentingIndex = -1;
        private long _drawSequence;
        private bool _isDisposed;
        private IntPtr _hostHwnd;

        protected int Width { get; private set; }

        protected int Height { get; private set; }

        protected IntPtr HostHwnd => _hostHwnd;

        public abstract GraphicsBackendKind Kind { get; }

        public bool Initialize(IntPtr hostHwnd, int width, int height, int bufferCount)
        {
            ThrowIfDisposed();
            if (hostHwnd == IntPtr.Zero) return false;
            if (width <= 0 || height <= 0) return false;
            if (bufferCount < 2) return false;

            lock (_sync)
            {
                ReleaseFrames();

                _hostHwnd = hostHwnd;
                Width = width;
                Height = height;
                _drawSequence = 0;
                _currentPresentingIndex = -1;

                _frames = Enumerable.Range(0, bufferCount)
                    .Select(index => CreateFrame(index, width, height))
                    .ToArray();
            }

            return InitializeCore(hostHwnd, width, height, bufferCount);
        }

        public void Resize(int width, int height)
        {
            ThrowIfDisposed();
            if (width <= 0 || height <= 0) return;

            lock (_sync)
            {
                Width = width;
                Height = height;
                foreach (var frame in _frames)
                {
                    frame.Width = width;
                    frame.Height = height;
                }
            }

            ResizeCore(width, height);
        }

        public ICompositionFrame? TryAcquireForDrawing()
        {
            ThrowIfDisposed();

            lock (_sync)
            {
                var frame = _frames.FirstOrDefault(f => f.State == BackendFrameState.Ready);
                if (frame == null) return null;

                frame.State = BackendFrameState.Drawing;
                return frame;
            }
        }

        public void CompleteDrawing(ICompositionFrame frame, bool success)
        {
            ThrowIfDisposed();
            var backendFrame = GetFrame(frame);

            lock (_sync)
            {
                if (backendFrame.State != BackendFrameState.Drawing)
                    return;

                if (!success)
                {
                    backendFrame.State = BackendFrameState.Ready;
                    return;
                }

                backendFrame.DrawSequence = ++_drawSequence;
                backendFrame.State = BackendFrameState.ReadyForPresent;
                // Keep only the latest presentable frame to minimize latency.
                foreach (var other in _frames)
                {
                    if (ReferenceEquals(other, backendFrame)) continue;
                    if (other.State == BackendFrameState.ReadyForPresent)
                        other.State = BackendFrameState.Ready;
                }
            }
        }

        public ICompositionFrame? TryAcquireForPresent()
        {
            ThrowIfDisposed();

            lock (_sync)
            {
                var candidate = _frames
                    .Where(f => f.State == BackendFrameState.ReadyForPresent)
                    .OrderByDescending(f => f.DrawSequence)
                    .FirstOrDefault();

                if (candidate == null) return null;

                candidate.State = BackendFrameState.Presenting;
                _currentPresentingIndex = candidate.Index;
                return candidate;
            }
        }

        public void Present(ICompositionFrame frame, FramePresentationInfo info)
        {
            ThrowIfDisposed();
            var backendFrame = GetFrame(frame);
            PresentCore(backendFrame, info);
        }

        public void CompletePresent(ICompositionFrame frame)
        {
            ThrowIfDisposed();
            var backendFrame = GetFrame(frame);

            lock (_sync)
            {
                if (backendFrame.State == BackendFrameState.Presenting)
                    backendFrame.State = BackendFrameState.Ready;

                if (_currentPresentingIndex == backendFrame.Index)
                    _currentPresentingIndex = -1;

                foreach (var other in _frames)
                {
                    if (ReferenceEquals(other, backendFrame)) continue;
                    if (other.State == BackendFrameState.Presenting)
                        other.State = BackendFrameState.Ready;
                }
            }
        }

        protected virtual bool InitializeCore(IntPtr hostHwnd, int width, int height, int bufferCount) => true;

        protected virtual void ResizeCore(int width, int height)
        {
        }

        protected virtual IDrawingContext OpenDrawingContextCore(BackendFrame frame)
            => new NullDrawingContext(frame.Width, frame.Height);

        protected virtual void PresentCore(BackendFrame frame, FramePresentationInfo info)
        {
        }

        protected virtual IntPtr GetNativeSurfaceHandle(BackendFrame frame) => IntPtr.Zero;

        private BackendFrame CreateFrame(int index, int width, int height)
            => new(this, index, width, height);

        private BackendFrame GetFrame(ICompositionFrame frame)
        {
            if (frame is not BackendFrame backendFrame || !ReferenceEquals(backendFrame.Owner, this))
                throw new InvalidOperationException("The frame does not belong to current backend instance.");

            return backendFrame;
        }

        private void ReleaseFrames()
        {
            _frames = Array.Empty<BackendFrame>();
            _currentPresentingIndex = -1;
            _drawSequence = 0;
        }

        protected void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(GetType().Name);
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            ReleaseFrames();
            DisposeCore();
        }

        protected virtual void DisposeCore()
        {
        }

        protected enum BackendFrameState : byte
        {
            Ready = 0,
            Drawing = 1,
            ReadyForPresent = 2,
            Presenting = 3
        }

        protected sealed class BackendFrame : ICompositionFrame
        {
            internal BackendFrame(BufferedGraphicsBackendBase owner, int index, int width, int height)
            {
                Owner = owner;
                Index = index;
                Width = width;
                Height = height;
                State = BackendFrameState.Ready;
            }

            internal BufferedGraphicsBackendBase Owner { get; }

            internal int Index { get; }

            internal long DrawSequence { get; set; }

            internal BackendFrameState State { get; set; }

            public int Width { get; internal set; }

            public int Height { get; internal set; }

            public IntPtr NativeSurfaceHandle => Owner.GetNativeSurfaceHandle(this);

            public IDrawingContext OpenDrawingContext()
                => Owner.OpenDrawingContextCore(this);
        }
    }
}
