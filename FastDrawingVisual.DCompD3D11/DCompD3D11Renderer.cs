using FastDrawingVisual.Rendering;
using Silk.NET.Core.Native;
using Silk.NET.Direct3D11;
using Silk.NET.DXGI;
using System.Numerics;
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using Windows.UI.Composition;
using Windows.UI.Composition.Desktop;

namespace FastDrawingVisual.DCompD3D11
{
    public sealed unsafe class DCompD3D11Renderer : IRenderer
    {
        private const uint BufferCount = 3;
        private const uint BgraSupportFlag = 0x20; // D3D11_CREATE_DEVICE_BGRA_SUPPORT
        private const uint DxgiUsageRenderTargetOutput = 0x20; // DXGI_USAGE_RENDER_TARGET_OUTPUT

        private FrameworkElement? _hostElement;
        private Window? _hostWindow;
        private IntPtr _windowHwnd;

        private D3D11? _d3d11Api;
        private DXGI? _dxgiApi;
        private ComPtr<ID3D11Device> _d3d11Device;
        private ComPtr<ID3D11DeviceContext> _d3d11Context;
        private ComPtr<IDXGIFactory2> _dxgiFactory;
        private ComPtr<IDXGISwapChain1> _swapChain;
        private ComPtr<ID3D11RenderTargetView> _rtv0;

        private Compositor? _compositor;
        private DesktopWindowTarget? _desktopTarget;
        private Windows.UI.Composition.ContainerVisual? _rootVisual;
        private SpriteVisual? _spriteVisual;
        private ICompositionSurface? _compositionSurface;
        private CompositionSurfaceBrush? _surfaceBrush;
        private IntPtr _dispatcherQueueController;

        private int _width;
        private int _height;
        private double _phase;
        private bool _isInitialized;
        private bool _isRenderingHooked;
        private bool _isDisposed;

        public bool AttachToElement(FrameworkElement element)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            if (element == null) throw new ArgumentNullException(nameof(element));

            if (!ReferenceEquals(_hostElement, element))
            {
                UnhookHostElement();
                _hostElement = element;
                _hostElement.Loaded += OnHostLoaded;
                _hostElement.Unloaded += OnHostUnloaded;
                _hostElement.LayoutUpdated += OnHostLayoutUpdated;
            }

