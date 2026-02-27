using System;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisual.Rendering
{
    /// <summary>
    /// 线程安全的绘图上下文接口。
    /// 提供与 WPF DrawingContext 兼容的 API，但支持在后台线程中使用。
    /// </summary>
    /// <remarks>
    /// 所有传入的 Freezable 参数（Brush、Pen、Geometry 等）
    /// 如果在非创建线程使用，必须先调用 Freeze()。
    /// </remarks>
    public interface IDrawingContext : IDisposable
    {
        /// <summary>获取渲染宽度（像素）。</summary>
        int Width { get; }

        /// <summary>获取渲染高度（像素）。</summary>
        int Height { get; }

        #region 基础形状绘制

        void DrawEllipse(Brush brush, Pen pen, Point center, double radiusX, double radiusY);
        void DrawRectangle(Brush brush, Pen pen, Rect rectangle);
        void DrawRoundedRectangle(Brush brush, Pen pen, Rect rectangle, double radiusX, double radiusY);
        void DrawLine(Pen pen, Point point0, Point point1);
        void DrawGeometry(Brush brush, Pen pen, Geometry geometry);

        #endregion

        #region 图像与文本

        void DrawImage(ImageSource imageSource, Rect rectangle);

        /// <summary>绘制文本（简化版本，适合调试和标注）。</summary>
        void DrawText(string text, Point origin, Brush foreground,
                      string fontFamily = "Segoe UI", double fontSize = 12);

        void DrawGlyphRun(Brush foregroundBrush, GlyphRun glyphRun);
        void DrawDrawing(Drawing drawing);

        #endregion

        #region 状态管理

        void PushClip(Geometry clipGeometry);
        void PushGuidelineSet(GuidelineSet guidelines);
        void PushOpacity(double opacity);
        void PushOpacityMask(Brush opacityMask);
        void PushTransform(Transform transform);
        void Pop();

        #endregion

        /// <summary>关闭绘图上下文，提交本次绘制。</summary>
        void Close();
    }
}
