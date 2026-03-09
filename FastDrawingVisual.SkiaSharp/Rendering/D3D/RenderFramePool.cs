using Silk.NET.Direct3D11;
using Silk.NET.Direct3D9;
using System;

namespace FastDrawingVisual.Rendering.D3D
{
    internal unsafe sealed class RenderFramePool : IDisposable
    {
        private const int FrameCount = 2;

        private readonly D3DDeviceManager _deviceManager;
        private readonly RenderFrame[] _frames;
        private ID3D11Texture2D* _presentingD3D11Texture;
        private IDirect3DTexture9* _presentingD3D9Texture;
        private IDirect3DSurface9* _presentingSurface;

        private readonly object _lock = new object();
        private volatile bool _hasReadyFrame;

        private bool _isDisposed;

        public RenderFramePool(D3DDeviceManager deviceManager)
        {
            _deviceManager = deviceManager ?? throw new ArgumentNullException(nameof(deviceManager));

            _frames = new RenderFrame[FrameCount];
            for (int i = 0; i < FrameCount; i++)
                _frames[i] = new RenderFrame(deviceManager, OnFrameDrawingComplete);
        }

        public void CreateResources(int width, int height)
        {
            if (width <= 0 || height <= 0)
                throw new ArgumentException("Width and height must be greater than zero.");

            lock (_lock)
            {
                _hasReadyFrame = false;
                foreach (var frame in _frames)
                {
                    frame.CreateResources(width, height);
                    frame.ForceSetState(FrameState.Ready);
                }

                _presentingD3D11Texture = _deviceManager.CreateSharedTexture(width, height);
                var sharedHandle = _deviceManager.GetSharedHandle(_presentingD3D11Texture);
                _presentingD3D9Texture = _deviceManager.FromSharedHandle(sharedHandle, width, height, out _presentingSurface);
            }
        }

        public void ReleaseResources()
        {
            lock (_lock)
            {
                _hasReadyFrame = false;
                foreach (var frame in _frames)
                    frame.ReleaseResources();

                ComPtrExtensions.Release(ref _presentingSurface);
                ComPtrExtensions.Release(ref _presentingD3D9Texture);
                ComPtrExtensions.Release(ref _presentingD3D11Texture);
            }
        }

        public RenderFrame? AcquireForDrawing()
        {
            foreach (var frame in _frames)
            {
                if (frame.TryTransitionTo(FrameState.Ready, FrameState.Drawing))
                    return frame;
            }

            return null;
        }

        private void OnFrameDrawingComplete(RenderFrame completedFrame)
        {
            lock (_lock)
            {
                foreach (var frame in _frames)
                {
                    if (frame != completedFrame && frame.State == FrameState.ReadyForPresent)
                    {
                        frame.ForceSetState(FrameState.Ready);
                        break;
                    }
                }

                completedFrame.ForceSetState(FrameState.ReadyForPresent);
                _hasReadyFrame = true;
            }
        }

        public IntPtr GetPresentingSurfacePointer()
        {
            lock (_lock)
            {
                return (IntPtr)_presentingSurface;
            }
        }

        public bool CopyReadyToPresenting()
        {
            if (!_hasReadyFrame)
                return false;

            lock (_lock)
            {
                if (!_hasReadyFrame)
                    return false;

                RenderFrame? readyFrame = null;
                foreach (var frame in _frames)
                {
                    if (frame.State == FrameState.ReadyForPresent)
                    {
                        readyFrame = frame;
                        break;
                    }
                }

                if (readyFrame == null)
                {
                    _hasReadyFrame = false;
                    return false;
                }

                if (readyFrame.D3D9Surface == null || _presentingSurface == null)
                    return false;

                if (!_deviceManager.CopySurface(readyFrame.D3D9Surface, _presentingSurface))
                    return false;

                readyFrame.ForceSetState(FrameState.Ready);
                _hasReadyFrame = false;
                return true;
            }
        }

        public void Dispose()
        {
            if (_isDisposed)
                return;

            _isDisposed = true;

            ReleaseResources();

            foreach (var frame in _frames)
                frame.Dispose();
        }
    }
}
