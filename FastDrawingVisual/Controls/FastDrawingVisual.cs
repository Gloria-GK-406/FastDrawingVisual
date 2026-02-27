using FastDrawingVisual.Rendering;
using System;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisual.Controls
{
    /// <summary>
    /// 高性能渲染 WPF 控件。
    /// <para>
    /// 通过 <see cref="RendererFactory"/> 在运行时选择最优实现：
    /// <list type="bullet">
    ///   <item>Windows 10+（有 D3D11 + d3d12.dll）→ <c>D3DSkiaRenderer</c>（GPU + Skia）</item>
    ///   <item>其余环境 → <c>WpfFallbackRenderer</c>（纯 WPF DrawingVisual）</item>
    /// </list>
    /// </para>
    /// <para>
    /// 使用方式：<br/>
    ///   XAML 中放置控件，在代码中调用 <see cref="SubmitDrawing"/> 提交绘制委托。<br/>
    ///   委托接收 <see cref="IDrawingContext"/>，可在任意线程调用（线程安全）。
    /// </para>
    /// </summary>
    public class FastDrawingVisual : FrameworkElement, IDisposable
    {
        private IRenderer? _renderer;
        private bool _visualAdded;
        private bool _isDisposed;

        // ── 公共状态 ──────────────────────────────────────────────────────────

        /// <summary>渲染器已初始化且可接受绘制请求。</summary>
        public bool IsReady => _renderer != null && _visualAdded && !_isDisposed;

        // ── WPF 视觉树托管 ────────────────────────────────────────────────────

        protected override int VisualChildrenCount => _visualAdded ? 1 : 0;

        protected override Visual GetVisualChild(int index)
        {
            if (index != 0 || !_visualAdded || _renderer == null)
                throw new ArgumentOutOfRangeException(nameof(index));
            return _renderer.Visual;
        }

        // ── 构造与生命周期 ────────────────────────────────────────────────────

        public FastDrawingVisual()
        {
            Loaded   += OnLoaded;
            Unloaded += OnUnloaded;
        }

        private void OnLoaded(object sender, RoutedEventArgs e)
        {
            SizeChanged += OnSizeChanged;
            EnsureInitialized();
        }

        private void OnUnloaded(object sender, RoutedEventArgs e)
        {
            SizeChanged -= OnSizeChanged;
        }

        // ── 初始化 / Resize ───────────────────────────────────────────────────

        private void EnsureInitialized()
        {
            var (px, py) = GetPixelSize();
            if (px <= 0 || py <= 0) return;

            if (_renderer == null)
            {
                // 首次：工厂按运行时能力选择实现
                _renderer = RendererFactory.Create();

                if (_renderer.Initialize(px, py))
                {
                    AddVisualChild(_renderer.Visual);
                    _visualAdded = true;
                }
            }
            else if (_visualAdded)
            {
                _renderer.Resize(px, py);
            }
        }

        private void OnSizeChanged(object sender, SizeChangedEventArgs e)
            => EnsureInitialized();

        // ── 公共 API ──────────────────────────────────────────────────────────

        /// <summary>
        /// 向内部调度器提交绘制委托（任意线程，线程安全）。
        /// <para>Replace 语义：新委托原子替换未执行的旧委托（旧委托有后继，丢弃安全）。</para>
        /// 控件未就绪时静默丢弃（不抛出异常）。
        /// </summary>
        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(FastDrawingVisual));
            if (!IsReady) return;
            _renderer!.SubmitDrawing(drawAction);
        }

        // ── 布局 ─────────────────────────────────────────────────────────────

        protected override Size MeasureOverride(Size availableSize)
        {
            // 填充所有可用空间
            return availableSize;
        }

        protected override Size ArrangeOverride(Size finalSize)
        {
            if (_visualAdded && _renderer != null)
            {
                // 让 DrawingVisual 占满控件区域
                _renderer.Visual.Offset = new Vector(0, 0);
            }
            return finalSize;
        }

        // ── DPI 感知像素尺寸 ──────────────────────────────────────────────────

        private (int width, int height) GetPixelSize()
        {
            var dpi = VisualTreeHelper.GetDpi(this);
            return (
                (int)Math.Round(ActualWidth  * dpi.DpiScaleX),
                (int)Math.Round(ActualHeight * dpi.DpiScaleY)
            );
        }

        // ── 释放 ──────────────────────────────────────────────────────────────

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            Loaded   -= OnLoaded;
            Unloaded -= OnUnloaded;
            SizeChanged -= OnSizeChanged;

            if (_visualAdded && _renderer != null)
            {
                RemoveVisualChild(_renderer.Visual);
                _visualAdded = false;
            }

            _renderer?.Dispose();
            _renderer = null;
        }
    }
}
