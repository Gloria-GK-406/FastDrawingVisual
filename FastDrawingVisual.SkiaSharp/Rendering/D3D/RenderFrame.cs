using FastDrawingVisual.Rendering.Skia;
using Silk.NET.Direct3D11;
using Silk.NET.Direct3D9;
using Silk.NET.DXGI;
using SkiaSharp;
using System;
using System.Runtime.InteropServices;
using System.Threading;
using Format11 = Silk.NET.DXGI.Format;

namespace FastDrawingVisual.Rendering.D3D
{
    internal unsafe sealed class RenderFrame : IDisposable
    {
        private const uint D3D11DoNotFlush = 0x0000_0001;

        private readonly D3DDeviceManager _deviceManager;
        private readonly Action<RenderFrame> _onDrawingComplete;

        private ID3D11Texture2D* _d3d11Texture;
        private ID3D11Texture2D* _stagingTexture;
        private IDirect3DTexture9* _d3d9Texture;
        private IDirect3DSurface9* _d3d9Surface;
        private ID3D11Query* _syncQuery;
        private SKSurface? _skiaSurface;

        private int _state = (int)FrameState.Ready;
        private int _width;
        private int _height;
        private bool _isDisposed;

        public FrameState State => (FrameState)Volatile.Read(ref _state);

        public IntPtr D3D9SurfacePointer => (IntPtr)_d3d9Surface;
        internal IDirect3DSurface9* D3D9Surface => _d3d9Surface;

        public int Width => _width;
        public int Height => _height;

        public RenderFrame(D3DDeviceManager deviceManager, Action<RenderFrame> onDrawingComplete)
        {
            _deviceManager = deviceManager ?? throw new ArgumentNullException(nameof(deviceManager));
            _onDrawingComplete = onDrawingComplete ?? throw new ArgumentNullException(nameof(onDrawingComplete));
        }

        public void CreateResources(int width, int height)
        {
            ReleaseResources();

            _width = width;
            _height = height;

            _d3d11Texture = _deviceManager.CreateSharedTexture(width, height);

            var stagingDesc = new Texture2DDesc
            {
                Width = (uint)width,
                Height = (uint)height,
                MipLevels = 1,
                ArraySize = 1,
                Format = Format11.FormatB8G8R8A8Unorm,
                SampleDesc = new SampleDesc { Count = 1, Quality = 0 },
                Usage = Usage.Staging,
                BindFlags = 0,
                CPUAccessFlags = (uint)CpuAccessFlag.Write,
                MiscFlags = 0
            };

            var device = _deviceManager.D3D11Device;
            ID3D11Texture2D* stagingTexture = null;
            ThrowIfFailed(device->CreateTexture2D(&stagingDesc, (SubresourceData*)null, &stagingTexture));
            _stagingTexture = stagingTexture;

            var queryDesc = new QueryDesc
            {
                Query = Query.Event,
                MiscFlags = 0
            };
            ID3D11Query* syncQuery = null;
            ThrowIfFailed(device->CreateQuery(&queryDesc, &syncQuery));
            _syncQuery = syncQuery;

            var sharedHandle = _deviceManager.GetSharedHandle(_d3d11Texture);
            _d3d9Texture = _deviceManager.FromSharedHandle(sharedHandle, width, height, out _d3d9Surface);

            var info = new SKImageInfo(
                width,
                height,
                SKColorType.Bgra8888,
                SKAlphaType.Premul,
                SKColorSpace.CreateSrgb());

            _skiaSurface = SKSurface.Create(info)
                ?? throw new InvalidOperationException("Could not create SKSurface.");

            Interlocked.Exchange(ref _state, (int)FrameState.Ready);
        }

        public void ReleaseResources()
        {
            _skiaSurface?.Dispose();
            _skiaSurface = null;

            ComPtrExtensions.Release(ref _syncQuery);
            ComPtrExtensions.Release(ref _d3d9Surface);
            ComPtrExtensions.Release(ref _d3d9Texture);
            ComPtrExtensions.Release(ref _stagingTexture);
            ComPtrExtensions.Release(ref _d3d11Texture);
        }

        public IDrawingContext OpenCanvas(Action? onClosed = null)
        {
            if (_skiaSurface == null)
                throw new InvalidOperationException("Resources are not created. Call CreateResources first.");

            _skiaSurface.Canvas.Clear(SKColors.Transparent);
            return new SkiaDrawingContext(_skiaSurface.Canvas, _width, _height, () =>
            {
                try
                {
                    OnCanvasClosed();
                }
                finally
                {
                    onClosed?.Invoke();
                }
            });
        }

        private void OnCanvasClosed()
        {
            if (_isDisposed || _skiaSurface == null)
                return;

            _skiaSurface.Canvas.Flush();
            UploadToD3D11();
            _onDrawingComplete(this);
        }

        private void UploadToD3D11()
        {
            if (_skiaSurface == null || _d3d11Texture == null || _stagingTexture == null || _syncQuery == null)
                return;

            var pixmap = _skiaSurface.PeekPixels();
            if (pixmap == null)
                return;

            var context = _deviceManager.D3D11ImmediateContext;
            var mapped = default(MappedSubresource);

            ThrowIfFailed(context->Map((ID3D11Resource*)_stagingTexture, 0, Map.Write, 0, &mapped));
            try
            {
                var src = (byte*)pixmap.GetPixels().ToPointer();
                var dst = (byte*)mapped.PData;
                var srcRowBytes = _width * 4;

                if (mapped.RowPitch == srcRowBytes)
                {
                    Buffer.MemoryCopy(src, dst, (long)srcRowBytes * _height, (long)srcRowBytes * _height);
                }
                else
                {
                    for (int y = 0; y < _height; y++)
                    {
                        Buffer.MemoryCopy(
                            src + ((long)y * srcRowBytes),
                            dst + ((long)y * mapped.RowPitch),
                            srcRowBytes,
                            srcRowBytes);
                    }
                }
            }
            finally
            {
                context->Unmap((ID3D11Resource*)_stagingTexture, 0);
            }

            context->CopyResource((ID3D11Resource*)_d3d11Texture, (ID3D11Resource*)_stagingTexture);
            context->End((ID3D11Asynchronous*)_syncQuery);
            context->Flush();

            var completed = 0;
            int hr;
            do
            {
                hr = context->GetData((ID3D11Asynchronous*)_syncQuery, &completed, (uint)sizeof(int), D3D11DoNotFlush);
                if (hr == 1)
                    Thread.Sleep(0);
            }
            while (hr == 1);

            ThrowIfFailed(hr);
        }

        internal bool TryTransitionTo(FrameState from, FrameState to)
            => Interlocked.CompareExchange(ref _state, (int)to, (int)from) == (int)from;

        internal void ForceSetState(FrameState state)
            => Interlocked.Exchange(ref _state, (int)state);

        public void Dispose()
        {
            if (_isDisposed)
                return;

            _isDisposed = true;
            ReleaseResources();
        }

        private static void ThrowIfFailed(int hr)
        {
            if (hr < 0)
                Marshal.ThrowExceptionForHR(hr);
        }
    }
}
