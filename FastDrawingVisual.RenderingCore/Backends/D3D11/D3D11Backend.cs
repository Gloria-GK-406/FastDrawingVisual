extern alias D3D11Proxy;

using FastDrawingVisual.CommandRuntime;
using System;
using Proxy = D3D11Proxy::FastDrawingVisual.NativeProxy.NativeProxy;

namespace FastDrawingVisual.Rendering.Backends
{
    public sealed class D3D11Backend : IRenderBackend, IDXGISwapChainProvider, IRenderBackendReadiness
    {
        private IntPtr _nativeRenderer;
        private int _width;
        private int _height;
        private bool _isInitialized;
        private bool _isFaulted;
        private bool _isDisposed;
        private bool _lastReadyState;

        public bool IsReadyForRendering => !_isDisposed && _isInitialized && !_isFaulted && _nativeRenderer != IntPtr.Zero;

        public event Action? ReadyStateChanged;

        public bool Initialize(int width, int height)
        {
            ThrowIfDisposed();
            if (width <= 0 || height <= 0)
                throw new ArgumentException("Width and height must be greater than zero.");

            _width = width;
            _height = height;

            if (_nativeRenderer == IntPtr.Zero)
            {
                if (!Proxy.IsBridgeReady())
                {
                    UpdateReadyState();
                    return false;
                }

                _nativeRenderer = Proxy.CreateRenderer(IntPtr.Zero, width, height);
                if (_nativeRenderer == IntPtr.Zero)
                {
                    UpdateReadyState();
                    return false;
                }
            }

            _isInitialized = true;
            _isFaulted = false;
            UpdateReadyState();
            return true;
        }

        public void Resize(int width, int height)
        {
            ThrowIfDisposed();
            if (width <= 0 || height <= 0)
                throw new ArgumentException("Width and height must be greater than zero.");

            _width = width;
            _height = height;

            if (_nativeRenderer == IntPtr.Zero || !_isInitialized || _isFaulted)
            {
                UpdateReadyState();
                return;
            }

            try
            {
                if (!Proxy.Resize(_nativeRenderer, width, height))
                    _isFaulted = true;
            }
            catch
            {
                _isFaulted = true;
            }

            UpdateReadyState();
        }

        public IDrawingContext? CreateDrawingContext(int width, int height)
        {
            ThrowIfDisposed();
            if (!IsReadyForRendering)
                return null;

            return new LayeredCommandRecordingContext(width, height, SubmitFrame);
        }

        private unsafe void SubmitFrame(BridgeLayeredFramePacket frame)
        {
            ThrowIfDisposed();
            if (!IsReadyForRendering || !frame.HasAnyCommands)
                return;

            try
            {
                var frameCopy = frame;
                if (!Proxy.SubmitLayeredCommands(_nativeRenderer, new IntPtr(&frameCopy), sizeof(BridgeLayeredFramePacket)))
                    _isFaulted = true;
            }
            catch
            {
                _isFaulted = true;
            }

            UpdateReadyState();
        }

        public IntPtr GetSwapChain()
        {
            ThrowIfDisposed();
            if (!IsReadyForRendering)
                return IntPtr.Zero;

            try
            {
                var swapChain = IntPtr.Zero;
                return Proxy.TryGetSwapChain(_nativeRenderer, ref swapChain) ? swapChain : IntPtr.Zero;
            }
            catch
            {
                _isFaulted = true;
                UpdateReadyState();
                return IntPtr.Zero;
            }
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
            if (_nativeRenderer != IntPtr.Zero)
            {
                try
                {
                    Proxy.DestroyRenderer(_nativeRenderer);
                }
                catch
                {
                }
                finally
                {
                    _nativeRenderer = IntPtr.Zero;
                }
            }

            _isInitialized = false;
            _isFaulted = false;
            UpdateReadyState();
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
                throw new ObjectDisposedException(nameof(D3D11Backend));
        }
    }
}
