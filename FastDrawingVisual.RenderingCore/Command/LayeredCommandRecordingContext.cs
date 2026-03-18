using FastDrawingVisual.CommandRuntime;
using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisual.Rendering
{
    public sealed class LayeredCommandRecordingContext : IDrawingContext, ILayeredDrawingContextContainer
    {
        private readonly Action<LayeredFramePacket> _onClose;
        private readonly LayerCommandWriter?[] _layers = new LayerCommandWriter[LayeredFramePacket.MaxLayerCount];
        private readonly object _sync = new();
        private bool _isDisposed;

        public LayeredCommandRecordingContext(int width, int height, Action<LayeredFramePacket> onClose)
        {
            Width = width;
            Height = height;
            _onClose = onClose ?? throw new ArgumentNullException(nameof(onClose));
            _layers[0] = new LayerCommandWriter(this, 0);
        }

        public int Width { get; }

        public int Height { get; }

        public int LayerCount => LayeredFramePacket.MaxLayerCount;

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

            var packet = new LayeredFramePacket();

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
            if ((uint)layerIndex >= LayeredFramePacket.MaxLayerCount)
                throw new ArgumentOutOfRangeException(nameof(layerIndex));
        }

        private sealed class LayerCommandWriter : IDrawingContext
        {
            private enum PushStateKind
            {
                NoOp,
                Transform,
            }

            private readonly LayeredCommandRecordingContext _root;
            private readonly CommandWriter _commands = new();
            private readonly Stack<PushStateKind> _pushStates = new();
            private readonly Stack<Matrix> _transformStack = new();
            private Matrix _currentTransform = Matrix.Identity;
            private const double TransformEpsilon = 0.0001d;

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
                if (!TryTransformEllipse(center, radiusX, radiusY, out var transformedCenter, out var transformedRadiusX, out var transformedRadiusY))
                    return;

                if (TryGetSolidColor(brush, out var fill))
                    _commands.WriteFillEllipse((float)transformedCenter.X, (float)transformedCenter.Y, (float)transformedRadiusX, (float)transformedRadiusY, ToProtocolColor(fill));

                if (TryGetSolidPen(pen, out var stroke, out var thickness))
                    _commands.WriteStrokeEllipse((float)transformedCenter.X, (float)transformedCenter.Y, (float)transformedRadiusX, (float)transformedRadiusY, TransformThickness(thickness), ToProtocolColor(stroke));
            }

            public void DrawRectangle(Brush brush, Pen pen, Rect rectangle)
            {
                ThrowIfDisposed();
                if (!TryTransformAxisAlignedRect(rectangle, out var transformedRectangle))
                    return;

                if (TryGetSolidColor(brush, out var fill))
                    _commands.WriteFillRect((float)transformedRectangle.X, (float)transformedRectangle.Y, (float)transformedRectangle.Width, (float)transformedRectangle.Height, ToProtocolColor(fill));

                if (TryGetSolidPen(pen, out var stroke, out var thickness))
                    _commands.WriteStrokeRect((float)transformedRectangle.X, (float)transformedRectangle.Y, (float)transformedRectangle.Width, (float)transformedRectangle.Height, TransformThickness(thickness), ToProtocolColor(stroke));
            }

            public void DrawRoundedRectangle(Brush brush, Pen pen, Rect rectangle, double radiusX, double radiusY)
            {
                ThrowIfDisposed();
                if (radiusX <= 0d || radiusY <= 0d)
                {
                    DrawRectangle(brush, pen, rectangle);
                    return;
                }

                if (!TryTransformRoundedRect(rectangle, radiusX, radiusY, out var transformedRectangle, out var transformedRadiusX, out var transformedRadiusY))
                    return;

                if (TryGetSolidColor(brush, out var fill))
                {
                    _commands.WriteFillRoundedRect(
                        (float)transformedRectangle.X,
                        (float)transformedRectangle.Y,
                        (float)transformedRectangle.Width,
                        (float)transformedRectangle.Height,
                        (float)transformedRadiusX,
                        (float)transformedRadiusY,
                        ToProtocolColor(fill));
                }

                if (TryGetSolidPen(pen, out var stroke, out var thickness))
                {
                    _commands.WriteStrokeRoundedRect(
                        (float)transformedRectangle.X,
                        (float)transformedRectangle.Y,
                        (float)transformedRectangle.Width,
                        (float)transformedRectangle.Height,
                        (float)transformedRadiusX,
                        (float)transformedRadiusY,
                        TransformThickness(thickness),
                        ToProtocolColor(stroke));
                }
            }

            public void DrawLine(Pen pen, Point point0, Point point1)
            {
                ThrowIfDisposed();
                var transformedStart = TransformPoint(point0);
                var transformedEnd = TransformPoint(point1);
                if (TryGetSolidPen(pen, out var color, out var thickness))
                    _commands.WriteLine((float)transformedStart.X, (float)transformedStart.Y, (float)transformedEnd.X, (float)transformedEnd.Y, TransformThickness(thickness), ToProtocolColor(color));
            }

            public void DrawGeometry(Brush brush, Pen pen, Geometry geometry)
            {
                ThrowIfDisposed();
                if (geometry == null)
                    return;

                switch (geometry)
                {
                    case RectangleGeometry rectangleGeometry:
                        DrawRoundedRectangle(brush, pen, rectangleGeometry.Rect, rectangleGeometry.RadiusX, rectangleGeometry.RadiusY);
                        break;
                    case EllipseGeometry ellipseGeometry:
                        DrawEllipse(brush, pen, ellipseGeometry.Center, ellipseGeometry.RadiusX, ellipseGeometry.RadiusY);
                        break;
                    case LineGeometry lineGeometry:
                        DrawLine(pen, lineGeometry.StartPoint, lineGeometry.EndPoint);
                        break;
                    case GeometryGroup geometryGroup:
                        foreach (var child in geometryGroup.Children)
                            DrawGeometry(brush, pen, child);
                        break;
                }
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

                origin = TransformPoint(origin);
                fontSize = Math.Max(1d, fontSize * GetApproximateFontScale());

                _commands.WriteDrawTextRun(
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
                if (drawing == null)
                    return;

                switch (drawing)
                {
                    case GeometryDrawing geometryDrawing:
                        DrawGeometry(geometryDrawing.Brush, geometryDrawing.Pen, geometryDrawing.Geometry);
                        break;
                    case ImageDrawing imageDrawing:
                        DrawImage(imageDrawing.ImageSource, imageDrawing.Rect);
                        break;
                    case GlyphRunDrawing glyphRunDrawing:
                        DrawGlyphRun(glyphRunDrawing.ForegroundBrush, glyphRunDrawing.GlyphRun);
                        break;
                    case DrawingGroup drawingGroup:
                        DrawDrawingGroup(drawingGroup);
                        break;
                }
            }

            public void PushClip(Geometry clipGeometry)
            {
                ThrowIfDisposed();
                _pushStates.Push(PushStateKind.NoOp);
            }

            public void PushGuidelineSet(GuidelineSet guidelines)
            {
                ThrowIfDisposed();
                _pushStates.Push(PushStateKind.NoOp);
            }

            public void PushOpacity(double opacity)
            {
                ThrowIfDisposed();
                _pushStates.Push(PushStateKind.NoOp);
            }

            public void PushOpacityMask(Brush opacityMask)
            {
                ThrowIfDisposed();
                _pushStates.Push(PushStateKind.NoOp);
            }

            public void PushTransform(Transform transform)
            {
                ThrowIfDisposed();
                _transformStack.Push(_currentTransform);
                _pushStates.Push(PushStateKind.Transform);
                if (transform == null)
                    return;

                var next = _currentTransform;
                next.Append(transform.Value);
                _currentTransform = next;
            }

            public void Pop()
            {
                ThrowIfDisposed();
                if (_pushStates.Count == 0)
                    return;

                var pushKind = _pushStates.Pop();
                if (pushKind == PushStateKind.Transform && _transformStack.Count > 0)
                    _currentTransform = _transformStack.Pop();
            }

            public void Close() => Dispose();

            public void Dispose()
            {
            }

            internal LayerPacket BuildPacket()
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

            private void DrawDrawingGroup(DrawingGroup drawingGroup)
            {
                var pushedTransform = false;
                if (drawingGroup.Transform != null && !drawingGroup.Transform.Value.IsIdentity)
                {
                    PushTransform(drawingGroup.Transform);
                    pushedTransform = true;
                }

                try
                {
                    foreach (var child in drawingGroup.Children)
                        DrawDrawing(child);
                }
                finally
                {
                    if (pushedTransform)
                        Pop();
                }
            }

            private Point TransformPoint(Point point)
            {
                return _currentTransform.IsIdentity ? point : _currentTransform.Transform(point);
            }

            private bool TryTransformAxisAlignedRect(Rect rectangle, out Rect transformedRectangle)
            {
                transformedRectangle = rectangle;
                if (_currentTransform.IsIdentity)
                    return true;

                if (!IsAxisAlignedTransform(_currentTransform))
                    return false;

                var p0 = TransformPoint(rectangle.TopLeft);
                var p1 = TransformPoint(rectangle.TopRight);
                var p2 = TransformPoint(rectangle.BottomLeft);
                var p3 = TransformPoint(rectangle.BottomRight);

                var minX = Math.Min(Math.Min(p0.X, p1.X), Math.Min(p2.X, p3.X));
                var minY = Math.Min(Math.Min(p0.Y, p1.Y), Math.Min(p2.Y, p3.Y));
                var maxX = Math.Max(Math.Max(p0.X, p1.X), Math.Max(p2.X, p3.X));
                var maxY = Math.Max(Math.Max(p0.Y, p1.Y), Math.Max(p2.Y, p3.Y));
                transformedRectangle = new Rect(new Point(minX, minY), new Point(maxX, maxY));
                return true;
            }

            private bool TryTransformEllipse(Point center, double radiusX, double radiusY, out Point transformedCenter, out double transformedRadiusX, out double transformedRadiusY)
            {
                transformedCenter = center;
                transformedRadiusX = radiusX;
                transformedRadiusY = radiusY;

                if (_currentTransform.IsIdentity)
                    return true;

                if (!IsAxisAlignedTransform(_currentTransform))
                    return false;

                transformedCenter = TransformPoint(center);
                transformedRadiusX = Math.Abs(radiusX * _currentTransform.M11);
                transformedRadiusY = Math.Abs(radiusY * _currentTransform.M22);
                return transformedRadiusX > 0d && transformedRadiusY > 0d;
            }

            private bool TryTransformRoundedRect(Rect rectangle, double radiusX, double radiusY, out Rect transformedRectangle, out double transformedRadiusX, out double transformedRadiusY)
            {
                transformedRectangle = rectangle;
                transformedRadiusX = radiusX;
                transformedRadiusY = radiusY;

                if (!TryTransformAxisAlignedRect(rectangle, out transformedRectangle))
                    return false;

                if (_currentTransform.IsIdentity)
                    return transformedRadiusX > 0d && transformedRadiusY > 0d;

                transformedRadiusX = Math.Abs(radiusX * _currentTransform.M11);
                transformedRadiusY = Math.Abs(radiusY * _currentTransform.M22);
                return transformedRadiusX > 0d && transformedRadiusY > 0d;
            }

            private float TransformThickness(float thickness)
            {
                var scale = GetApproximateFontScale();
                return (float)Math.Max(0.1d, thickness * scale);
            }

            private double GetApproximateFontScale()
            {
                if (_currentTransform.IsIdentity)
                    return 1d;

                var scaleX = Math.Sqrt((_currentTransform.M11 * _currentTransform.M11) + (_currentTransform.M21 * _currentTransform.M21));
                var scaleY = Math.Sqrt((_currentTransform.M12 * _currentTransform.M12) + (_currentTransform.M22 * _currentTransform.M22));
                var scale = Math.Max(scaleX, scaleY);
                return scale > TransformEpsilon ? scale : 1d;
            }

            private static bool IsAxisAlignedTransform(Matrix matrix)
            {
                return Math.Abs(matrix.M12) < TransformEpsilon && Math.Abs(matrix.M21) < TransformEpsilon;
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

        private static CommandColorArgb8 ToProtocolColor(Color color)
        {
            return new CommandColorArgb8(color.A, color.R, color.G, color.B);
        }
    }
}
