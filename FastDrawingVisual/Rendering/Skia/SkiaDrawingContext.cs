using SkiaSharp;
using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisual.Rendering.Skia
{
    /// <summary>
    /// 基于 Skia 的绘图上下文实现。
    /// 封装 <see cref="SKCanvas"/>，提供与 WPF DrawingContext 兼容的 API。
    /// Close/Dispose 时回调通知 <see cref="D3D.RenderFrame"/> 完成上传流程。
    /// </summary>
    internal sealed class SkiaDrawingContext : IDrawingContext
    {
        private readonly SKCanvas    _canvas;
        private readonly int         _width;
        private readonly int         _height;
        private readonly Action      _onClose;
        private readonly Stack<int>  _saveCountStack;
        private bool _isDisposed;

        public int Width  => _width;
        public int Height => _height;

        /// <param name="canvas">由 RenderFrame 提供的 Skia 画布。</param>
        /// <param name="width">渲染宽度。</param>
        /// <param name="height">渲染高度。</param>
        /// <param name="onClose">Close/Dispose 时触发的回调（通知 RenderFrame）。</param>
        internal SkiaDrawingContext(SKCanvas canvas, int width, int height, Action onClose)
        {
            _canvas         = canvas  ?? throw new ArgumentNullException(nameof(canvas));
            _width          = width;
            _height         = height;
            _onClose        = onClose ?? throw new ArgumentNullException(nameof(onClose));
            _saveCountStack = new Stack<int>();
        }

        private void ThrowIfDisposed()
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(SkiaDrawingContext));
        }

        // ── 形状绘制 ──────────────────────────────────────────

        public void DrawEllipse(Brush brush, Pen pen, Point center, double radiusX, double radiusY)
        {
            ThrowIfDisposed();
            var rect = new SKRect(
                (float)(center.X - radiusX), (float)(center.Y - radiusY),
                (float)(center.X + radiusX), (float)(center.Y + radiusY));

            if (brush != null)
            {
                using var fillPaint = WpfToSkiaConverter.ToSkiaPaint(brush);
                if (fillPaint != null) _canvas.DrawOval(rect, fillPaint);
            }
            if (pen != null)
            {
                using var strokePaint = WpfToSkiaConverter.ToSkiaPaint(pen);
                if (strokePaint != null) _canvas.DrawOval(rect, strokePaint);
            }
        }

        public void DrawRectangle(Brush brush, Pen pen, Rect rectangle)
        {
            ThrowIfDisposed();
            var rect = WpfToSkiaConverter.ToSkiaRect(rectangle);

            if (brush != null)
            {
                using var fillPaint = WpfToSkiaConverter.ToSkiaPaint(brush);
                if (fillPaint != null) _canvas.DrawRect(rect, fillPaint);
            }
            if (pen != null)
            {
                using var strokePaint = WpfToSkiaConverter.ToSkiaPaint(pen);
                if (strokePaint != null) _canvas.DrawRect(rect, strokePaint);
            }
        }

        public void DrawRoundedRectangle(Brush brush, Pen pen, Rect rectangle, double radiusX, double radiusY)
        {
            ThrowIfDisposed();
            var rect = WpfToSkiaConverter.ToSkiaRect(rectangle);

            if (brush != null)
            {
                using var fillPaint = WpfToSkiaConverter.ToSkiaPaint(brush);
                if (fillPaint != null) _canvas.DrawRoundRect(rect, (float)radiusX, (float)radiusY, fillPaint);
            }
            if (pen != null)
            {
                using var strokePaint = WpfToSkiaConverter.ToSkiaPaint(pen);
                if (strokePaint != null) _canvas.DrawRoundRect(rect, (float)radiusX, (float)radiusY, strokePaint);
            }
        }

        public void DrawLine(Pen pen, Point point0, Point point1)
        {
            ThrowIfDisposed();
            if (pen == null) return;

            using var strokePaint = WpfToSkiaConverter.ToSkiaPaint(pen);
            if (strokePaint != null)
                _canvas.DrawLine((float)point0.X, (float)point0.Y, (float)point1.X, (float)point1.Y, strokePaint);
        }

        public void DrawGeometry(Brush brush, Pen pen, Geometry geometry)
        {
            ThrowIfDisposed();
            if (geometry == null) return;

            using var path = WpfToSkiaConverter.ToSkiaPath(geometry);
            if (brush != null)
            {
                using var fillPaint = WpfToSkiaConverter.ToSkiaPaint(brush);
                if (fillPaint != null) _canvas.DrawPath(path, fillPaint);
            }
            if (pen != null)
            {
                using var strokePaint = WpfToSkiaConverter.ToSkiaPaint(pen);
                if (strokePaint != null) _canvas.DrawPath(path, strokePaint);
            }
        }

        // ── 图像与文本 ────────────────────────────────────────

        public void DrawImage(ImageSource imageSource, Rect rectangle)
        {
            ThrowIfDisposed();
            if (imageSource == null) return;

            using var bitmap = WpfToSkiaConverter.ToSkiaBitmap(imageSource);
            if (bitmap != null)
                _canvas.DrawBitmap(bitmap, WpfToSkiaConverter.ToSkiaRect(rectangle));
        }

        public void DrawText(string text, Point origin, Brush foreground,
                             string fontFamily = "Segoe UI", double fontSize = 12)
        {
            ThrowIfDisposed();
            if (string.IsNullOrEmpty(text) || foreground == null) return;

            using var paint = WpfToSkiaConverter.ToSkiaPaint(foreground);
            if (paint == null) return;

            paint.IsAntialias = true;
            using var typeface = SKTypeface.FromFamilyName(fontFamily);
            using var font = new SKFont(typeface, (float)fontSize);
            _canvas.DrawText(text, (float)origin.X, (float)(origin.Y + fontSize),
                             SKTextAlign.Left, font, paint);
        }

        public void DrawGlyphRun(Brush foregroundBrush, GlyphRun glyphRun)
        {
            ThrowIfDisposed();
            // TODO: 完整 GlyphRun 支持需解析字形和位置
        }

        public void DrawDrawing(Drawing drawing)
        {
            ThrowIfDisposed();
            if (drawing == null) return;
            DrawDrawingInternal(drawing);
        }

        private void DrawDrawingInternal(Drawing drawing)
        {
            switch (drawing)
            {
                case GeometryDrawing gd:
                    DrawGeometry(gd.Brush, gd.Pen, gd.Geometry);
                    break;
                case ImageDrawing id:
                    DrawImage(id.ImageSource, id.Rect);
                    break;
                case DrawingGroup dg:
                    foreach (var child in dg.Children) DrawDrawingInternal(child);
                    break;
                case GlyphRunDrawing gr:
                    DrawGlyphRun(gr.ForegroundBrush, gr.GlyphRun);
                    break;
            }
        }

        // ── 状态管理 ──────────────────────────────────────────

        public void PushClip(Geometry clipGeometry)
        {
            ThrowIfDisposed();
            if (clipGeometry == null) return;
            _saveCountStack.Push(_canvas.Save());
            using var path = WpfToSkiaConverter.ToSkiaPath(clipGeometry);
            _canvas.ClipPath(path);
        }

        public void PushGuidelineSet(GuidelineSet guidelines)
        {
            ThrowIfDisposed();
            // Skia 无对应概念，跳过
        }

        public void PushOpacity(double opacity)
        {
            ThrowIfDisposed();
            using var paint = new SKPaint { Color = SKColors.White.WithAlpha((byte)(opacity * 255)) };
            _saveCountStack.Push(_canvas.SaveLayer(paint));
        }

        public void PushOpacityMask(Brush opacityMask)
        {
            ThrowIfDisposed();
            _saveCountStack.Push(_canvas.Save());
        }

        public void PushTransform(Transform transform)
        {
            ThrowIfDisposed();
            if (transform == null) return;
            _saveCountStack.Push(_canvas.Save());
            var matrix = WpfToSkiaConverter.ToSkiaMatrix(transform);
            _canvas.SetMatrix(_canvas.TotalMatrix.PreConcat(matrix));
        }

        public void Pop()
        {
            ThrowIfDisposed();
            if (_saveCountStack.Count > 0)
                _canvas.RestoreToCount(_saveCountStack.Pop());
        }

        // ── 生命周期 ──────────────────────────────────────────

        public void Close()   => Dispose();

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            // Flush（CPU 模式 no-op，GPU 模式确保提交）
            _canvas.Flush();

            // 回调 RenderFrame.OnCanvasClosed → 上传 → 通知 Pool
            _onClose.Invoke();
        }
    }
}
