using System;
using Texture2D11 = SharpDX.Direct3D11.Texture2D;
using Texture9 = SharpDX.Direct3D9.Texture;
using Surface9 = SharpDX.Direct3D9.Surface;

namespace FastDrawingVisual.Rendering.D3D
{
    /// <summary>
    /// 渲染帧池。管理两个绘制帧（Drawing / ReadyForPresent）和一个独立的 Presenting 表面。
    /// </summary>
    internal sealed class RenderFramePool : IDisposable
    {
        private const int FrameCount = 2;

        private readonly D3DDeviceManager _deviceManager;
        private readonly RenderFrame[] _frames;
        private Texture2D11? _presentingD3D11Texture;
        private Texture9? _presentingD3D9Texture;
        private Surface9? _presentingSurface;

        private readonly object _lock = new object();
        private volatile bool _hasReadyFrame;

        private bool _isDisposed;

        public RenderFramePool(D3DDeviceManager deviceManager)
        {
            if (deviceManager == null) throw new ArgumentNullException(nameof(deviceManager));

            _deviceManager = deviceManager;

            _frames = new RenderFrame[FrameCount];
            for (int i = 0; i < FrameCount; i++)
                _frames[i] = new RenderFrame(deviceManager, OnFrameDrawingComplete);
        }

        /// <summary>为所有帧创建（或重建）GPU 资源。</summary>
        public void CreateResources(int width, int height)
        {
            if (width <= 0 || height <= 0)
                throw new ArgumentException("宽高必须大于 0。");

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
                _presentingD3D9Texture = _deviceManager.FromSharedHandle(sharedHandle, width, height, out var surface);
                _presentingSurface = surface;
            }
        }

        /// <summary>释放所有帧的 GPU 资源（设备丢失时调用）。</summary>
        public void ReleaseResources()
        {
            lock (_lock)
            {
                _hasReadyFrame = false;
                foreach (var frame in _frames)
                    frame.ReleaseResources();

                _presentingSurface?.Dispose();
                _presentingSurface = null;

                _presentingD3D9Texture?.Dispose();
                _presentingD3D9Texture = null;

                _presentingD3D11Texture?.Dispose();
                _presentingD3D11Texture = null;
            }
        }

        /// <summary>
        /// 获取一个可用于绘制的帧，并原子性地将其从 Ready → Drawing。
        /// 若所有帧都忙则返回 null（调用方可稍后重试或跳过本帧）。
        /// </summary>
        public RenderFrame? AcquireForDrawing()
        {
            foreach (var frame in _frames)
            {
                if (frame.TryTransitionTo(FrameState.Ready, FrameState.Drawing))
                    return frame;
            }
            return null;
        }

        /// <summary>
        /// 由 <see cref="RenderFrame"/> 在画布关闭后回调。
        /// 保证同一时刻最多只有一帧处于 ReadyForPresent，
        /// 新帧到来时旧的 ReadyForPresent 帧被抢占回 Ready（帧丢弃策略，始终呈现最新帧）。
        /// </summary>
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

                // 将完成的帧推进到 ReadyForPresent
                completedFrame.ForceSetState(FrameState.ReadyForPresent);
                _hasReadyFrame = true;
            }
        }

        public IntPtr GetPresentingSurfacePointer()
        {
            lock (_lock)
            {
                return _presentingSurface?.NativePointer ?? IntPtr.Zero;
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
            if (_isDisposed) return;
            _isDisposed = true;

            foreach (var frame in _frames)
                frame.Dispose();
        }
    }
}
