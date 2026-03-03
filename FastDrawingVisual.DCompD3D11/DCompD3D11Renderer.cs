using FastDrawingVisual.Rendering;
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Proxy = FastDrawingVisual.NativeProxy.NativeProxy;

namespace FastDrawingVisual.DCompD3D11
{
    public sealed class DCompD3D11Renderer : IRenderer
    {
        private ContentControl? _hostElement;
        private DCompHostHwnd? _hwndHost;
        private object? _previousContent;
        private bool _contentInjected;
        private IntPtr _boundHwnd;

        private IntPtr _nativeRenderer;
        private IntPtr _proxyHandle;
        private bool _desktopTargetBound;
        private bool _swapChainBound;

        private int _width;
        private int _height;
        private double _phase;
        private bool _isInitialized;
        private bool _isRenderingHooked;
        private bool _isDisposed;

        public bool AttachToElement(ContentControl element)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            if (element == null) throw new ArgumentNullException(nameof(element));

            if (ReferenceEquals(_hostElement, element))
            {
                TryEnsurePresentationBinding();
                return true;
            }

            UnhookHostElement();

            _hostElement = element;
            _hostElement.Loaded += OnHostLoaded;
            _hostElement.Unloaded += OnHostUnloaded;

            _hwndHost = new DCompHostHwnd
            {
                HorizontalAlignment = HorizontalAlignment.Stretch,
                VerticalAlignment = VerticalAlignment.Stretch
            };
            _previousContent = _hostElement.Content;
            _hostElement.Content = _hwndHost;
            _contentInjected = true;

            TryEnsurePresentationBinding();
            return true;
        }

        public bool Initialize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");

            _width = width;
            _height = height;

