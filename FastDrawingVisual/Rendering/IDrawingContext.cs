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
        /// <summary>
        /// 获取渲染宽度。
        /// </summary>
        int Width { get; }

        /// <summary>
        /// 获取渲染高度。
        /// </summary>
        int Height { get; }

        #region 基础形状绘制

        /// <summary>
        /// 绘制椭圆。
        /// </summary>
        void DrawEllipse(Brush brush, Pen pen, Point center, double radiusX, double radiusY);

        /// <summary>
        /// 绘制矩形。
        /// </summary>
        void DrawRectangle(Brush brush, Pen pen, Rect rectangle);

        /// <summary>
        /// 绘制圆角矩形。
        /// </summary>
        void DrawRoundedRectangle(Brush brush, Pen pen, Rect rectangle, double radiusX, double radiusY);

        /// <summary>
        /// 绘制直线。
        /// </summary>
        void DrawLine(Pen pen, Point point0, Point point1);

        /// <summary>
        /// 绘制几何图形。
        /// </summary>
        void DrawGeometry(Brush brush, Pen pen, Geometry geometry);

        #endregion

        #region 图像与文本

        /// <summary>
        /// 绘制图像。
        /// </summary>
        void DrawImage(ImageSource imageSource, Rect rectangle);

        /// <summary>
        /// 绘制文本（简化版本）。
        /// </summary>
        /// <param name="text">要绘制的文本。</param>
        /// <param name="origin">起始位置。</param>
        /// <param name="foreground">前景画刷。</param>
        /// <param name="fontFamily">字体族名称。</param>
        /// <param name="fontSize">字体大小。</param>
        void DrawText(string text, Point origin, Brush foreground, string fontFamily = "Segoe UI", double fontSize = 12);

        /// <summary>
        /// 绘制字形运行。
        /// </summary>
        void DrawGlyphRun(Brush foregroundBrush, GlyphRun glyphRun);

        /// <summary>
        /// 绘制 Drawing 对象。
        /// </summary>
        void DrawDrawing(Drawing drawing);

        #endregion

        #region 状态管理

        /// <summary>
        /// 推入裁剪区域。
        /// </summary>
        void PushClip(Geometry clipGeometry);

        /// <summary>
        /// 推入参考线集。
        /// </summary>
        void PushGuidelineSet(GuidelineSet guidelines);

        /// <summary>
        /// 推入不透明度。
        /// </summary>
        void PushOpacity(double opacity);

        /// <summary>
        /// 推入不透明度遮罩。
        /// </summary>
        void PushOpacityMask(Brush opacityMask);

        /// <summary>
        /// 推入变换。
        /// </summary>
        void PushTransform(Transform transform);

        /// <summary>
        /// 弹出最近的状态。
        /// </summary>
        void Pop();

        #endregion

        #region 生命周期

        /// <summary>
        /// 关闭绘图上下文。
        /// </summary>
        void Close();

        #endregion
    }
}
