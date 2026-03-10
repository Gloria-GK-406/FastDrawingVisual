using System;
using System.Windows;
using System.Windows.Controls;

namespace FastDrawingVisual.Rendering.Presentation
{
    public sealed class DCompPresenter : IRenderPresenter
    {
        private ContentControl? _hostElement;
        private DCompHostHwnd? _hwndHost;
        private object? _previousContent;
        private IDXGISwapChainProvider? _swapChainProvider;
        private IntPtr _proxyHandle;
        private IntPtr _boundHwnd;
        private bool _contentInjected;
        private bool _desktopTargetBound;
        private bool _swapChainBound;
        private bool _isDisposed;
        private bool _lastReadyState;
        private int _width;
        private int _height;

        public bool IsPresentationReady =>
            !_isDisposed &&
            _proxyHandle != IntPtr.Zero &&
            _desktopTargetBound &&
            _swapChainBound &&
            _boundHwnd != IntPtr.Zero;

        public event Action? ReadyStateChanged;

        public bool AttachToElement(ContentControl element, ICapabilityProvider capabilityProvider)
        {
            ThrowIfDisposed();
            if (element == null) throw new ArgumentNullException(nameof(element));
            if (capabilityProvider == null) throw new ArgumentNullException(nameof(capabilityProvider));

            if (!capabilityProvider.TryGetCapability<IDXGISwapChainProvider>(out var swapChainProvider) || swapChainProvider == null)
                return false;

            EnsureProxyCreated();
            _swapChainProvider = swapChainProvider;

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
            _hwndHost.HandleCreated += OnHostHandleCreated;
            _hwndHost.HandleDestroyed += OnHostHandleDestroyed;
            _previousContent = _hostElement.Content;
            _hostElement.Content = _hwndHost;
            _contentInjected = true;

            TryEnsurePresentationBinding();
            UpdateReadyState();
            return true;
        }

        public void Resize(int width, int height)
        {
            ThrowIfDisposed();
            _width = width;
            _height = height;
            UpdateProxyRect();
            UpdateReadyState();
        }

        public void Dispose()
        {
            if (_isDisposed)
                return;

            _isDisposed = true;
            UnhookHostElement();
            DestroyProxy();
            UpdateReadyState();
        }

        private void EnsureProxyCreated()
        {
            if (_proxyHandle != IntPtr.Zero)
                return;

            if (!WinRTProxyNative.FDV_WinRTProxy_IsReady())
                throw new InvalidOperationException("FDV_WinRTProxy_IsReady returned false.");

            _proxyHandle = WinRTProxyNative.FDV_WinRTProxy_Create();
            if (_proxyHandle == IntPtr.Zero)
                throw new InvalidOperationException("FDV_WinRTProxy_Create returned null handle.");

            if (!WinRTProxyNative.FDV_WinRTProxy_EnsureDispatcherQueue(_proxyHandle))
                throw new InvalidOperationException("FDV_WinRTProxy_EnsureDispatcherQueue failed.");
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
            }
            finally
            {
                _proxyHandle = IntPtr.Zero;
                _boundHwnd = IntPtr.Zero;
                _desktopTargetBound = false;
                _swapChainBound = false;
            }
        }

        private bool TryEnsurePresentationBinding()
        {
            if (_isDisposed || _proxyHandle == IntPtr.Zero || _hwndHost == null || _swapChainProvider == null)
                return false;

            var hwnd = _hwndHost.HostHandle;
            if (hwnd == IntPtr.Zero)
                return false;

            if (!_desktopTargetBound || _boundHwnd != hwnd)
            {
                if (!WinRTProxyNative.FDV_WinRTProxy_EnsureDesktopTarget(_proxyHandle, hwnd, false))
                {
                    UpdateReadyState();
                    return false;
                }

                _desktopTargetBound = true;
                _swapChainBound = false;
                _boundHwnd = hwnd;
            }

            if (!_swapChainBound)
            {
                var swapChain = _swapChainProvider.GetSwapChain();
                if (swapChain == IntPtr.Zero)
                {
                    UpdateReadyState();
                    return false;
                }

                if (!WinRTProxyNative.FDV_WinRTProxy_BindSwapChain(_proxyHandle, swapChain))
                {
                    UpdateReadyState();
                    return false;
                }

                _swapChainBound = true;
            }

            UpdateProxyRect();
            UpdateReadyState();
            return true;
        }

        private void UpdateProxyRect()
        {
            if (_proxyHandle == IntPtr.Zero || !_desktopTargetBound || !_swapChainBound)
                return;

            var width = Math.Max(1, _width);
            var height = Math.Max(1, _height);
            if (!WinRTProxyNative.FDV_WinRTProxy_UpdateSpriteRect(_proxyHandle, 0f, 0f, width, height))
            {
                _swapChainBound = false;
                UpdateReadyState();
            }
        }

        private void OnHostLoaded(object? sender, RoutedEventArgs e)
        {
            TryEnsurePresentationBinding();
        }

        private void OnHostUnloaded(object? sender, RoutedEventArgs e)
        {
            UpdateReadyState();
        }

        private void OnHostHandleCreated(IntPtr hwnd)
        {
            _ = hwnd;
            TryEnsurePresentationBinding();
        }

        private void OnHostHandleDestroyed()
        {
            _boundHwnd = IntPtr.Zero;
            _desktopTargetBound = false;
            _swapChainBound = false;
            UpdateReadyState();
        }

        private void UnhookHostElement()
        {
            if (_hostElement != null)
            {
                _hostElement.Loaded -= OnHostLoaded;
                _hostElement.Unloaded -= OnHostUnloaded;
            }

            if (_contentInjected && _hostElement != null && _hwndHost != null && ReferenceEquals(_hostElement.Content, _hwndHost))
            {
                _hostElement.Content = _previousContent;
                _contentInjected = false;
            }

            if (_hwndHost != null)
            {
                _hwndHost.HandleCreated -= OnHostHandleCreated;
                _hwndHost.HandleDestroyed -= OnHostHandleDestroyed;
                _hwndHost.Dispose();
                _hwndHost = null;
            }

            _hostElement = null;
            _previousContent = null;
            _boundHwnd = IntPtr.Zero;
            _desktopTargetBound = false;
            _swapChainBound = false;
            UpdateReadyState();
        }

        private void UpdateReadyState()
        {
            var isReady = IsPresentationReady;
            if (_lastReadyState == isReady)
                return;

            _lastReadyState = isReady;
            ReadyStateChanged?.Invoke();
        }

        private void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(DCompPresenter));
        }
    }
}