            try
            {
                EnsureNativeRenderer();
                EnsureProxyCreated();
                EnsureRenderLoop();
                _isInitialized = true;
                TryEnsurePresentationBinding();
                return true;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"[DCompD3D11] Initialize failed: {ex}");
                _isInitialized = false;
                return false;
            }
        }

        public void Resize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");

            _width = width;
            _height = height;

            if (!_isInitialized || _nativeRenderer == IntPtr.Zero)
                return;

            if (!Proxy.Resize(_nativeRenderer, _width, _height))
                ThrowNativeFailure("FDV_Resize");

            TryEnsurePresentationBinding();
            UpdateProxyRect();
        }

        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            // Demo stage: keep a GPU clear animation to validate HwndHost + DComp proxy path.
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            if (_isRenderingHooked)
            {
                CompositionTarget.Rendering -= OnCompositionTargetRendering;
                _isRenderingHooked = false;
            }

            UnhookHostElement();

            DestroyProxy();
            DestroyNativeRenderer();
        }

        private void EnsureRenderLoop()
        {
            if (_isRenderingHooked)
                return;

            CompositionTarget.Rendering += OnCompositionTargetRendering;
            _isRenderingHooked = true;
        }

        private void EnsureNativeRenderer()
        {
            if (_nativeRenderer != IntPtr.Zero)
                return;

            if (!Proxy.IsBridgeReady())
                throw new InvalidOperationException("FDV_IsBridgeReady returned false.");

            _nativeRenderer = Proxy.CreateRenderer(IntPtr.Zero, _width, _height);
            if (_nativeRenderer == IntPtr.Zero)
                throw new InvalidOperationException("FDV_CreateRenderer returned null handle.");
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
                // Suppress teardown errors.
            }
            finally
            {
                _nativeRenderer = IntPtr.Zero;
                _swapChainBound = false;
            }
        }

        private void EnsureProxyCreated()
        {
            if (_proxyHandle != IntPtr.Zero)
                return;

            WinRTProxyNative.EnsureResolverRegistered();

            if (!WinRTProxyNative.FDV_WinRTProxy_IsReady())
                throw new InvalidOperationException("FDV_WinRTProxy_IsReady returned false.");

            _proxyHandle = WinRTProxyNative.FDV_WinRTProxy_Create();
            if (_proxyHandle == IntPtr.Zero)
                throw new InvalidOperationException("FDV_WinRTProxy_Create returned null handle.");

            if (!WinRTProxyNative.FDV_WinRTProxy_EnsureDispatcherQueue(_proxyHandle))
                ThrowProxyFailure("FDV_WinRTProxy_EnsureDispatcherQueue");
        }

        private void DestroyProxy()
        {
            if (_proxyHandle == IntPtr.Zero)
                return;

            try
            {
                WinRTProxyNative.FDV_WinRTProxy_Destroy(_proxyHandle);
            }
            catch
            {
                // Suppress teardown errors.
            }
            finally
            {
                _proxyHandle = IntPtr.Zero;
                _desktopTargetBound = false;
                _swapChainBound = false;
                _boundHwnd = IntPtr.Zero;
            }
        }

        private bool TryEnsurePresentationBinding()
        {
            if (_isDisposed || !_isInitialized || _nativeRenderer == IntPtr.Zero || _proxyHandle == IntPtr.Zero)
                return false;

            if (_hostElement == null || _hwndHost == null)
                return false;

            var hwnd = _hwndHost.HostHandle;
            if (hwnd == IntPtr.Zero)
                return false;

            try
            {
                if (!_desktopTargetBound || _boundHwnd != hwnd)
                {
                    if (!WinRTProxyNative.FDV_WinRTProxy_EnsureDesktopTarget(_proxyHandle, hwnd, false))
                        ThrowProxyFailure("FDV_WinRTProxy_EnsureDesktopTarget");

                    _desktopTargetBound = true;
                    _swapChainBound = false;
                    _boundHwnd = hwnd;
                }

                if (!_swapChainBound)
                {
                    IntPtr swapChain = IntPtr.Zero;
                    if (!Proxy.TryGetSwapChain(_nativeRenderer, ref swapChain) || swapChain == IntPtr.Zero)
                        ThrowNativeFailure("FDV_TryGetSwapChain");

                    if (!WinRTProxyNative.FDV_WinRTProxy_BindSwapChain(_proxyHandle, swapChain))
                        ThrowProxyFailure("FDV_WinRTProxy_BindSwapChain");

                    _swapChainBound = true;
                }

                UpdateProxyRect();
                return true;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"[DCompD3D11] Presentation binding failed: {ex}");
                return false;
            }
        }

        private void UpdateProxyRect()
        {
            if (_proxyHandle == IntPtr.Zero || !_desktopTargetBound || !_swapChainBound)
                return;

            var w = Math.Max(1, _width);
            var h = Math.Max(1, _height);

            if (!WinRTProxyNative.FDV_WinRTProxy_UpdateSpriteRect(_proxyHandle, 0f, 0f, w, h))
                ThrowProxyFailure("FDV_WinRTProxy_UpdateSpriteRect");
        }

        private void OnCompositionTargetRendering(object? sender, EventArgs e)
        {
            if (_isDisposed || !_isInitialized || _nativeRenderer == IntPtr.Zero)
                return;

            if (!TryEnsurePresentationBinding())
                return;

            _phase += 0.015;
            var red = (float)(0.5 + 0.5 * Math.Sin(_phase));
            var green = (float)(0.5 + 0.5 * Math.Sin(_phase + 2.0));
            var blue = (float)(0.5 + 0.5 * Math.Sin(_phase + 4.0));

            if (!Proxy.ClearAndPresent(_nativeRenderer, red, green, blue, 1.0f, 1))
                ThrowNativeFailure("FDV_ClearAndPresent");
        }

        private void OnHostLoaded(object? sender, RoutedEventArgs e)
        {
            TryEnsurePresentationBinding();
        }

        private void OnHostUnloaded(object? sender, RoutedEventArgs e)
        {
            // Keep resources alive; attach can recover when host loads again.
        }

        private void UnhookHostElement()
        {
            if (_hostElement != null)
            {
                _hostElement.Loaded -= OnHostLoaded;
                _hostElement.Unloaded -= OnHostUnloaded;
            }

            if (_contentInjected && _hostElement != null && _hwndHost != null)
            {
                if (ReferenceEquals(_hostElement.Content, _hwndHost))
                    _hostElement.Content = _previousContent;

                _contentInjected = false;
            }

            if (_hwndHost != null)
            {
                _hwndHost.Dispose();
                _hwndHost = null;
            }

            _hostElement = null;
            _previousContent = null;
            _boundHwnd = IntPtr.Zero;
            _desktopTargetBound = false;
            _swapChainBound = false;
        }

        private void ThrowProxyFailure(string operation)
        {
            var hr = _proxyHandle != IntPtr.Zero
                ? WinRTProxyNative.FDV_WinRTProxy_GetLastErrorHr(_proxyHandle)
                : unchecked((int)0x80004005);

            if (hr >= 0)
                hr = unchecked((int)0x80004005);

            throw new COMException($"{operation} failed with HRESULT=0x{hr:X8}", hr);
        }

        private void ThrowNativeFailure(string operation)
        {
            var hr = _nativeRenderer != IntPtr.Zero
                ? Proxy.GetLastErrorHr(_nativeRenderer)
                : unchecked((int)0x80004005);

            if (hr >= 0)
                hr = unchecked((int)0x80004005);

            throw new COMException($"{operation} failed with HRESULT=0x{hr:X8}", hr);
        }
    }
}
