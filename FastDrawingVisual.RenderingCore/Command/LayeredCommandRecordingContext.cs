using FastDrawingVisual.CommandRuntime;
using System;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisual.Rendering
{
    public sealed class LayeredCommandRecordingContext : IDrawingContext, ILayeredDrawingContextContainer
    {
        private readonly Action<BridgeLayeredFramePacket> _onClose;
        private readonly LayerCommandWriter?[] _layers = new LayerCommandWriter[BridgeLayeredFramePacket.MaxLayerCount];
        private readonly object _sync = new();
        private bool _isDisposed;

        public LayeredCommandRecordingContext(int width, int height, Action<BridgeLayeredFramePacket> onClose)
        {
            Width = width;
            Height = height;
            _onClose = onClose ?? throw new ArgumentNullException(nameof(onClose));
            _layers[0] = new LayerCommandWriter(this, 0);
        }

        public int Width { get; }

        public int Height { get; }

        public int LayerCount => BridgeLayeredFramePacket.MaxLayerCount;

        public IDrawingContext GetLayer(int layerIndex)
        {
            ThrowIfDisposed();
            ValidateLayerIndex(layerIndex);
            return GetOrCreateLayer(layerIndex);
        }

        public void DrawEllipse(Brush brush, Pen pen, Point center, double radiusX, double radiusY)
            => GetOrCreateLayer(0).DrawEllipse(brush, pen, center, radiusX, radiusY);

        public void DrawRectangle(Brush brush, Pen pen, Rect rectangle)
            => GetOrCreateLayer(0).DrawRectangle(brush, pen, rectangle);

        public void DrawRoundedRectangle(Brush brush, Pen pen, Rect rectangle, double radiusX, double radiusY)
            => GetOrCreateLayer(0).DrawRoundedRectangle(brush, pen, rectangle, radiusX, radiusY);

        public void DrawLine(Pen pen, Point point0, Point point1)
            => GetOrCreateLayer(0).DrawLine(pen, point0, point1);

        public void DrawGeometry(Brush brush, Pen pen, Geometry geometry)
            => GetOrCreateLayer(0).DrawGeometry(brush, pen, geometry);

        public void DrawImage(ImageSource imageSource, Rect rectangle)
            => GetOrCreateLayer(0).DrawImage(imageSource, rectangle);

        public void DrawText(string text, Point origin, Brush foreground, string fontFamily = "Segoe UI", double fontSize = 12)
            => GetOrCreateLayer(0).DrawText(text, origin, foreground, fontFamily, fontSize);

        public void DrawGlyphRun(Brush foregroundBrush, GlyphRun glyphRun)
            => GetOrCreateLayer(0).DrawGlyphRun(foregroundBrush, glyphRun);

        public void DrawDrawing(Drawing drawing)
            => GetOrCreateLayer(0).DrawDrawing(drawing);

        public void PushClip(Geometry clipGeometry)
            => GetOrCreateLayer(0).PushClip(clipGeometry);

        public void PushGuidelineSet(GuidelineSet guidelines)
            => GetOrCreateLayer(0).PushGuidelineSet(guidelines);

        public void PushOpacity(double opacity)
            => GetOrCreateLayer(0).PushOpacity(opacity);

        public void PushOpacityMask(Brush opacityMask)
            => GetOrCreateLayer(0).PushOpacityMask(opacityMask);

        public void PushTransform(Transform transform)
            => GetOrCreateLayer(0).PushTransform(transform);

        public void Pop()
            => GetOrCreateLayer(0).Pop();

        public void Close() => Dispose();

        public void Dispose()
        {
            if (_isDisposed)
                return;

            var packet = new BridgeLayeredFramePacket();

            try
            {
                for (int i = 0; i < _layers.Length; i++)
                {
                    var layer = _layers[i];
                    if (layer == null)
                        continue;

                    packet.SetLayer(i, layer.BuildPacket());
                }

                if (packet.HasAnyCommands)
                    _onClose(packet);
            }
            finally
            {
                _isDisposed = true;
                for (int i = 0; i < _layers.Length; i++)
                    _layers[i]?.DisposeWriter();
            }
        }

        internal void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(LayeredCommandRecordingContext));
        }

        private LayerCommandWriter GetOrCreateLayer(int layerIndex)
        {
            var existing = _layers[layerIndex];
            if (existing != null)
                return existing;

            lock (_sync)
            {
                existing = _layers[layerIndex];
                if (existing == null)
                {
                    existing = new LayerCommandWriter(this, layerIndex);
                    _layers[layerIndex] = existing;
                }

                return existing;
            }
        }

        private static void ValidateLayerIndex(int layerIndex)
        {
            if ((uint)layerIndex >= BridgeLayeredFramePacket.MaxLayerCount)
                throw new ArgumentOutOfRangeException(nameof(layerIndex));
        }

        private sealed class LayerCommandWriter : IDrawingContext
        {
            private readonly LayeredCommandRecordingContext _root;
            private readonly BridgeCommandWriter _commands = new();

            public LayerCommandWriter(LayeredCommandRecordingContext root, int layerIndex)
            {
                _root = root ?? throw new ArgumentNullException(nameof(root));
                LayerIndex = layerIndex;
            }

            public int LayerIndex { get; }

            public int Width => _root.Width;

            public int Height => _root.Height;

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
            }

            public void DrawImage(ImageSource imageSource, Rect rectangle)
            {
                ThrowIfDisposed();
            }

            public void DrawText(string text, Point origin, Brush foreground, string fontFamily = "Segoe UI", double fontSize = 12)
            {
                ThrowIfDisposed();
                if (!TryGetSolidColor(foreground, out var color) || string.IsNullOrEmpty(text))
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

            public void DrawGlyphRun(Brush foregroundBrush, GlyphRun glyphRun)
            {
                ThrowIfDisposed();
            }

            public void DrawDrawing(Drawing drawing)
            {
                ThrowIfDisposed();
            }

            public void PushClip(Geometry clipGeometry)
            {
                ThrowIfDisposed();
            }

            public void PushGuidelineSet(GuidelineSet guidelines)
            {
                ThrowIfDisposed();
            }

            public void PushOpacity(double opacity)
            {
                ThrowIfDisposed();
            }

            public void PushOpacityMask(Brush opacityMask)
            {
                ThrowIfDisposed();
            }

            public void PushTransform(Transform transform)
            {
                ThrowIfDisposed();
            }

            public void Pop()
            {
                ThrowIfDisposed();
            }

            public void Close() => Dispose();

            public void Dispose()
            {
            }

            internal BridgeLayerPacket BuildPacket()
            {
                return _commands.BuildPacket();
            }

            internal void DisposeWriter()
            {
                _commands.Dispose();
            }

            private void ThrowIfDisposed()
            {
                _root.ThrowIfDisposed();
            }
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
