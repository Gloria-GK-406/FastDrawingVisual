using FastDrawingVisual.Rendering;
using FastDrawingVisual.Rendering.D3D;
using System;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;

namespace FastDrawingVisual.Controls
{
    /// <summary>
    /// 基于 D3DImage 的高性能渲染图像控件。
    /// 职责仅限于：
    ///   1. 通过 <see cref="RenderFramePool"/> 对外提供 <see cref="IDrawingContext"/>；
    ///   2. 在 WPF 的 CompositionTarget.Rendering 回调中，检查是否有新帧就绪，
    ///      若有则加锁交换 D3DImage 的后缓冲。
    /// 渲染逻辑、像素上传、缓冲区轮转均由底层封装，此类不感知。
    /// </summary>
    internal sealed class D3DFastImage : IFastImage, IDisposable
    {
        private readonly D3DImage _d3dImage;
        private readonly D3DDeviceManager _deviceManager;
        private readonly RenderFramePool _pool;
        private readonly Dispatcher _uiDispatcher;

        private int _width;
        private int _height;
        private bool _isInitialized;
        private bool _isDeviceLost;
        private bool _isDisposed;

        public ImageSource Source => _d3dImage;
        public double Width => _width;
        public double Height => _height;
        public bool IsInitialized => _isInitialized;

        public D3DFastImage()
        {
            _d3dImage = new D3DImage();
            _deviceManager = new D3DDeviceManager();
            _pool = new RenderFramePool(_deviceManager);
            _uiDispatcher = Dispatcher.CurrentDispatcher;

            _d3dImage.IsFrontBufferAvailableChanged += OnFrontBufferAvailableChanged;
        }

        #region 初始化 / 调整大小

        /// <summary>初始化设备和所有渲染帧资源。</summary>
        public bool Initialize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(D3DFastImage));

            if (width <= 0 || height <= 0)
                throw new ArgumentException("宽高必须大于 0。");

            if (!_deviceManager.IsInitialized && !_deviceManager.Initialize())
                return false;

            _pool.CreateResources(width, height);

            _width = width;
            _height = height;
            _isInitialized = true;
            _isDeviceLost = false;

            // 订阅 WPF 渲染回调，开始检测就绪帧
            CompositionTarget.Rendering += OnCompositionTargetRendering;

            return true;
        }

        /// <summary>调整渲染尺寸，重建所有帧资源。</summary>
        public void Resize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(D3DFastImage));

            if (width == _width && height == _height) return;

            // 先解绑旧的后缓冲，避免 D3DImage 持有无效资源
            UnbindBackBuffer();

            _pool.CreateResources(width, height);
            _width = width;
            _height = height;
        }

        #endregion

        /// <summary>
        /// 尝试获取一个可绘制上下文。可在任意线程调用。
        /// 上下文 Close/Dispose 时自动完成像素上传并通知帧池。
        /// </summary>
        /// <returns>
        /// 成功时返回 <see cref="IDrawingContext"/>；
        /// 当前无可用渲染帧（所有帧均忙于 Drawing/Presenting）或设备丢失时返回 <c>null</c>，
        /// 调用方可直接跳过本帧。
        /// </returns>
        public IDrawingContext? TryOpenRender()
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(D3DFastImage));

            if (!_isInitialized)
                throw new InvalidOperationException("请先调用 Initialize。");

            if (_isDeviceLost)
                return null;

            var frame = _pool.AcquireForDrawing();

            return frame?.OpenCanvas();
        }

        /// <summary>
        /// CompositionTarget.Rendering 回调，在 WPF 每帧渲染前触发（UI 线程）。
        /// 尝试从帧池获取就绪帧，有则加锁替换 D3DImage 后缓冲。
        /// </summary>
        private void OnCompositionTargetRendering(object? sender, EventArgs e)
        {
            if (_isDisposed || _isDeviceLost || !_isInitialized) return;

            var frame = _pool.TryAcquireForPresent();
            if (frame == null) return;

            // TryLock 获取 D3DImage 写权限
            if (!_d3dImage.TryLock(new Duration(TimeSpan.FromMilliseconds(2))))
                return;

            try
            {
                // 替换后缓冲（WPF 会释放对旧 D3D9 表面的引用，Pool 会在下次调用时回收旧帧）
                _d3dImage.SetBackBuffer(D3DResourceType.IDirect3DSurface9,
                                        frame.D3D9SurfacePointer,
                                        enableSoftwareFallback: true);
                _d3dImage.AddDirtyRect(new Int32Rect(0, 0, _width, _height));
            }
            finally
            {
                _d3dImage.Unlock();
            }
        }

        private void OnFrontBufferAvailableChanged(object sender, DependencyPropertyChangedEventArgs e)
        {
            bool isAvailable = (bool)e.NewValue;

            if (!isAvailable)
            {
                // 设备丢失：解绑后缓冲，释放 GPU 资源
                _isDeviceLost = true;
                _uiDispatcher.BeginInvoke(() =>
                {
                    UnbindBackBuffer();
                    _pool.ReleaseResources();
                });
            }
            else
            {
                // 设备恢复：重建设备和所有帧资源
                _uiDispatcher.BeginInvoke(() =>
                {
                    try
                    {
                        _deviceManager.Dispose();
                        if (_deviceManager.Initialize() && _width > 0 && _height > 0)
                        {
                            _pool.CreateResources(_width, _height);
                            _isDeviceLost = false;
                        }
                    }
                    catch
                    {
                        // 恢复失败，保持 DeviceLost 状态，等待下次机会
                    }
                });
            }
        }

        private void UnbindBackBuffer()
        {
            if (_d3dImage.TryLock(new Duration(TimeSpan.FromMilliseconds(100))))
            {
                try
                {
                    _d3dImage.SetBackBuffer(D3DResourceType.IDirect3DSurface9, IntPtr.Zero);
                }
                finally
                {
                    _d3dImage.Unlock();
                }
            }
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            CompositionTarget.Rendering -= OnCompositionTargetRendering;
            _d3dImage.IsFrontBufferAvailableChanged -= OnFrontBufferAvailableChanged;

            UnbindBackBuffer();

            _pool.Dispose();
            _deviceManager.Dispose();
        }
    }
}