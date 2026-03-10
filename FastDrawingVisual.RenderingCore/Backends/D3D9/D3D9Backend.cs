extern alias D3D9Proxy;

using FastDrawingVisual.CommandRuntime;
using System;
using System.Windows;
using System.Windows.Interop;
using Proxy = D3D9Proxy::FastDrawingVisual.NativeProxy.NativeProxy;

namespace FastDrawingVisual.Rendering.Backends
{
    public sealed class D3D9Backend : IRenderBackend, ILayeredFrameSink, ID3D9SurfaceProvider, ID3D9PresentationController, IRenderBackendReadiness
    {
        private IntPtr _nativeRenderer;
        private HwndSource? _fallbackHwndSource;
        private int _width;
        private int _height;
        private bool _isInitialized;
        private bool _isDeviceLost;
        private bool _isDisposed;
        private bool _lastReadyState;

        public bool IsReadyForRendering => !_isDisposed && _isInitialized && !_isDeviceLost && _nativeRenderer != IntPtr.Zero;

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
                _nativeRenderer = CreateNativeRenderer(width, height);
                if (_nativeRenderer == IntPtr.Zero)
                {
                    UpdateReadyState();
                    return false;
                }
            }

            _isInitialized = true;
            _isDeviceLost = false;
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

            if (_nativeRenderer == IntPtr.Zero || !_isInitialized || _isDeviceLost)
            {
                UpdateReadyState();
                return;
            }

            SafeResizeNative(width, height);
            UpdateReadyState();
        }

        public void SubmitFrame(in BridgeLayeredFramePacket frame)
        {
            ThrowIfDisposed();
            if (!IsReadyForRendering || !frame.HasAnyCommands)
                return;

            try
            {
                for (int layerIndex = 0; layerIndex < BridgeLayeredFramePacket.MaxLayerCount; layerIndex++)
                {
                    var layer = frame.GetLayer(layerIndex);
                    if (!layer.HasCommands)
                        continue;

                    Proxy.SubmitCommands(
                        _nativeRenderer,
                        layer.CommandPointer,
                        layer.CommandBytes,
                        layer.BlobPointer,
                        layer.BlobBytes);
                }
            }
            catch
            {
                _isDeviceLost = true;
                UpdateReadyState();
            }
        }

        public IntPtr GetSurface9()
        {
            ThrowIfDisposed();
            if (!IsReadyForRendering)
                return IntPtr.Zero;

            try
            {
                var surface = IntPtr.Zero;
                return Proxy.TryAcquirePresentSurface(_nativeRenderer, ref surface) ? surface : IntPtr.Zero;
            }
            catch
            {
                _isDeviceLost = true;
                UpdateReadyState();
                return IntPtr.Zero;
            }
        }

        public bool CopyReadyToPresentSurface()
        {
            ThrowIfDisposed();
            if (!IsReadyForRendering)
                return false;

            try
            {
                return Proxy.CopyReadyToPresentSurface(_nativeRenderer);
            }
            catch
            {
                _isDeviceLost = true;
                UpdateReadyState();
                return false;
            }
        }

        public void NotifyFrontBufferAvailable(bool available)
        {
            ThrowIfDisposed();

            if (!available)
            {
                _isInitialized = false;
                _isDeviceLost = true;
                SafeNotifyFrontBufferAvailable(false);
                UpdateReadyState();
                return;
            }

            if (_width <= 0 || _height <= 0)
                return;

            if (_nativeRenderer == IntPtr.Zero)
            {
                _nativeRenderer = CreateNativeRenderer(_width, _height);
                if (_nativeRenderer == IntPtr.Zero)
                {
                    UpdateReadyState();
                    return;
                }
            }

            SafeNotifyFrontBufferAvailable(true);
            if (!SafeResizeNative(_width, _height))
            {
                UpdateReadyState();
                return;
            }

            _isDeviceLost = false;
            _isInitialized = true;
            UpdateReadyState();
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
            DestroyNativeRenderer();
            _fallbackHwndSource?.Dispose();
            _fallbackHwndSource = null;
            UpdateReadyState();
        }

        private IntPtr CreateNativeRenderer(int width, int height)
        {
            var hwnd = GetOrCreateDeviceHwnd();
            if (hwnd == IntPtr.Zero)
                return IntPtr.Zero;

            try
            {
                return Proxy.CreateRenderer(hwnd, width, height);
            }
            catch
            {
                return IntPtr.Zero;
            }
        }

        private bool SafeResizeNative(int width, int height)
        {
            if (_nativeRenderer == IntPtr.Zero)
                return false;

            try
            {
                if (!Proxy.Resize(_nativeRenderer, width, height))
                {
                    _isDeviceLost = true;
                    return false;
                }

                return true;
            }
            catch
            {
                _isDeviceLost = true;
                return false;
            }
        }

        private void SafeNotifyFrontBufferAvailable(bool available)
        {
            if (_nativeRenderer == IntPtr.Zero)
                return;

            try
            {
                Proxy.OnFrontBufferAvailable(_nativeRenderer, available);
            }
            catch
            {
                _isDeviceLost = true;
            }
        }

        private void DestroyNativeRenderer()
        {
            if (_nativeRenderer == IntPtr.Zero)
                return;

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
                _isInitialized = false;
                _isDeviceLost = false;
            }
        }

        private IntPtr GetOrCreateDeviceHwnd()
        {
            var mainWindow = Application.Current?.MainWindow;
            if (mainWindow != null)
            {
                var mainHwnd = new WindowInteropHelper(mainWindow).Handle;
                if (mainHwnd != IntPtr.Zero)
                    return mainHwnd;
            }

            if (_fallbackHwndSource == null)
            {
                var parameters = new HwndSourceParameters("FastDrawingVisual.D3D9Backend")
                {
                    Width = 1,
                    Height = 1,
                    WindowStyle = unchecked((int)0x80000000),
                };
                _fallbackHwndSource = new HwndSource(parameters);
            }

            return _fallbackHwndSource.Handle;
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
                throw new ObjectDisposedException(nameof(D3D9Backend));
        }
    }
}
