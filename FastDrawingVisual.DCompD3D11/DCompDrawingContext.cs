using FastDrawingVisual.CommandRuntime;
using FastDrawingVisual.Log;
using FastDrawingVisual.Rendering;
using System;
using System.Diagnostics;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisual.DCompD3D11
{
    internal sealed class DCompDrawingContext : IDrawingContext, ILayeredDrawingContextContainer
    {
        private const int MetricWindowSec = 1;
        private const string CommandEncodeMetricFormat = "name={name} periodSec={periodSec}s samples={count} avgMs={avg} minMs={min} maxMs={max}";
        private static readonly Logger s_logger = new("DCompDrawingContext");
        private static readonly int s_commandEncodeMetricId = s_logger.RegisterMetric(
            "dcomp.d3d11.cmd_encode_ms",
            MetricWindowSec,
            CommandEncodeMetricFormat,
            LogLevel.Info);

        private readonly Action<BridgeLayeredFramePacket> _onClose;
        private readonly DCompLayerDrawingContext?[] _layers = new DCompLayerDrawingContext[BridgeLayeredFramePacket.MaxLayerCount];
        private readonly object _layerSync = new();
        private readonly long _encodingStartedTimestamp;
        private bool _isDisposed;

        public DCompDrawingContext(int width, int height, Action<BridgeLayeredFramePacket> onClose)
        {
            Width = width;
            Height = height;
            _onClose = onClose ?? throw new ArgumentNullException(nameof(onClose));
            _layers[0] = new DCompLayerDrawingContext(this, 0);
            _encodingStartedTimestamp = Stopwatch.GetTimestamp();
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
            if (_isDisposed) return;

            var packet = BuildFramePacket();
            var encodingCompletedTimestamp = Stopwatch.GetTimestamp();
            var commandEncodeDurationMs = (encodingCompletedTimestamp - _encodingStartedTimestamp) * 1000d / Stopwatch.Frequency;
            _isDisposed = true;

            try
            {
                if (s_commandEncodeMetricId > 0)
                    s_logger.LogMetric(s_commandEncodeMetricId, commandEncodeDurationMs);

                if (packet.HasAnyCommands)
                    _onClose(packet);
            }
            finally
            {
                DisposeLayers();
            }
        }

        internal void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(DCompDrawingContext));
        }

        private DCompLayerDrawingContext GetOrCreateLayer(int layerIndex)
        {
            var existing = _layers[layerIndex];
            if (existing != null)
                return existing;

            lock (_layerSync)
            {
                existing = _layers[layerIndex];
                if (existing == null)
                {
                    existing = new DCompLayerDrawingContext(this, layerIndex);
                    _layers[layerIndex] = existing;
                }

                return existing;
            }
        }

        private BridgeLayeredFramePacket BuildFramePacket()
        {
            var packet = new BridgeLayeredFramePacket();

            for (int i = 0; i < _layers.Length; i++)
            {
                var layer = _layers[i];
                if (layer == null)
                    continue;

                packet.SetLayer(i, layer.BuildPacket());
            }

            return packet;
        }

        private void DisposeLayers()
        {
            for (int i = 0; i < _layers.Length; i++)
                _layers[i]?.DisposeWriter();
        }

        private static void ValidateLayerIndex(int layerIndex)
        {
            if ((uint)layerIndex >= BridgeLayeredFramePacket.MaxLayerCount)
                throw new ArgumentOutOfRangeException(nameof(layerIndex));
        }

        private sealed class DCompLayerDrawingContext : IDrawingContext
        {
            private readonly DCompDrawingContext _root;
            private readonly BridgeCommandWriter _commands = new();

            public DCompLayerDrawingContext(DCompDrawingContext root, int layerIndex)
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
                // Layer context lifetime is owned by the root context.
            }

            internal BridgeLayerPacket BuildPacket()
            {
                ThrowIfDisposed();
                return BridgeLayerPacket.FromCommandPacket(_commands.BuildPacket());
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
