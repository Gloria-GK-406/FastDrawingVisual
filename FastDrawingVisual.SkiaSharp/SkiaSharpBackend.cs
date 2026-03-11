using FastDrawingVisual.Rendering.D3D;
using System;
using System.Threading;

namespace FastDrawingVisual.Rendering.Backends
{
    public sealed class SkiaSharpBackend : IRenderBackend, ID3D9PresentationSource, IRenderBackendReadiness
    {
        private static readonly TimeSpan ActiveContextDrainTimeout = TimeSpan.FromSeconds(2);

        private readonly D3DDeviceManager _deviceManager;
        private readonly RenderFramePool _pool;
        private readonly ManualResetEventSlim _noActiveDrawingContexts = new(initialState: true);
        private int _activeDrawingContexts;
        private int _width;
        private int _height;
        private bool _isInitialized;
        private bool _isDeviceLost;
        private bool _isDisposed;
        private bool _lastReadyState;

        public SkiaSharpBackend()
        {
            _deviceManager = new D3DDeviceManager();
            _pool = new RenderFramePool(_deviceManager, OnDrawingFrameAvailable);
        }

        public bool IsReadyForRendering =>
            !_isDisposed &&
            _isInitialized &&
            !_isDeviceLost &&
            _deviceManager.IsInitialized &&
            _pool.HasAvailableFrameForDrawing();

        public event Action? ReadyStateChanged;

        public bool Initialize(int width, int height)
        {
            ThrowIfDisposed();
            ValidateSize(width, height);

            _width = width;
            _height = height;

            if (!EnsureDeviceManagerInitialized())
            {
                UpdateReadyState();
                return false;
            }

            if (!WaitForIdleDrawingContexts(ActiveContextDrainTimeout))
            {
                UpdateReadyState();
                return false;
            }

            return RecreateResources(width, height);
        }

        public void Resize(int width, int height)
        {
            ThrowIfDisposed();
            ValidateSize(width, height);

            if (width == _width && height == _height)
                return;

            _width = width;
            _height = height;

            if (_isDeviceLost || !_isInitialized)
            {
                UpdateReadyState();
                return;
            }

            if (!WaitForIdleDrawingContexts(ActiveContextDrainTimeout))
                throw new TimeoutException("Skia drawing context did not drain before resize.");

            if (!RecreateResources(width, height))
                throw new InvalidOperationException("Skia backend failed to rebuild resources during resize.");
        }

        public IDrawingContext? CreateDrawingContext(int width, int height)
        {
            ThrowIfDisposed();
            if (!IsReadyForRendering || width <= 0 || height <= 0)
                return null;

            var frame = _pool.AcquireForDrawing();
            UpdateReadyState();
            if (frame == null)
                return null;

            MarkDrawingContextOpened();
            try
            {
                return frame.OpenCanvas(OnDrawingContextClosed);
            }
            catch
            {
                frame.TryTransitionTo(FrameState.Drawing, FrameState.Ready);
                OnDrawingContextClosed();
                UpdateReadyState();
                return null;
            }
        }

        public IntPtr GetSurface9()
        {
            ThrowIfDisposed();
            if (_isDeviceLost || !_isInitialized)
                return IntPtr.Zero;

            return _pool.GetPresentingSurfacePointer();
        }

        public bool CopyReadyToPresentSurface()
        {
            ThrowIfDisposed();
            if (_isDeviceLost || !_isInitialized)
                return false;

            return _pool.CopyReadyToPresenting();
        }

        public void NotifyFrontBufferAvailable(bool available)
        {
            ThrowIfDisposed();

            if (!available)
            {
                _isDeviceLost = true;
                _isInitialized = false;

                if (WaitForIdleDrawingContexts(ActiveContextDrainTimeout))
                    _pool.ReleaseResources();

                UpdateReadyState();
                return;
            }

            if (_width <= 0 || _height <= 0)
            {
                UpdateReadyState();
                return;
            }

            if (!WaitForIdleDrawingContexts(ActiveContextDrainTimeout))
            {
                UpdateReadyState();
                return;
            }

            if (!EnsureDeviceManagerInitialized())
            {
                UpdateReadyState();
                return;
            }

            RecreateResources(_width, _height);
        }

        public bool TryGetCapability<TCapability>(out TCapability? capability)
            where TCapability : class
        {
            capability = this as TCapability;
            return capability != null;
        }

        public void Dispose()
        {
            if (_isDisposed)
                return;

            _isDisposed = true;
            WaitForIdleDrawingContexts(Timeout.InfiniteTimeSpan);
            _pool.Dispose();
            _deviceManager.Dispose();
            _noActiveDrawingContexts.Dispose();
            _isInitialized = false;
            _isDeviceLost = false;
            UpdateReadyState();
        }

        private void OnDrawingFrameAvailable()
        {
            UpdateReadyState();
        }

        private void OnDrawingContextClosed()
        {
            if (Interlocked.Decrement(ref _activeDrawingContexts) == 0)
                _noActiveDrawingContexts.Set();
        }

        private void MarkDrawingContextOpened()
        {
            if (Interlocked.Increment(ref _activeDrawingContexts) == 1)
                _noActiveDrawingContexts.Reset();
        }

        private bool EnsureDeviceManagerInitialized()
        {
            return _deviceManager.IsInitialized || _deviceManager.Initialize();
        }

        private bool RecreateResources(int width, int height)
        {
            try
            {
                _pool.ReleaseResources();
                _pool.CreateResources(width, height);
                _isInitialized = true;
                _isDeviceLost = false;
                UpdateReadyState();
                return true;
            }
            catch
            {
                _pool.ReleaseResources();
                _isInitialized = false;
                _isDeviceLost = true;
                UpdateReadyState();
                return false;
            }
        }

        private bool WaitForIdleDrawingContexts(TimeSpan timeout)
        {
            if (timeout == Timeout.InfiniteTimeSpan)
                return _noActiveDrawingContexts.Wait(Timeout.Infinite);

            return _noActiveDrawingContexts.Wait(timeout);
        }

        private void UpdateReadyState()
        {
            var isReady = IsReadyForRendering;
            if (_lastReadyState == isReady)
                return;

            _lastReadyState = isReady;
            ReadyStateChanged?.Invoke();
        }

        private void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(SkiaSharpBackend));
        }

        private static void ValidateSize(int width, int height)
        {
            if (width <= 0 || height <= 0)
                throw new ArgumentException("Width and height must be greater than zero.");
        }
    }
}
