using SkiaSharp;
using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisual.Rendering.Skia
{
    /// <summary>
    /// WPF 类型到 Skia 类型的转换器。
    /// 将 WPF 的 Brush、Pen、Geometry 等转换为 Skia 的 SKPaint、SKPath 等。
    /// </summary>
    internal static class WpfToSkiaConverter
    {
        /// <summary>
        /// 将 WPF Color 转换为 SKColor。
        /// </summary>
        public static SKColor ToSkiaColor(Color color)
        {
            return new SKColor(color.R, color.G, color.B, color.A);
        }

        /// <summary>
        /// 将 WPF Rect 转换为 SKRect。
        /// </summary>
        public static SKRect ToSkiaRect(Rect rect)
        {
            return new SKRect((float)rect.X, (float)rect.Y, (float)rect.Right, (float)rect.Bottom);
        }

        /// <summary>
        /// 将 WPF Point 转换为 SKPoint。
        /// </summary>
        public static SKPoint ToSkiaPoint(Point point)
        {
            return new SKPoint((float)point.X, (float)point.Y);
        }

        /// <summary>
        /// 验证 Freezable 对象是否可以跨线程使用。
        /// </summary>
        /// <param name="obj">要验证的对象。</param>
        /// <param name="paramName">参数名称。</param>
        /// <exception cref="InvalidOperationException">如果对象未冻结且不能跨线程使用。</exception>
        public static void ValidateThreadAffinity(Freezable? obj, string paramName)
        {
            if (obj == null)
                return;

            if (!obj.IsFrozen && !obj.Dispatcher.CheckAccess())
            {
                throw new InvalidOperationException(
                    $"跨线程使用的 {obj.GetType().Name} 必须先调用 Freeze()。" +
                    $"提示：创建对象后立即调用 .Freeze() 方法。");
            }
        }

        /// <summary>
        /// 从 WPF Brush 创建 SKPaint。
        /// </summary>
        /// <param name="brush">WPF 画刷。</param>
        /// <returns>SKPaint 实例，如果 brush 为 null 则返回 null。</returns>
        public static SKPaint? ToSkiaPaint(Brush? brush)
        {
            if (brush == null)
                return null;

            ValidateThreadAffinity(brush, nameof(brush));

            var paint = new SKPaint
            {
                IsAntialias = true,
                Style = SKPaintStyle.Fill
            };

            switch (brush)
            {
                case SolidColorBrush solidBrush:
                    paint.Color = ToSkiaColor(solidBrush.Color);
                    break;

                case LinearGradientBrush linearBrush:
                    // 简化处理：使用起始颜色
                    paint.Color = ToSkiaColor(linearBrush.GradientStops.Count > 0 
                        ? linearBrush.GradientStops[0].Color 
                        : Colors.Transparent);
                    // TODO: 完整实现渐变
                    break;

                case RadialGradientBrush radialBrush:
                    paint.Color = ToSkiaColor(radialBrush.GradientStops.Count > 0 
                        ? radialBrush.GradientStops[0].Color 
                        : Colors.Transparent);
                    break;

                default:
                    // 尝试获取颜色
                    paint.Color = SKColors.Transparent;
                    break;
            }

            return paint;
        }

        /// <summary>
        /// 从 WPF Pen 创建 SKPaint。
        /// </summary>
        /// <param name="pen">WPF 画笔。</param>
        /// <returns>SKPaint 实例，如果 pen 为 null 则返回 null。</returns>
        public static SKPaint? ToSkiaPaint(Pen? pen)
        {
            if (pen == null)
                return null;

            ValidateThreadAffinity(pen, nameof(pen));

            var paint = new SKPaint
            {
                IsAntialias = true,
                Style = SKPaintStyle.Stroke,
                StrokeWidth = (float)pen.Thickness
            };

            // 转换画刷
            if (pen.Brush != null)
            {
                var brushPaint = ToSkiaPaint(pen.Brush);
                if (brushPaint != null)
                {
                    paint.Color = brushPaint.Color;
                    brushPaint.Dispose();
                }
            }

            // 转换线帽
            paint.StrokeCap = pen.StartLineCap switch
            {
                PenLineCap.Flat => SKStrokeCap.Butt,
                PenLineCap.Round => SKStrokeCap.Round,
                PenLineCap.Square => SKStrokeCap.Square,
                PenLineCap.Triangle => SKStrokeCap.Butt, // Skia 没有三角形线帽
                _ => SKStrokeCap.Butt
            };

            // 转换线连接
            paint.StrokeJoin = pen.LineJoin switch
            {
                PenLineJoin.Miter => SKStrokeJoin.Miter,
                PenLineJoin.Bevel => SKStrokeJoin.Bevel,
                PenLineJoin.Round => SKStrokeJoin.Round,
                _ => SKStrokeJoin.Miter
            };

            // 转换虚线
            if (pen.DashStyle != null && pen.DashStyle != DashStyles.Solid)
            {
                var dashes = new List<float>();
                foreach (var dash in pen.DashStyle.Dashes)
                {
                    dashes.Add((float)dash);
                }
                paint.PathEffect = SKPathEffect.CreateDash(dashes.ToArray(), (float)pen.DashStyle.Offset);
            }

            return paint;
        }

        /// <summary>
        /// 将 WPF Geometry 转换为 SKPath。
        /// </summary>
        /// <param name="geometry">WPF 几何图形。</param>
        /// <returns>SKPath 实例。</returns>
        public static SKPath ToSkiaPath(Geometry geometry)
        {
            ValidateThreadAffinity(geometry, nameof(geometry));

            var path = new SKPath();
            var pathGeometry = PathGeometry.CreateFromGeometry(geometry);
            
            ConvertPathGeometry(pathGeometry, path);
            return path;
        }

        private static void ConvertPathGeometry(PathGeometry pathGeometry, SKPath path)
        {
            foreach (var figure in pathGeometry.Figures)
            {
                if (figure.Segments.Count == 0)
                    continue;

                path.MoveTo((float)figure.StartPoint.X, (float)figure.StartPoint.Y);

                foreach (var segment in figure.Segments)
                {
                    switch (segment)
                    {
                        case LineSegment lineSegment:
                            path.LineTo((float)lineSegment.Point.X, (float)lineSegment.Point.Y);
                            break;

                        case PolyLineSegment polyLineSegment:
                            foreach (var point in polyLineSegment.Points)
                            {
                                path.LineTo((float)point.X, (float)point.Y);
                            }
                            break;

                        case BezierSegment bezierSegment:
                            path.CubicTo(
                                (float)bezierSegment.Point1.X, (float)bezierSegment.Point1.Y,
                                (float)bezierSegment.Point2.X, (float)bezierSegment.Point2.Y,
                                (float)bezierSegment.Point3.X, (float)bezierSegment.Point3.Y);
                            break;

                        case PolyBezierSegment polyBezierSegment:
                            var points = polyBezierSegment.Points;
                            for (int i = 0; i < points.Count - 2; i += 3)
                            {
                                path.CubicTo(
                                    (float)points[i].X, (float)points[i].Y,
                                    (float)points[i + 1].X, (float)points[i + 1].Y,
                                    (float)points[i + 2].X, (float)points[i + 2].Y);
                            }
                            break;

                        case QuadraticBezierSegment quadraticSegment:
                            path.QuadTo(
                                (float)quadraticSegment.Point1.X, (float)quadraticSegment.Point1.Y,
                                (float)quadraticSegment.Point2.X, (float)quadraticSegment.Point2.Y);
                            break;

                        case PolyQuadraticBezierSegment polyQuadraticSegment:
                            var qPoints = polyQuadraticSegment.Points;
                            for (int i = 0; i < qPoints.Count - 1; i += 2)
                            {
                                path.QuadTo(
                                    (float)qPoints[i].X, (float)qPoints[i].Y,
                                    (float)qPoints[i + 1].X, (float)qPoints[i + 1].Y);
                            }
                            break;

                        case ArcSegment arcSegment:
                            // 简化处理：用线段连接
                            path.LineTo((float)arcSegment.Point.X, (float)arcSegment.Point.Y);
                            break;
                    }
                }

                if (figure.IsClosed)
                {
                    path.Close();
                }
            }
        }

        /// <summary>
        /// 将 WPF Transform 转换为 SKMatrix。
        /// </summary>
        /// <param name="transform">WPF 变换。</param>
        /// <returns>SKMatrix。</returns>
        public static SKMatrix ToSkiaMatrix(Transform transform)
        {
            if (transform == null)
                return SKMatrix.Identity;

            ValidateThreadAffinity(transform, nameof(transform));

            var matrix = transform.Value;
            return new SKMatrix(
                (float)matrix.M11, (float)matrix.M12, (float)matrix.OffsetX,
                (float)matrix.M21, (float)matrix.M22, (float)matrix.OffsetY,
                0, 0, 1);
        }

        /// <summary>
        /// 将 WPF BitmapImage 转换为 SKBitmap。
        /// </summary>
        /// <param name="imageSource">WPF 图像源。</param>
        /// <returns>SKBitmap 实例。</returns>
        public static SKBitmap? ToSkiaBitmap(ImageSource? imageSource)
        {
            if (imageSource == null)
                return null;

            ValidateThreadAffinity(imageSource, nameof(imageSource));

            // 尝试获取像素数据
            if (imageSource is System.Windows.Media.Imaging.BitmapSource bitmapSource)
            {
                var width = bitmapSource.PixelWidth;
                var height = bitmapSource.PixelHeight;
                var stride = width * 4;
                var pixels = new byte[height * stride];

                bitmapSource.CopyPixels(pixels, stride, 0);

                var skBitmap = new SKBitmap(width, height, SKColorType.Bgra8888, SKAlphaType.Premul);
                var ptr = skBitmap.GetPixels();
                
                unsafe
                {
                    System.Runtime.InteropServices.Marshal.Copy(pixels, 0, ptr, pixels.Length);
                }

                return skBitmap;
            }

            return null;
        }
    }
}
