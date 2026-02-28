using FastDrawingVisual.Rendering;
using System;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisual.Rendering.NativeD3D9
{
    internal sealed class NativeDrawingContext : IDrawingContext
    {
        private readonly NativeCommandBuffer _commands;
        private readonly Action<ReadOnlyMemory<byte>> _onClose;
        private bool _isDisposed;

        public int Width { get; }
        public int Height { get; }

        public NativeDrawingContext(int width, int height, Action<ReadOnlyMemory<byte>> onClose)
        {
            Width = width;
            Height = height;
            _onClose = onClose ?? throw new ArgumentNullException(nameof(onClose));
            _commands = new NativeCommandBuffer();
        }

        public void DrawEllipse(Brush brush, Pen pen, Point center, double radiusX, double radiusY)
        {
            ThrowIfDisposed();

            if (TryGetSolidColor(brush, out var fill))
                _commands.WriteFillEllipse(center, (float)radiusX, (float)radiusY, fill);

            if (TryGetSolidPen(pen, out var stroke, out var thickness))
                _commands.WriteStrokeEllipse(center, (float)radiusX, (float)radiusY, thickness, stroke);
        }

        public void DrawRectangle(Brush brush, Pen pen, Rect rectangle)
        {
            ThrowIfDisposed();

            if (TryGetSolidColor(brush, out var fill))
                _commands.WriteFillRect(rectangle, fill);

            if (TryGetSolidPen(pen, out var stroke, out var thickness))
                _commands.WriteStrokeRect(rectangle, thickness, stroke);
        }

        public void DrawRoundedRectangle(Brush brush, Pen pen, Rect rectangle, double radiusX, double radiusY)
        {
            // MVP: rounded rectangle falls back to regular rectangle.
            DrawRectangle(brush, pen, rectangle);
        }

        public void DrawLine(Pen pen, Point point0, Point point1)
        {
            ThrowIfDisposed();
            if (TryGetSolidPen(pen, out var color, out var thickness))
                _commands.WriteLine(point0, point1, thickness, color);
        }

        public void DrawGeometry(Brush brush, Pen pen, Geometry geometry)
        {
            ThrowIfDisposed();
            // MVP: complex geometry is intentionally skipped.
        }

        public void DrawImage(ImageSource imageSource, Rect rectangle)
        {
            ThrowIfDisposed();
            // MVP: image drawing is intentionally skipped.
        }

        public void DrawText(string text, Point origin, Brush foreground, string fontFamily = "Segoe UI", double fontSize = 12)
        {
            ThrowIfDisposed();
            // MVP: text drawing is intentionally skipped.
        }

        public void DrawGlyphRun(Brush foregroundBrush, GlyphRun glyphRun)
        {
            ThrowIfDisposed();
            // MVP: glyph run is intentionally skipped.
        }

        public void DrawDrawing(Drawing drawing)
        {
            ThrowIfDisposed();
            // MVP: nested drawing is intentionally skipped.
        }

        public void PushClip(Geometry clipGeometry)
        {
            ThrowIfDisposed();
            // MVP: clipping is intentionally skipped.
        }

        public void PushGuidelineSet(GuidelineSet guidelines)
        {
            ThrowIfDisposed();
            // MVP: guidelines are intentionally skipped.
        }

        public void PushOpacity(double opacity)
        {
            ThrowIfDisposed();
            // MVP: opacity stack is intentionally skipped.
        }

        public void PushOpacityMask(Brush opacityMask)
        {
            ThrowIfDisposed();
            // MVP: opacity mask is intentionally skipped.
        }

        public void PushTransform(Transform transform)
        {
            ThrowIfDisposed();
            // MVP: transform stack is intentionally skipped.
        }

        public void Pop()
        {
            ThrowIfDisposed();
            // MVP: transform/clip stacks are intentionally skipped.
        }

        public void Close() => Dispose();

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;
            _onClose(_commands.WrittenMemory);
        }

        private void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(NativeDrawingContext));
        }

        private static bool TryGetSolidColor(Brush brush, out Color color)
        {
            if (brush is SolidColorBrush solid)
            {
                color = solid.Color;
                return true;
            }

            color = Colors.Transparent;
            return false;
        }

        private static bool TryGetSolidPen(Pen pen, out Color color, out float thickness)
        {
            if (pen != null && pen.Thickness > 0 && pen.Brush is SolidColorBrush solid)
            {
                color = solid.Color;
                thickness = (float)pen.Thickness;
                return true;
            }

            color = Colors.Transparent;
            thickness = 0f;
            return false;
        }
    }
}