            if (_isInitialized)
                TryFinalizePresentationBinding();

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
                _isInitialized = true;
                TryFinalizePresentationBinding();
                return true;
            }
            catch
            {
                _isInitialized = false;
                return false;
            }
        }

        public void Resize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");
            if (!_isInitialized)
            {
                _width = width;
                _height = height;
                return;
            }
            if (_width == width && _height == height) return;

            _width = width;
            _height = height;

            if (_swapChain.Handle == null)
            {
                TryFinalizePresentationBinding();
                return;
            }

            ReleaseRenderTargets();
            ThrowIfFailed(_swapChain.ResizeBuffers(BufferCount, (uint)width, (uint)height, (Format)87, 0), "IDXGISwapChain1.ResizeBuffers");
            CreateRenderTargets();
            TryFinalizePresentationBinding();
            UpdateSpritePlacement();
        }

        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            // Demo pass: this renderer currently drives a GPU clear animation to validate
            // DComp + D3D11 triple-buffer presentation, so drawing commands are ignored.
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            if (_isRenderingHooked)
            {
                System.Windows.Media.CompositionTarget.Rendering -= OnCompositionTargetRendering;
                _isRenderingHooked = false;
            }
            UnhookHostElement();
            UnhookHostWindow();

            if (_desktopTarget != null)
                _desktopTarget.Root = null;

            _surfaceBrush = null;
            _compositionSurface = null;
            _spriteVisual = null;
            _rootVisual = null;
            _desktopTarget = null;
            _compositor = null;
            if (_dispatcherQueueController != IntPtr.Zero)
            {
                Marshal.Release(_dispatcherQueueController);
                _dispatcherQueueController = IntPtr.Zero;
            }

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
                var createdDesc = default(SwapChainDesc1);
                ThrowIfFailed(_swapChain.GetDesc1(&createdDesc), "IDXGISwapChain1.GetDesc1");
                Debug.WriteLine($"[DCompD3D11] SwapChain created BufferCount={createdDesc.BufferCount}, Size={createdDesc.Width}x{createdDesc.Height}");
                CreateRenderTargets();
            }
        }

        private void EnsureCompositionTree()
        {
            if (_compositor != null)
                return;

            EnsureDispatcherQueue();
            _compositor = new Compositor();
            var interop = _compositor as ICompositorDesktopInterop;
            _compositor.CreateTargetForCurrentView();

            var desktopInterop = GetComInterface<ICompositorDesktopInterop>(_compositor);
            ThrowIfFailed(
                desktopInterop.CreateDesktopWindowTarget(_windowHwnd, true, out var targetObject),
                "ICompositorDesktopInterop.CreateDesktopWindowTarget");
            _desktopTarget = (DesktopWindowTarget)targetObject;

            _rootVisual = _compositor.CreateContainerVisual();
            _spriteVisual = _compositor.CreateSpriteVisual();
            _rootVisual.Children.InsertAtTop(_spriteVisual);
            _desktopTarget.Root = _rootVisual;
        }

        private void EnsureDispatcherQueue()
        {
            if (Windows.System.DispatcherQueue.GetForCurrentThread() != null)
                return;

            if (_dispatcherQueueController != IntPtr.Zero)
                return;

            var options = new DispatcherQueueOptions
            {
                DwSize = (uint)Marshal.SizeOf<DispatcherQueueOptions>(),
                ThreadType = (int)DispatcherQueueThreadType.Current,
                ApartmentType = (int)DispatcherQueueThreadApartmentType.Sta
            };

            ThrowIfFailed(
                DispatcherQueueInterop.CreateDispatcherQueueController(options, out _dispatcherQueueController),
                "CreateDispatcherQueueController");

            if (_dispatcherQueueController == IntPtr.Zero)
                throw new InvalidOperationException("CreateDispatcherQueueController returned null controller.");

            Debug.WriteLine($"[DCompD3D11] DispatcherQueue initialized, controller=0x{_dispatcherQueueController.ToInt64():X}");
        }

        private void EnsureSurfaceBinding()
        {
            if (_compositor == null || _spriteVisual == null || _swapChain.Handle == null)
                return;

            if (_surfaceBrush != null)
                return;

            var compositorInterop = GetComInterface<ICompositorInterop>(_compositor);
            ThrowIfFailed(
                compositorInterop.CreateCompositionSurfaceForSwapChain((IntPtr)_swapChain.Handle, out var surfaceObject),
                "ICompositorInterop.CreateCompositionSurfaceForSwapChain");

            _compositionSurface = (ICompositionSurface)surfaceObject;
            _surfaceBrush = _compositor.CreateSurfaceBrush(_compositionSurface);
            _spriteVisual.Brush = _surfaceBrush;
        }

        private void OnCompositionTargetRendering(object? sender, EventArgs e)
        {
            if (_isDisposed || !_isInitialized || _swapChain.Handle == null || _d3d11Context.Handle == null)
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

            UpdateSpritePlacement();
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

                Debug.WriteLine($"[DCompD3D11] CreateRTV bufferIndex=0, resource=0x{((nint)backBuffer.Handle):X}");
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

        private void OnHostLoaded(object sender, RoutedEventArgs e)
        {
            if (_isInitialized)
                TryFinalizePresentationBinding();
        }

        private void OnHostUnloaded(object sender, RoutedEventArgs e)
        {
            // Keep native resources alive; WPF can transiently unload/reload controls.
        }

        private void OnHostLayoutUpdated(object? sender, EventArgs e)
        {
            if (_isInitialized && _desktopTarget == null)
                TryFinalizePresentationBinding();
            else if (_isInitialized)
                UpdateSpritePlacement();
        }

        private bool TryFinalizePresentationBinding()
        {
            if (!_isInitialized)
                return false;

            if (!TryBindHostWindow())
                return false;

            EnsureCompositionTree();
            EnsureSurfaceBinding();

            if (!_isRenderingHooked)
            {
                System.Windows.Media.CompositionTarget.Rendering += OnCompositionTargetRendering;
                _isRenderingHooked = true;
            }

            UpdateSpritePlacement();
            return true;
        }

        private bool TryBindHostWindow()
        {
            if (_hostElement == null)
                return false;

            var window = Window.GetWindow(_hostElement);
            if (window == null)
                return false;

            if (!ReferenceEquals(_hostWindow, window))
            {
                UnhookHostWindow();
                _hostWindow = window;
                _hostWindow.LocationChanged += OnHostWindowLayoutChanged;
                _hostWindow.SizeChanged += OnHostWindowLayoutChanged;
                _hostWindow.StateChanged += OnHostWindowLayoutChanged;
            }

            var source = PresentationSource.FromVisual(window) as HwndSource;
            if (source == null || source.Handle == IntPtr.Zero)
                return false;

            _windowHwnd = source.Handle;
            return true;
        }

        private void OnHostWindowLayoutChanged(object? sender, EventArgs e)
        {
            if (_isInitialized && _desktopTarget == null)
                TryFinalizePresentationBinding();
            else if (_isInitialized)
                UpdateSpritePlacement();
        }

        private void UnhookHostElement()
        {
            if (_hostElement == null)
                return;

            _hostElement.Loaded -= OnHostLoaded;
            _hostElement.Unloaded -= OnHostUnloaded;
            _hostElement.LayoutUpdated -= OnHostLayoutUpdated;
            _hostElement = null;
        }

        private void UnhookHostWindow()
        {
            if (_hostWindow == null)
                return;

            _hostWindow.LocationChanged -= OnHostWindowLayoutChanged;
            _hostWindow.SizeChanged -= OnHostWindowLayoutChanged;
            _hostWindow.StateChanged -= OnHostWindowLayoutChanged;
            _hostWindow = null;
        }

        private void UpdateSpritePlacement()
        {
            if (_spriteVisual == null || _hostElement == null || _hostWindow == null)
                return;

            if (_hostElement.ActualWidth <= 0 || _hostElement.ActualHeight <= 0)
                return;

            var topLeftInWindow = _hostElement.TranslatePoint(new Point(0, 0), _hostWindow);
            var dpi = VisualTreeHelper.GetDpi(_hostElement);

            var x = (float)(topLeftInWindow.X * dpi.DpiScaleX);
            var y = (float)(topLeftInWindow.Y * dpi.DpiScaleY);
            var w = (float)Math.Max(1.0, _hostElement.ActualWidth * dpi.DpiScaleX);
            var h = (float)Math.Max(1.0, _hostElement.ActualHeight * dpi.DpiScaleY);

            _spriteVisual.Offset = new Vector3(x, y, 0f);
            _spriteVisual.Size = new Vector2(w, h);
        }

        private static void ThrowIfFailed(int hr, string operation)
        {
            if (hr >= 0)
                return;

            throw new COMException($"{operation} failed with HRESULT=0x{hr:X8}", hr);
        }

        private static TInterface GetComInterface<TInterface>(object instance) where TInterface : class
        {
            if (instance == null) throw new ArgumentNullException(nameof(instance));

            var unknownPtr = IntPtr.Zero;
            var interfacePtr = IntPtr.Zero;
            try
            {
                unknownPtr = Marshal.GetIUnknownForObject(instance);
                var iid = typeof(TInterface).GUID;
                var hr = Marshal.QueryInterface(unknownPtr, ref iid, out interfacePtr);
                if (hr < 0 || interfacePtr == IntPtr.Zero)
                    throw new COMException($"QueryInterface for {typeof(TInterface).Name} failed with HRESULT=0x{hr:X8}", hr);

                return (TInterface)Marshal.GetObjectForIUnknown(interfacePtr);
            }
            finally
            {
                if (interfacePtr != IntPtr.Zero)
                    Marshal.Release(interfacePtr);
                if (unknownPtr != IntPtr.Zero)
                    Marshal.Release(unknownPtr);
            }
        }
    }

    [ComImport]
    [Guid("29E691FA-4567-4DCA-B319-D0F207EB6807")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface ICompositorDesktopInterop
    {
        [PreserveSig]
        int CreateDesktopWindowTarget(
            IntPtr hwndTarget,
            [MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)] bool isTopmost,
            [MarshalAs(System.Runtime.InteropServices.UnmanagedType.IUnknown)] out object target);

        [PreserveSig]
        int EnsureOnThread(uint threadId);
    }

    [ComImport]
    [Guid("25297D5C-3AD4-4C9C-B5CF-E36A38512330")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface ICompositorInterop
    {
        [PreserveSig]
        int CreateCompositionSurfaceForHandle(
            IntPtr swapChainHandle,
            [MarshalAs(System.Runtime.InteropServices.UnmanagedType.IUnknown)] out object surface);

        [PreserveSig]
        int CreateCompositionSurfaceForSwapChain(
            IntPtr swapChain,
            [MarshalAs(System.Runtime.InteropServices.UnmanagedType.IUnknown)] out object surface);

        [PreserveSig]
        int CreateGraphicsDevice(
            IntPtr renderingDevice,
            [MarshalAs(System.Runtime.InteropServices.UnmanagedType.IUnknown)] out object graphicsDevice);
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DispatcherQueueOptions
    {
        public uint DwSize;
        public int ThreadType;
        public int ApartmentType;
    }

    internal enum DispatcherQueueThreadType
    {
        Dedicated = 1,
        Current = 2
    }

    internal enum DispatcherQueueThreadApartmentType
    {
        None = 0,
        Asta = 1,
        Sta = 2
    }

    internal static class DispatcherQueueInterop
    {
        [DllImport("CoreMessaging.dll")]
        internal static extern int CreateDispatcherQueueController(
            DispatcherQueueOptions options,
            out IntPtr dispatcherQueueController);
    }
}
