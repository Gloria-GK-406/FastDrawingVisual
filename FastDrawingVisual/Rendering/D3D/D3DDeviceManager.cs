using SharpDX;
using SharpDX.Direct3D9;
using SharpDX.Direct3D11;
using System;
using System.Windows;
using System.Windows.Interop;
using Device11 = SharpDX.Direct3D11.Device;
using Device9 = SharpDX.Direct3D9.Device;
using Factory1 = SharpDX.DXGI.Factory1;
using DriverType11 = SharpDX.Direct3D.DriverType;
using Format11 = SharpDX.DXGI.Format;
using ResultCode9 = SharpDX.Direct3D9.ResultCode;
using Surface9 = SharpDX.Direct3D9.Surface;
using Texture2D11 = SharpDX.Direct3D11.Texture2D;

namespace FastDrawingVisual.Rendering.D3D
{
    internal class D3DDeviceManager : IDisposable
    {
        private Device11? _d3d11Device;
        private Device9? _d3d9Device;
        private Direct3DEx? _d3d9Ex;
        private HwndSource? _fallbackHwndSource;
        private bool _isDisposed;

        public Device11 D3D11Device => _d3d11Device ?? throw new ObjectDisposedException(nameof(D3DDeviceManager));
        public Device9 D3D9Device => _d3d9Device ?? throw new ObjectDisposedException(nameof(D3DDeviceManager));
        public bool IsInitialized => _d3d11Device != null && _d3d9Device != null;

        public bool Initialize()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(D3DDeviceManager));

            ReleaseResources();

            try
            {
                var createFlags11 = DeviceCreationFlags.BgraSupport;
#if DEBUG
                createFlags11 |= DeviceCreationFlags.Debug;
#endif
                // Keep D3D11 on adapter 0 to match D3D9Ex adapter 0 for shared-handle interop.
                _d3d11Device = CreateD3D11Device(createFlags11);

                var hwnd = GetOrCreateDeviceHwnd();
                if (hwnd == IntPtr.Zero)
                    throw new InvalidOperationException("Could not create a valid HWND for Direct3D9Ex.");

                var pp = new PresentParameters
                {
                    Windowed = true,
                    SwapEffect = SwapEffect.Discard,
                    DeviceWindowHandle = hwnd,
                    PresentationInterval = PresentInterval.Immediate,
                    BackBufferWidth = 1,
                    BackBufferHeight = 1,
                    BackBufferFormat = Format.A8R8G8B8
                };

                _d3d9Ex = new Direct3DEx();
                var commonFlags = CreateFlags.Multithreaded | CreateFlags.FpuPreserve;

                var caps = _d3d9Ex.GetDeviceCaps(0, DeviceType.Hardware);
                var vpFlag = (caps.DeviceCaps & DeviceCaps.HWTransformAndLight) != 0
                    ? CreateFlags.HardwareVertexProcessing
                    : CreateFlags.SoftwareVertexProcessing;

                try
                {
                    _d3d9Device = new DeviceEx(
                        _d3d9Ex,
                        0,
                        DeviceType.Hardware,
                        hwnd,
                        commonFlags | vpFlag,
                        pp);
                }
                catch (SharpDXException ex) when (
                    ex.HResult == unchecked((int)0x8876086C) || // D3DERR_INVALIDCALL
                    ex.HResult == unchecked((int)0x80004001))   // E_NOTIMPL
                {
                    _d3d9Device = new DeviceEx(
                        _d3d9Ex,
                        0,
                        DeviceType.Hardware,
                        hwnd,
                        commonFlags | CreateFlags.SoftwareVertexProcessing,
                        pp);
                }

                return true;
            }
            catch
            {
                ReleaseResources();
                return false;
            }
        }

        public Texture2D CreateSharedTexture(int width, int height)
        {
            if (_d3d11Device == null)
                throw new InvalidOperationException("D3D11 device is not initialized.");

            var description = new Texture2DDescription
            {
                Width = width,
                Height = height,
                MipLevels = 1,
                ArraySize = 1,
                Format = Format11.B8G8R8A8_UNorm,
                SampleDescription = new SharpDX.DXGI.SampleDescription(1, 0),
                Usage = ResourceUsage.Default,
                // D3D11<->D3D9 shared texture interop needs RT + SRV bind flags.
                BindFlags = BindFlags.RenderTarget | BindFlags.ShaderResource,
                CpuAccessFlags = CpuAccessFlags.None,
                OptionFlags = ResourceOptionFlags.Shared
            };

            return new Texture2D(_d3d11Device, description);
        }

        public IntPtr GetSharedHandle(Texture2D texture)
        {
            using var resource = texture.QueryInterface<SharpDX.DXGI.Resource>();
            return resource.SharedHandle;
        }

        public Texture FromSharedHandle(IntPtr sharedHandle, int width, int height, out Surface9 surface)
        {
            if (_d3d9Device == null)
                throw new InvalidOperationException("D3D9 device is not initialized.");
            if (sharedHandle == IntPtr.Zero)
                throw new InvalidOperationException("Shared handle is null. D3D11 texture was not created as a sharable resource.");

            Texture texture;
            try
            {
                texture = new Texture(
                    _d3d9Device,
                    width,
                    height,
                    1,
                    Usage.RenderTarget,
                    Format.A8R8G8B8,
                    Pool.Default,
                    ref sharedHandle);
            }
            catch (SharpDXException ex) when (ex.HResult == unchecked((int)0x80070057)) // E_INVALIDARG
            {
                // Some drivers only accept X8R8G8B8 on the D3D9 side for shared textures.
                texture = new Texture(
                    _d3d9Device,
                    width,
                    height,
                    1,
                    Usage.RenderTarget,
                    Format.X8R8G8B8,
                    Pool.Default,
                    ref sharedHandle);
            }

            surface = texture.GetSurfaceLevel(0);
            return texture;
        }

        public bool CheckDeviceState()
        {
            if (_d3d9Device == null)
                return false;

            try
            {
                return _d3d9Device.TestCooperativeLevel() == ResultCode9.Success;
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
                    WindowStyle = unchecked((int)0x80000000) // WS_POPUP
                };
                _fallbackHwndSource = new HwndSource(p);
            }

            return _fallbackHwndSource.Handle;
        }

        private static Device11 CreateD3D11Device(DeviceCreationFlags createFlags)
        {
            try
            {
                using var factory = new Factory1();
                using var adapter = factory.GetAdapter1(0);
                return new Device11(adapter, createFlags);
            }
            catch
            {
                return new Device11(DriverType11.Hardware, createFlags);
            }
        }

        private void ReleaseResources()
        {
            _d3d9Device?.Dispose();
            _d3d9Device = null;

            _d3d9Ex?.Dispose();
            _d3d9Ex = null;

            _d3d11Device?.Dispose();
            _d3d11Device = null;
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
    }
}
