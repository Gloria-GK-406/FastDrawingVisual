using Silk.NET.Core;
using Silk.NET.Core.Native;
using Silk.NET.Direct3D11;
using Silk.NET.Direct3D9;
using Silk.NET.DXGI;
using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using D3DDriverType = Silk.NET.Core.Native.D3DDriverType;
using D3DFeatureLevel = Silk.NET.Core.Native.D3DFeatureLevel;
using DeviceCaps9 = Silk.NET.Direct3D9.Caps9;
using Format11 = Silk.NET.DXGI.Format;
using Format9 = Silk.NET.Direct3D9.Format;
using PresentParameters9 = Silk.NET.Direct3D9.PresentParameters;

namespace FastDrawingVisual.Rendering.D3D
{
    internal unsafe sealed class D3DDeviceManager : IDisposable
    {
        private const uint D3D9SdkVersion = 32;
        private const uint D3D11SdkVersion = 7;
        private const uint D3DCreateFpuPreserve = 0x0000_0002;
        private const uint D3DCreateMultithreaded = 0x0000_0004;
        private const uint D3DCreateSoftwareVertexProcessing = 0x0000_0020;
        private const uint D3DCreateHardwareVertexProcessing = 0x0000_0040;
        private const uint D3DPresentIntervalImmediate = 0x8000_0000;
        private const uint D3DUsageRenderTarget = 0x0000_0001;
        private const uint D3DDeviceCapsHwTransformAndLight = 0x0001_0000;
        private const int D3DErrInvalidCall = unchecked((int)0x8876086C);
        private const int EInvalidArg = unchecked((int)0x80070057);
        private const int ENotImpl = unchecked((int)0x80004001);

        private ID3D11Device* _d3d11Device;
        private ID3D11DeviceContext* _d3d11ImmediateContext;
        private IDirect3DDevice9Ex* _d3d9Device;
        private IDirect3D9Ex* _d3d9Ex;
        private HwndSource? _fallbackHwndSource;
        private bool _isDisposed;

        public ID3D11Device* D3D11Device
        {
            get
            {
                if (_d3d11Device == null)
                    throw new ObjectDisposedException(nameof(D3DDeviceManager));
                return _d3d11Device;
            }
        }

        public ID3D11DeviceContext* D3D11ImmediateContext
        {
            get
            {
                if (_d3d11ImmediateContext == null)
                    throw new ObjectDisposedException(nameof(D3DDeviceManager));
                return _d3d11ImmediateContext;
            }
        }

        public IDirect3DDevice9Ex* D3D9Device
        {
            get
            {
                if (_d3d9Device == null)
                    throw new ObjectDisposedException(nameof(D3DDeviceManager));
                return _d3d9Device;
            }
        }

        public bool IsInitialized => _d3d11Device != null && _d3d9Device != null;

        public bool Initialize()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(D3DDeviceManager));

            ReleaseResources();

