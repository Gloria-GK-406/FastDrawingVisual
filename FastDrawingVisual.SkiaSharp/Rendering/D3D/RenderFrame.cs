using FastDrawingVisual.Rendering.Skia;
using SkiaSharp;
using System;
using System.Threading;
using SharpDX.Direct3D11;
using Texture2D11 = SharpDX.Direct3D11.Texture2D;
using Texture9 = SharpDX.Direct3D9.Texture;
using Surface9 = SharpDX.Direct3D9.Surface;
using System.Diagnostics;

namespace FastDrawingVisual.Rendering.D3D
{
    /// <summary>
    /// 单个渲染帧缓冲区。
    /// 将 D3D11 共享纹理、D3D9Ex 表面（供 WPF D3DImage 使用）和 Skia CPU 画布
    /// 组织在一起，提供"获取绘制上下文"和"获取呈现表面"两个功能，
    /// 并通过状态机（<see cref="FrameState"/>）管理自身生命周期。
    /// </summary>
    internal sealed class RenderFrame : IDisposable
    {
        private readonly D3DDeviceManager _deviceManager;

        // 绘制完成后的回调，通知 Pool 将本帧标记为 ReadyForPresent
        private readonly Action<RenderFrame> _onDrawingComplete;

        // D3D 资源
        private Texture2D11? _d3d11Texture;
        private Texture2D11? _stagingTexture;   // 持久化 staging，避免每帧分配
        private Texture9? _d3d9Texture;
        private Surface9? _d3d9Surface;

        // GPU 同步查询：在 CopyResource 提交后插入 Event query，
        // 自旋等待 GPU 回信后再标记帧就绪，彻底消除异步读取旧纹理的问题。
        private Query? _syncQuery;

        // Skia CPU 画布
        private SKSurface? _skiaSurface;

        // 状态（int 以便使用 Interlocked）
        private int _state = (int)FrameState.Ready;

        private int _width;
        private int _height;
        private bool _isDisposed;

        /// <summary>当前帧状态。</summary>
        public FrameState State => (FrameState)Volatile.Read(ref _state);

        /// <summary>D3D9Ex 表面的 COM 指针，供 D3DImage.SetBackBuffer 使用。</summary>
        public IntPtr D3D9SurfacePointer => _d3d9Surface?.NativePointer ?? IntPtr.Zero;
        internal Surface9? D3D9Surface => _d3d9Surface;

        public int Width => _width;
        public int Height => _height;

        public RenderFrame(D3DDeviceManager deviceManager, Action<RenderFrame> onDrawingComplete)
        {
            _deviceManager = deviceManager ?? throw new ArgumentNullException(nameof(deviceManager));
            _onDrawingComplete = onDrawingComplete ?? throw new ArgumentNullException(nameof(onDrawingComplete));
        }

        /// <summary>
        /// 创建（或重建）所有 GPU/Skia 资源。
        /// 应在设备初始化后或尺寸变化时调用。
        /// </summary>
        public void CreateResources(int width, int height)
        {
            ReleaseResources();

            _width = width;
            _height = height;

            var device = _deviceManager.D3D11Device;

            // 1. 创建 D3D11 共享纹理（用于渲染和 D3D9 共享）
            _d3d11Texture = _deviceManager.CreateSharedTexture(width, height);

            // 2. 创建持久化 Staging 纹理，供 CPU→GPU 上传复用
            var stagingDesc = new Texture2DDescription
            {
                Width = width,
                Height = height,
                MipLevels = 1,
                ArraySize = 1,
                Format = SharpDX.DXGI.Format.B8G8R8A8_UNorm,
                SampleDescription = new SharpDX.DXGI.SampleDescription(1, 0),
                Usage = ResourceUsage.Staging,
                BindFlags = BindFlags.None,
                CpuAccessFlags = CpuAccessFlags.Write,
                OptionFlags = ResourceOptionFlags.None,
            };
            _stagingTexture = new Texture2D11(device, stagingDesc);

            // 3. 通过共享句柄创建 D3D9Ex 表面（WPF D3DImage 唯一接受 D3D9 表面）
            var sharedHandle = _deviceManager.GetSharedHandle(_d3d11Texture);
            _d3d9Texture = _deviceManager.FromSharedHandle(sharedHandle, width, height, out var surface);
            _d3d9Surface = surface;

            // 4. 创建 Skia CPU 画布（BGRA8888 与 D3D11 B8G8R8A8 内存布局一致，可直接 memcpy）
            var info = new SKImageInfo(width, height, SKColorType.Bgra8888, SKAlphaType.Premul,
                                       SKColorSpace.CreateSrgb());
            _skiaSurface = SKSurface.Create(info)
                ?? throw new InvalidOperationException("无法创建 SKSurface。");

            // 5. 创建持久化 GPU Event Query，用于精确等待 CopyResource 完成
            var queryDesc = new QueryDescription
            {
                Type = QueryType.Event,
                Flags = QueryFlags.None,
            };
            _syncQuery = new Query(device, queryDesc);

            // 重置为可用状态
            Interlocked.Exchange(ref _state, (int)FrameState.Ready);
        }

        /// <summary>释放所有 GPU/Skia 资源，状态不变。</summary>
        public void ReleaseResources()
        {
            _skiaSurface?.Dispose();
            _skiaSurface = null;

            _syncQuery?.Dispose();
            _syncQuery = null;

            _d3d9Surface?.Dispose();
            _d3d9Surface = null;

            _d3d9Texture?.Dispose();
            _d3d9Texture = null;

            _stagingTexture?.Dispose();
            _stagingTexture = null;

            _d3d11Texture?.Dispose();
            _d3d11Texture = null;
        }

