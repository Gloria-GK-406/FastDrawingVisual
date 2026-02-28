using FastDrawingVisual.Rendering;
using System;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisual.Rendering.DComp
{
    /// <summary>
    /// 框架阶段占位绘图上下文。后续由具体后端替换为真实 GPU/CPU 上下文。
    /// </summary>
    internal sealed class NullDrawingContext : IDrawingContext
    {
        public NullDrawingContext(int width, int height)
        {
            Width = width;
            Height = height;
        }

        public int Width { get; }

        public int Height { get; }

        public void Close()
        {
        }

        public void Dispose()
        {
        }

        public void DrawDrawing(Drawing drawing)
        {
        }

        public void DrawEllipse(Brush brush, Pen pen, Point center, double radiusX, double radiusY)
        {
        }

        public void DrawGeometry(Brush brush, Pen pen, Geometry geometry)
        {
        }

        public void DrawGlyphRun(Brush foregroundBrush, GlyphRun glyphRun)
        {
        }

        public void DrawImage(ImageSource imageSource, Rect rectangle)
        {
        }

        public void DrawLine(Pen pen, Point point0, Point point1)
        {
        }

        public void DrawRectangle(Brush brush, Pen pen, Rect rectangle)
        {
        }

        public void DrawRoundedRectangle(Brush brush, Pen pen, Rect rectangle, double radiusX, double radiusY)
        {
        }

        public void DrawText(string text, Point origin, Brush foreground, string fontFamily = "Segoe UI", double fontSize = 12)
        {
        }

        public void Pop()
        {
        }

        public void PushClip(Geometry clipGeometry)
        {
        }

        public void PushGuidelineSet(GuidelineSet guidelines)
        {
        }

        public void PushOpacity(double opacity)
        {
        }

        public void PushOpacityMask(Brush opacityMask)
        {
        }

        public void PushTransform(Transform transform)
        {
        }
    }
}
