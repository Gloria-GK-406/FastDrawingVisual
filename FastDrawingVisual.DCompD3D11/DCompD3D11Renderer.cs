using FastDrawingVisual.Rendering;
using Silk.NET.Core.Native;
using Silk.NET.Direct3D11;
using Silk.NET.DXGI;
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;

namespace FastDrawingVisual.DCompD3D11
{
    public sealed unsafe class DCompD3D11Renderer : IRenderer
    {
        private const uint BufferCount = 3;
        private const uint BgraSupportFlag = 0x20; // D3D11_CREATE_DEVICE_BGRA_SUPPORT
        private const uint DxgiUsageRenderTargetOutput = 0x20; // DXGI_USAGE_RENDER_TARGET_OUTPUT

        private ContentControl? _hostElement;
        private DCompHostHwnd? _hwndHost;
        private object? _previousContent;
        private bool _contentInjected;
        private IntPtr _boundHwnd;

        private D3D11? _d3d11Api;
        private DXGI? _dxgiApi;
        private ComPtr<ID3D11Device> _d3d11Device;
        private ComPtr<ID3D11DeviceContext> _d3d11Context;
        private ComPtr<IDXGIFactory2> _dxgiFactory;
        private ComPtr<IDXGISwapChain1> _swapChain;
        private ComPtr<ID3D11RenderTargetView> _rtv0;

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
                EnsureDeviceAndSwapChain();
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

            if (!_isInitialized || _swapChain.Handle == null)
                return;

            ReleaseRenderTargets();
            ThrowIfFailed(_swapChain.ResizeBuffers(BufferCount, (uint)_width, (uint)_height, (Format)87, 0), "IDXGISwapChain1.ResizeBuffers");
            CreateRenderTargets();

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

            ReleaseRenderTargets();
            _swapChain.Dispose();
            _swapChain = default;
            _dxgiFactory.Dispose();
            _dxgiFactory = default;
            _d3d11Context.Dispose();
            _d3d11Context = default;
            _d3d11Device.Dispose();
            _d3d11Device = default;
            _d3d11Api?.Dispose();
            _dxgiApi?.Dispose();
            _d3d11Api = null;
            _dxgiApi = null;
        }

        private void EnsureRenderLoop()
        {
            if (_isRenderingHooked)
                return;

            CompositionTarget.Rendering += OnCompositionTargetRendering;
            _isRenderingHooked = true;
        }

        private void EnsureDeviceAndSwapChain()
        {
            if (_d3d11Device.Handle != null && _swapChain.Handle != null)
                return;

            _d3d11Api ??= D3D11.GetApi();
            _dxgiApi ??= DXGI.GetApi();

            if (_d3d11Device.Handle == null)
            {
                var newDevice = default(ComPtr<ID3D11Device>);
                var newContext = default(ComPtr<ID3D11DeviceContext>);
                ThrowIfFailed(
                    _d3d11Api.CreateDevice(
                        (IDXGIAdapter*)null,
                        (D3DDriverType)1,
                        IntPtr.Zero,
                        BgraSupportFlag,
                        (D3DFeatureLevel*)null,
                        0,
                        D3D11.SdkVersion,
                        newDevice.GetAddressOf(),
                        (D3DFeatureLevel*)null,
                        newContext.GetAddressOf()),
                    "D3D11.CreateDevice");

                _d3d11Device = newDevice;
                _d3d11Context = newContext;
            }

            if (_dxgiFactory.Handle == null)
                ThrowIfFailed(_dxgiApi.CreateDXGIFactory2(0, out _dxgiFactory), "DXGI.CreateDXGIFactory2");

            if (_swapChain.Handle == null)
            {
                var swapDesc = new SwapChainDesc1
                {
                    Width = (uint)_width,
                    Height = (uint)_height,
                    Format = (Format)87, // DXGI_FORMAT_B8G8R8A8_UNORM
                    Stereo = 0,
                    SampleDesc = new SampleDesc(1, 0),
                    BufferUsage = DxgiUsageRenderTargetOutput,
                    BufferCount = BufferCount,
                    Scaling = (Scaling)0, // DXGI_SCALING_STRETCH
                    SwapEffect = (SwapEffect)3, // DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
                    AlphaMode = (AlphaMode)1, // DXGI_ALPHA_MODE_PREMULTIPLIED
                    Flags = 0
                };

                var newSwapChain = default(ComPtr<IDXGISwapChain1>);
                ThrowIfFailed(
                    _dxgiFactory.CreateSwapChainForComposition(
                        (IUnknown*)_d3d11Device.Handle,
                        &swapDesc,
                        (IDXGIOutput*)null,
                        newSwapChain.GetAddressOf()),
                    "IDXGIFactory2.CreateSwapChainForComposition");

                _swapChain = newSwapChain;
                CreateRenderTargets();
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
            if (_isDisposed || !_isInitialized || _swapChain.Handle == null || _proxyHandle == IntPtr.Zero)
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
                    if (!WinRTProxyNative.FDV_WinRTProxy_BindSwapChain(_proxyHandle, (IntPtr)_swapChain.Handle))
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
            if (_isDisposed || !_isInitialized || _swapChain.Handle == null || _d3d11Context.Handle == null)
                return;

            if (!TryEnsurePresentationBinding())
                return;

            var rtv = GetCurrentRenderTarget();
            if (rtv == null)
                return;

            _phase += 0.015;
            var clearColor = stackalloc float[4];
            clearColor[0] = (float)(0.5 + 0.5 * Math.Sin(_phase));
            clearColor[1] = (float)(0.5 + 0.5 * Math.Sin(_phase + 2.0));
            clearColor[2] = (float)(0.5 + 0.5 * Math.Sin(_phase + 4.0));
            clearColor[3] = 1.0f;

            _d3d11Context.OMSetRenderTargets(1, &rtv, (ID3D11DepthStencilView*)null);
            _d3d11Context.ClearRenderTargetView(rtv, clearColor);

            ThrowIfFailed(_swapChain.Present(1, 0), "IDXGISwapChain1.Present");
        }

        private void CreateRenderTargets()
        {
            const uint backBufferIndex = 0;
            var backBuffer = default(ComPtr<ID3D11Resource>);
            try
            {
                ThrowIfFailed(
                    _swapChain.GetBuffer(backBufferIndex, out backBuffer),
                    "IDXGISwapChain1.GetBuffer(bufferIndex=0)");

                if (backBuffer.Handle == null)
                    throw new InvalidOperationException("GetBuffer returned null handle for bufferIndex=0.");

                var newRtv = default(ComPtr<ID3D11RenderTargetView>);
                ThrowIfFailed(
                    _d3d11Device.CreateRenderTargetView(backBuffer.Handle, (RenderTargetViewDesc*)null, newRtv.GetAddressOf()),
                    "ID3D11Device.CreateRenderTargetView(bufferIndex=0)");

                _rtv0.Dispose();
                _rtv0 = newRtv;
            }
            finally
            {
                backBuffer.Dispose();
            }
        }

        private void ReleaseRenderTargets()
        {
            _rtv0.Dispose();
            _rtv0 = default;
        }

        private ID3D11RenderTargetView* GetCurrentRenderTarget()
        {
            return _rtv0.Handle;
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

        private static void ThrowIfFailed(int hr, string operation)
        {
            if (hr >= 0)
                return;

            throw new COMException($"{operation} failed with HRESULT=0x{hr:X8}", hr);
        }
    }
}
