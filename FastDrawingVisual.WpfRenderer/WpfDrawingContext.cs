using FastDrawingVisual.Rendering;
using System;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisual.WpfRenderer
{
    /// <summary>
    /// 将 WPF <see cref="DrawingContext"/> 包装为 <see cref="IDrawingContext"/>（降级路径）。
    /// Close/Dispose 时调用 <see cref="DrawingContext.Close"/>，提交本次内容到 DrawingVisual。
    /// </summary>
    internal sealed class WpfDrawingContext : IDrawingContext
    {
        private readonly DrawingContext _dc;
        private readonly int _width;
        private readonly int _height;
        private bool _isDisposed;

        public int Width  => _width;
        public int Height => _height;

        internal WpfDrawingContext(DrawingContext dc, int width, int height)
        {
            _dc     = dc ?? throw new ArgumentNullException(nameof(dc));
            _width  = width;
            _height = height;
        }

        private void ThrowIfDisposed()
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(WpfDrawingContext));
        }

        // ── 形状绘制 ─────────────────────────────────────────────────────────

        public void DrawEllipse(Brush brush, Pen pen, Point center, double radiusX, double radiusY)
            { ThrowIfDisposed(); _dc.DrawEllipse(brush, pen, center, radiusX, radiusY); }

        public void DrawRectangle(Brush brush, Pen pen, Rect rectangle)
            { ThrowIfDisposed(); _dc.DrawRectangle(brush, pen, rectangle); }

        public void DrawRoundedRectangle(Brush brush, Pen pen, Rect rectangle, double radiusX, double radiusY)
            { ThrowIfDisposed(); _dc.DrawRoundedRectangle(brush, pen, rectangle, radiusX, radiusY); }

        public void DrawLine(Pen pen, Point point0, Point point1)
            { ThrowIfDisposed(); _dc.DrawLine(pen, point0, point1); }

        public void DrawGeometry(Brush brush, Pen pen, Geometry geometry)
            { ThrowIfDisposed(); _dc.DrawGeometry(brush, pen, geometry); }

        // ── 图像与文本 ────────────────────────────────────────────────────────

        public void DrawImage(ImageSource imageSource, Rect rectangle)
            { ThrowIfDisposed(); _dc.DrawImage(imageSource, rectangle); }

        public void DrawText(string text, Point origin, Brush foreground,
                             string fontFamily = "Segoe UI", double fontSize = 12)
        {
            ThrowIfDisposed();
            if (string.IsNullOrEmpty(text) || foreground == null) return;

            // FormattedText 在 UI 线程（本实现保证委托在 UI 线程执行）
            var ft = new FormattedText(
                text,
                System.Globalization.CultureInfo.CurrentCulture,
                FlowDirection.LeftToRight,
                new Typeface(fontFamily),
                fontSize,
                foreground,
                VisualTreeHelper.GetDpi(new System.Windows.Media.DrawingVisual()).PixelsPerDip);

            _dc.DrawText(ft, origin);
        }

        public void DrawGlyphRun(Brush foregroundBrush, GlyphRun glyphRun)
            { ThrowIfDisposed(); if (glyphRun != null) _dc.DrawGlyphRun(foregroundBrush, glyphRun); }

        public void DrawDrawing(Drawing drawing)
            { ThrowIfDisposed(); if (drawing != null) _dc.DrawDrawing(drawing); }

        // ── 状态管理 ─────────────────────────────────────────────────────────

        public void PushClip(Geometry clipGeometry)
            { ThrowIfDisposed(); _dc.PushClip(clipGeometry); }

        public void PushGuidelineSet(GuidelineSet guidelines)
            { ThrowIfDisposed(); _dc.PushGuidelineSet(guidelines); }

        public void PushOpacity(double opacity)
            { ThrowIfDisposed(); _dc.PushOpacity(opacity); }

        public void PushOpacityMask(Brush opacityMask)
            { ThrowIfDisposed(); _dc.PushOpacityMask(opacityMask); }

        public void PushTransform(Transform transform)
            { ThrowIfDisposed(); _dc.PushTransform(transform); }

        public void Pop()
            { ThrowIfDisposed(); _dc.Pop(); }

        // ── 生命周期 ─────────────────────────────────────────────────────────

        public void Close()   => Dispose();

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;
            _dc.Close(); // 提交内容流到 DrawingVisual
        }
    }
}