            try
            {
                _d3d11Device = CreateD3D11Device(out _d3d11ImmediateContext);

                var hwnd = GetOrCreateDeviceHwnd();
                if (hwnd == IntPtr.Zero)
                    throw new InvalidOperationException("Could not create a valid HWND for Direct3D9Ex.");

                _d3d9Ex = CreateD3D9Ex();

                var pp = new PresentParameters9
                {
                    Windowed = true,
                    SwapEffect = Swapeffect.Discard,
                    HDeviceWindow = hwnd,
                    PresentationInterval = D3DPresentIntervalImmediate,
                    BackBufferWidth = 1,
                    BackBufferHeight = 1,
                    BackBufferFormat = Format9.A8R8G8B8
                };

                var caps = default(DeviceCaps9);
                ThrowIfFailed(_d3d9Ex->GetDeviceCaps(0, Devtype.Hal, &caps));

                var createFlags = D3DCreateMultithreaded | D3DCreateFpuPreserve;
                createFlags |= (caps.DevCaps & D3DDeviceCapsHwTransformAndLight) != 0
                    ? D3DCreateHardwareVertexProcessing
                    : D3DCreateSoftwareVertexProcessing;

                IDirect3DDevice9Ex* d3d9Device = null;
                var hr = _d3d9Ex->CreateDeviceEx(
                    0,
                    Devtype.Hal,
                    hwnd,
                    createFlags,
                    &pp,
                    (Displaymodeex*)null,
                    &d3d9Device);

                if (hr == D3DErrInvalidCall || hr == ENotImpl)
                {
                    createFlags = D3DCreateMultithreaded | D3DCreateFpuPreserve | D3DCreateSoftwareVertexProcessing;
                    ThrowIfFailed(_d3d9Ex->CreateDeviceEx(
                        0,
                        Devtype.Hal,
                        hwnd,
                        createFlags,
                        &pp,
                        (Displaymodeex*)null,
                        &d3d9Device));
                }
                else
                {
                    ThrowIfFailed(hr);
                }

                _d3d9Device = d3d9Device;
                return true;
            }
            catch
            {
                ReleaseResources();
                return false;
            }
        }

        public ID3D11Texture2D* CreateSharedTexture(int width, int height)
        {
            if (_d3d11Device == null)
                throw new InvalidOperationException("D3D11 device is not initialized.");

            var description = new Texture2DDesc
            {
                Width = (uint)width,
                Height = (uint)height,
                MipLevels = 1,
                ArraySize = 1,
                Format = Format11.FormatB8G8R8A8Unorm,
                SampleDesc = new SampleDesc { Count = 1, Quality = 0 },
                Usage = Usage.Default,
                BindFlags = (uint)(BindFlag.RenderTarget | BindFlag.ShaderResource),
                CPUAccessFlags = 0,
                MiscFlags = (uint)ResourceMiscFlag.Shared
            };

            ID3D11Texture2D* texture = null;
            ThrowIfFailed(_d3d11Device->CreateTexture2D(&description, (SubresourceData*)null, &texture));
            return texture;
        }

        public IntPtr GetSharedHandle(ID3D11Texture2D* texture)
        {
            if (texture == null)
                throw new ArgumentNullException(nameof(texture));

            IDXGIResource* resource = null;
            try
            {
                var iid = typeof(IDXGIResource).GUID;
                ThrowIfFailed(texture->QueryInterface(ref iid, (void**)&resource));

                void* sharedHandle = null;
                ThrowIfFailed(resource->GetSharedHandle(&sharedHandle));
                return (IntPtr)sharedHandle;
            }
            finally
            {
                ComPtrExtensions.Release(ref resource);
            }
        }

        public IDirect3DTexture9* FromSharedHandle(IntPtr sharedHandle, int width, int height, out IDirect3DSurface9* surface)
        {
            if (_d3d9Device == null)
                throw new InvalidOperationException("D3D9 device is not initialized.");
            if (sharedHandle == IntPtr.Zero)
                throw new InvalidOperationException("Shared handle is null. D3D11 texture was not created as a sharable resource.");

            IDirect3DTexture9* texture = null;
            void* rawHandle = (void*)sharedHandle;

            var hr = _d3d9Device->CreateTexture(
                (uint)width,
                (uint)height,
                1,
                D3DUsageRenderTarget,
                Format9.A8R8G8B8,
                Pool.Default,
                &texture,
                &rawHandle);

            if (hr == EInvalidArg)
            {
                rawHandle = (void*)sharedHandle;
                hr = _d3d9Device->CreateTexture(
                    (uint)width,
                    (uint)height,
                    1,
                    D3DUsageRenderTarget,
                    Format9.X8R8G8B8,
                    Pool.Default,
                    &texture,
                    &rawHandle);
            }

            ThrowIfFailed(hr);

            surface = null;
            IDirect3DSurface9* level0Surface = null;
            ThrowIfFailed(texture->GetSurfaceLevel(0, &level0Surface));
            surface = level0Surface;
            return texture;
        }

        public bool CheckDeviceState()
        {
            if (_d3d9Device == null)
                return false;

            try
            {
                return _d3d9Device->TestCooperativeLevel() >= 0;
            }
            catch
            {
                return false;
            }
        }

        public bool CopySurface(IDirect3DSurface9* source, IDirect3DSurface9* destination)
        {
            if (_d3d9Device == null || source == null || destination == null)
                return false;

            try
            {
                return _d3d9Device->StretchRect(source, null, destination, null, Texturefiltertype.None) >= 0;
            }
            catch
            {
                return false;
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
                var p = new HwndSourceParameters("FastDrawingVisual.D3DHost")
                {
                    Width = 1,
                    Height = 1,
                    WindowStyle = unchecked((int)0x80000000)
                };
                _fallbackHwndSource = new HwndSource(p);
            }

            return _fallbackHwndSource.Handle;
        }

        private static ID3D11Device* CreateD3D11Device(out ID3D11DeviceContext* immediateContext)
        {
            immediateContext = null;
            var createFlags = (uint)CreateDeviceFlag.BgraSupport;
#if DEBUG
            createFlags |= (uint)CreateDeviceFlag.Debug;
#endif

            IDXGIFactory1* factory = null;
            IDXGIAdapter1* adapter = null;
            ID3D11Device* device = null;
            ID3D11DeviceContext* context = null;

            try
            {
                factory = CreateDxgiFactory1();
                if (factory != null)
                {
                    factory->EnumAdapters1(0, &adapter);
                }

                var hr = D3D11CreateDevice(
                    (IDXGIAdapter*)adapter,
                    adapter != null ? D3DDriverType.Unknown : D3DDriverType.Hardware,
                    0,
                    createFlags,
                    (D3DFeatureLevel*)null,
                    0,
                    D3D11SdkVersion,
                    &device,
                    (D3DFeatureLevel*)null,
                    &context);

                if (hr < 0 && adapter != null)
                {
                    ComPtrExtensions.Release(ref device);
                    ComPtrExtensions.Release(ref context);

                    hr = D3D11CreateDevice(
                        null,
                        D3DDriverType.Hardware,
                        0,
                        createFlags,
                        (D3DFeatureLevel*)null,
                        0,
                        D3D11SdkVersion,
                        &device,
                        (D3DFeatureLevel*)null,
                        &context);
                }

                ThrowIfFailed(hr);
                immediateContext = context;
                return device;
            }
            finally
            {
                ComPtrExtensions.Release(ref adapter);
                ComPtrExtensions.Release(ref factory);
            }
        }

        private static IDXGIFactory1* CreateDxgiFactory1()
        {
            IDXGIFactory1* factory = null;
            var iid = typeof(IDXGIFactory1).GUID;
            ThrowIfFailed(CreateDXGIFactory1(ref iid, (void**)&factory));
            return factory;
        }

        private static IDirect3D9Ex* CreateD3D9Ex()
        {
            IDirect3D9Ex* d3d9Ex = null;
            ThrowIfFailed(Direct3DCreate9Ex(D3D9SdkVersion, &d3d9Ex));
            return d3d9Ex;
        }

        private void ReleaseResources()
        {
            ComPtrExtensions.Release(ref _d3d9Device);
            ComPtrExtensions.Release(ref _d3d9Ex);
            ComPtrExtensions.Release(ref _d3d11ImmediateContext);
            ComPtrExtensions.Release(ref _d3d11Device);
        }

        public void Dispose()
        {
            if (_isDisposed)
                return;

            _isDisposed = true;

            ReleaseResources();

            _fallbackHwndSource?.Dispose();
            _fallbackHwndSource = null;
        }

        private static void ThrowIfFailed(int hr)
        {
            if (hr < 0)
                Marshal.ThrowExceptionForHR(hr);
        }

        [DllImport("d3d11.dll", ExactSpelling = true)]
        private static extern int D3D11CreateDevice(
            IDXGIAdapter* adapter,
            D3DDriverType driverType,
            nint software,
            uint flags,
            D3DFeatureLevel* featureLevels,
            uint featureLevelCount,
            uint sdkVersion,
            ID3D11Device** device,
            D3DFeatureLevel* featureLevel,
            ID3D11DeviceContext** immediateContext);

        [DllImport("dxgi.dll", ExactSpelling = true)]
        private static extern int CreateDXGIFactory1(ref Guid riid, void** factory);

        [DllImport("d3d9.dll", ExactSpelling = true)]
        private static extern int Direct3DCreate9Ex(uint sdkVersion, IDirect3D9Ex** direct3D9Ex);
    }

    internal static unsafe class ComPtrExtensions
    {
        public static void Release<T>(ref T* comObject)
            where T : unmanaged
        {
            if (comObject == null)
                return;

            ((IUnknown*)comObject)->Release();
            comObject = null;
        }
    }
}
