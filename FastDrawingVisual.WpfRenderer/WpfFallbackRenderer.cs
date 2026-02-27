using FastDrawingVisual.Rendering;
using System;
using System.Threading;
using System.Windows.Media;
using System.Windows.Threading;

namespace FastDrawingVisual.WpfRenderer
{
    /// <summary>
    /// 基于 <see cref="DrawingVisual"/> 的纯 WPF 降级渲染器。
    /// <para>
    /// 不依赖 SkiaSharp、SharpDX 或任何 D3D API，在 Windows 7 / 无 D3D12 环境下可用。
    /// </para>
    /// <para>调度模型：</para>
    /// <list type="bullet">
    ///   <item>
    ///     外部通过 <see cref="SubmitDrawing"/> 写入单槽 Replace 队列（任意线程，无锁）。
    ///     首次写入时通过 <see cref="Dispatcher.InvokeAsync"/> 在 <see cref="DispatcherPriority.Render"/>
    ///     优先级向 UI 线程投递一个执行回调；若回调已在队列中则不重复投递。
    ///   </item>
    ///   <item>
    ///     UI 线程在 Render 优先级窗口（VSync 前，紧接在布局/渲染之前）执行回调，
    ///     调用 <see cref="DrawingVisual.RenderOpen"/> 写入新绘制内容。
    ///   </item>
    ///   <item>
    ///     WPF 合成器在下一个 VSync 自动拾取 DrawingVisual 的内容变化并呈现。
    ///   </item>
    /// </list>
    /// <para>
    /// 注意：绘制委托在 <b>UI 线程</b>执行（WPF DrawingContext 要求 UI 线程）。
    /// 委托内请避免耗时操作；复杂的几何计算建议在外部后台线程完成，
    /// 将结果（已 Freeze 的 Geometry/Brush）传入委托。
    /// </para>
    /// </summary>
    public sealed class WpfFallbackRenderer : IRenderer
    {
        private readonly DrawingVisual _visual;
        private readonly Dispatcher    _uiDispatcher;

        // ── 单槽 Replace 队列（无锁） ─────────────────────────────────────────
        private volatile Action<IDrawingContext>? _pendingDrawAction;

        // 0 = UI 回调未入队；1 = 已有回调在 Dispatcher 队列中
        // 防止为同一批 Replace 写入重复投递多次 InvokeAsync，浪费 Dispatcher 周期。
        private int _callbackQueued;

        // ── 状态 ─────────────────────────────────────────────────────────────
        private int  _width;
        private int  _height;
        private bool _isInitialized;
        private bool _isDisposed;

        /// <inheritdoc/>
        public DrawingVisual Visual => _visual;

        public WpfFallbackRenderer()
        {
            _uiDispatcher = Dispatcher.CurrentDispatcher;
            _visual       = new DrawingVisual();
        }

        #region IRenderer

        /// <inheritdoc/>
        public bool Initialize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(WpfFallbackRenderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("宽高必须大于 0。");

            _width         = width;
            _height        = height;
            _isInitialized = true;
            return true;
        }

        /// <inheritdoc/>
        public void Resize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(WpfFallbackRenderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("宽高必须大于 0。");
            // DrawingVisual 无固定尺寸，直接更新记录值；下次绘制自然采用新尺寸
            _width  = width;
            _height = height;
        }

        /// <inheritdoc/>
        /// <remarks>
        /// <para>线程安全，可从任意线程调用。</para>
        /// <para>Replace 语义：若上一个委托尚未由 UI 线程执行，新委托原子替换旧委托。
        /// 同时保证最多只有一个 <see cref="DispatcherPriority.Render"/> 回调在 
        /// Dispatcher 队列中等待，避免积压。</para>
        /// </remarks>
        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(WpfFallbackRenderer));
            if (!_isInitialized) throw new InvalidOperationException("请先调用 Initialize。");
            if (drawAction == null) throw new ArgumentNullException(nameof(drawAction));

            // ── 1. 原子写入 Replace 槽 ────────────────────────────────────────
            Interlocked.Exchange(ref _pendingDrawAction, drawAction);

            // ── 2. 保证只投递一次 Dispatcher 回调 ────────────────────────────
            // CAS：0→1 成功，说明当前无回调在队列中，本次我方负责投递。
            // CAS 失败（已是 1），说明已有回调在队列中，它执行时会取到我们刚写入的最新 action，
            // 无需重复投递。
            if (Interlocked.CompareExchange(ref _callbackQueued, 1, 0) == 0)
            {
                _uiDispatcher.InvokeAsync(ExecutePendingDraw, DispatcherPriority.Render);
            }
        }

        #endregion

        #region UI 线程执行（由 Dispatcher 回调）

        private void ExecutePendingDraw()
        {
            // ── 先清标志，再取 action ────────────────────────────────────────
            //
            // 顺序很关键：
            //   清标志 → 有窗口让新的 SubmitDrawing 再次入队 → 再取 action
            //
            // 这样若新 SubmitDrawing 在"清标志"后、"取 action"前抢入：
            //   · 它存入 new_action，CAS 成功，投递新回调
            //   · 我们 Exchange 取走 new_action，执行它 ✓
            //   · 新投递的回调到达时，_pendingDrawAction 已为 null，安全 no-op ✓
            //
            // 若顺序颠倒（先取 action 后清标志），新 SubmitDrawing 在窗口期存入 
            // next_action 后发现标志仍为 1，不投递回调 → next_action 永远不被执行（丢帧 bug）。
            Interlocked.Exchange(ref _callbackQueued, 0);

            if (_isDisposed || !_isInitialized) return;

            // ── 原子取走最新 action（Replace 语义的"消费端"） ─────────────────
            var action = Interlocked.Exchange(ref _pendingDrawAction, null);
            if (action == null) return;

            // ── 在 UI 线程执行绘制 ────────────────────────────────────────────
            // DrawingVisual.RenderOpen 在 UI 线程调用是安全的，
            // 且 Open 语义：本次绘制完全替换 Visual 的旧内容流。
            using var dc  = _visual.RenderOpen();
            using var ctx = new WpfDrawingContext(dc, _width, _height);
            action(ctx);
            // ctx.Dispose() → dc.Close() → 提交内容流
            // WPF 合成器在下一个 VSync 渲染此帧
        }

        #endregion

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;
            // Dispatcher 回调若已在队列中，执行时会检查 _isDisposed 并安全退出
        }
    }
}