        /// <summary>
        /// 打开绘制上下文。帧必须处于 <see cref="FrameState.Drawing"/> 状态（由 Pool 保证）。
        /// 返回的上下文 Close/Dispose 时会自动触发上传和回调。
        /// </summary>
        public IDrawingContext OpenCanvas()
        {
            if (_skiaSurface == null)
                throw new InvalidOperationException("资源未创建，请先调用 CreateResources。");

            // 每次绘制前清空画布
            _skiaSurface.Canvas.Clear(SKColors.Transparent);

            return new SkiaDrawingContext(_skiaSurface.Canvas, _width, _height, OnCanvasClosed);
        }

        /// <summary>
        /// 由 <see cref="SkiaDrawingContext"/> 在 Close/Dispose 时回调。
        /// 在此完成 Skia→D3D11 的像素上传，然后通知 Pool。
        /// </summary>
        private void OnCanvasClosed()
        {
            if (_isDisposed || _skiaSurface == null) return;

            // Skia flush（CPU 模式下是 no-op，但保持语义正确）
            _skiaSurface.Canvas.Flush();

            // CPU → D3D11 上传（包含 GPU 同步等待，完成后才返回）
            UploadToD3D11();

            // 通知 Pool：本帧已就绪，请将状态推进到 ReadyForPresent
            _onDrawingComplete(this);
        }

        /// <summary>
        /// 将 Skia CPU 画布的像素写入持久化 Staging 纹理，再 CopyResource 到共享 D3D11 纹理。
        /// 使用 D3D11 Event Query 精确等待 GPU 完成复制，确保 D3D9 共享表面内容完整后再通知帧池。
        /// 由于同一时刻只有一帧处于 Drawing 状态，此处无须额外加锁。
        /// </summary>
        private unsafe void UploadToD3D11()
        {
            if (_skiaSurface == null || _d3d11Texture == null || _stagingTexture == null || _syncQuery == null)
                return;

            // PeekPixels 对 CPU 画布是零拷贝访问
            var pixmap = _skiaSurface.PeekPixels();
            if (pixmap == null)
                return;

            var context = _deviceManager.D3D11Device.ImmediateContext;

            // ── 第一步：CPU 像素 → Staging 纹理 ─────────────────────────────────
            // MapSubresource 会隐式同步等待 GPU 对 staging 的访问完成（Write 模式）。
            // 由于 Event Query 自旋已确认上一次 CopyResource 完成，此处通常立即返回。
            var mapped = context.MapSubresource(_stagingTexture, 0, MapMode.Write, MapFlags.None);
            try
            {
                var src = (byte*)pixmap.GetPixels().ToPointer();
                var dst = (byte*)mapped.DataPointer.ToPointer();
                int srcRowBytes = _width * 4;

                if (mapped.RowPitch == srcRowBytes)
                {
                    // ── 快速路径：行跨度与像素行字节数相等，帧数据在内存中完全连续 ──
                    // 单次 bulk 拷贝：消除 _height 次函数调用，允许更大粒度 SIMD 传输。
                    // 常见对齐分辨率（1920/1280/960/800 等宽度 × 4 字节恰好是 256 的倍数）
                    // 均命中此路径。
                    System.Buffer.MemoryCopy(src, dst, (long)srcRowBytes * _height, (long)srcRowBytes * _height);
                }
                else
                {
                    // ── 慢速路径：GPU 驱动对行尾做了填充对齐，必须逐行拷贝 ──
                    for (int y = 0; y < _height; y++)
                    {
                        System.Buffer.MemoryCopy(
                            src + (long)y * srcRowBytes,
                            dst + (long)y * mapped.RowPitch,
                            srcRowBytes,
                            srcRowBytes);
                    }
                }
            }
            finally
            {
                context.UnmapSubresource(_stagingTexture, 0);
            }

            // ── 第二步：Staging → 共享纹理（GPU 拷贝） ───────────────────────────
            // SharpDX CopyResource 签名为 (source, destination)，内部再按 D3D11 顺序转发
            context.CopyResource(_stagingTexture, _d3d11Texture);

            // ── 第三步：插入 GPU Event Query fence，阻塞直到 CopyResource 真正完成 ──
            // End() 把 fence 标记插入 GPU 命令流。
            // Flush() 确保命令队列被立即提交给 GPU（否则命令可能仍滞留在驱动缓冲区中，
            //   导致 GetData 永远轮询不到完成信号）。
            // 注意：D3D11 Event Query 的数据类型是 BOOL（4 字节 int），
            //   不能用 C# bool（1 字节）——大小不匹配会导致读到错误值，出现死循环。
            context.End(_syncQuery);
            context.Flush(); // 必须显式 Flush，才能确保 End() 标记被提交给 GPU

            // 自旋等待 GPU 到达 fence 点（一次 CopyResource 通常 < 0.1 ms）
            // GetData<int> 对应 D3D11 BOOL（4 字节）；非零值表示 GPU 已完成
            while (!context.GetData<int>(_syncQuery, AsynchronousFlags.DoNotFlush, out _))
            {
                Thread.Sleep(0); // 让出 CPU 时间片，避免纯忙等
            }
        }

        /// <summary>
        /// CAS 状态转换：仅当当前状态等于 <paramref name="from"/> 时才转换到 <paramref name="to"/>。
        /// </summary>
        internal bool TryTransitionTo(FrameState from, FrameState to)
            => Interlocked.CompareExchange(ref _state, (int)to, (int)from) == (int)from;

        /// <summary>强制设置状态（供 Pool 在持锁时使用）。</summary>
        internal void ForceSetState(FrameState state)
            => Interlocked.Exchange(ref _state, (int)state);

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;
            ReleaseResources();
        }
    }
}
