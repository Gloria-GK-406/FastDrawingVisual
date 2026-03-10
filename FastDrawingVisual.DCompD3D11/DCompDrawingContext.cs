using FastDrawingVisual.CommandRuntime;
using FastDrawingVisual.Rendering;
using System;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisual.DCompD3D11
{
    internal sealed class DCompDrawingContext : IDrawingContext
    {
        private readonly BridgeCommandWriter _commands;
        private readonly Action<BridgeCommandPacket> _onClose;
        private bool _isDisposed;

        public int Width { get; }
        public int Height { get; }

        public DCompDrawingContext(int width, int height, BridgeCommandWriter commandWriter, Action<BridgeCommandPacket> onClose)
        {
            Width = width;
            Height = height;
            _commands = commandWriter ?? throw new ArgumentNullException(nameof(commandWriter));
            _onClose = onClose ?? throw new ArgumentNullException(nameof(onClose));
            _commands.Reset();
        }

        public void DrawEllipse(Brush brush, Pen pen, Point center, double radiusX, double radiusY)
        {
            ThrowIfDisposed();

            if (TryGetSolidColor(brush, out var fill))
                _commands.WriteFillEllipse((float)center.X, (float)center.Y, (float)radiusX, (float)radiusY, ToProtocolColor(fill));

            if (TryGetSolidPen(pen, out var stroke, out var thickness))
                _commands.WriteStrokeEllipse((float)center.X, (float)center.Y, (float)radiusX, (float)radiusY, thickness, ToProtocolColor(stroke));
        }

        public void DrawRectangle(Brush brush, Pen pen, Rect rectangle)
        {
            ThrowIfDisposed();

            if (TryGetSolidColor(brush, out var fill))
                _commands.WriteFillRect((float)rectangle.X, (float)rectangle.Y, (float)rectangle.Width, (float)rectangle.Height, ToProtocolColor(fill));

            if (TryGetSolidPen(pen, out var stroke, out var thickness))
                _commands.WriteStrokeRect((float)rectangle.X, (float)rectangle.Y, (float)rectangle.Width, (float)rectangle.Height, thickness, ToProtocolColor(stroke));
        }

        public void DrawRoundedRectangle(Brush brush, Pen pen, Rect rectangle, double radiusX, double radiusY)
        {
            DrawRectangle(brush, pen, rectangle);
        }

        public void DrawLine(Pen pen, Point point0, Point point1)
        {
            ThrowIfDisposed();
            if (TryGetSolidPen(pen, out var color, out var thickness))
                _commands.WriteLine((float)point0.X, (float)point0.Y, (float)point1.X, (float)point1.Y, thickness, ToProtocolColor(color));
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
            if (TryGetSolidColor(foreground, out var color))
            {
                if (string.IsNullOrEmpty(text))
                    return;

                if (string.IsNullOrWhiteSpace(fontFamily))
                    fontFamily = "Segoe UI";

                if (fontSize <= 0d)
                    fontSize = 12d;

                _commands.WriteDrawText(
                    (float)origin.X,
                    (float)origin.Y,
                    (float)fontSize,
                    ToProtocolColor(color),
                    text,
                    fontFamily);
            }
        }

        public void DrawGlyphRun(Brush foregroundBrush, GlyphRun glyphRun)
        {
            ThrowIfDisposed();
            // Per current D3D11 scope: glyph drawing is intentionally skipped.
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
            // MVP: stack operations are intentionally skipped.
        }

        public void Close() => Dispose();

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;
            _onClose(_commands.BuildPacket());
        }

        private void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(DCompDrawingContext));
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

        private static BridgeCommandColorArgb8 ToProtocolColor(Color color)
        {
            return new BridgeCommandColorArgb8(color.A, color.R, color.G, color.B);
        }
    }
}
