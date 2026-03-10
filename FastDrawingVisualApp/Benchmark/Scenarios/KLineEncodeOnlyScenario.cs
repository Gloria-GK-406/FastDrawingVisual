using FastDrawingVisual.Rendering;
using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisualApp.Benchmark.Scenarios
{
    internal sealed class KLineEncodeOnlyScenario : IBenchmarkScenario
    {
        public string Key => "kline-encode-only";

        public string DisplayName => "K-Line Encode Only";

        public string Description =>
            "Precomputes visible K-line primitives on the benchmark submit thread, then measures draw-thread command fill and native bridge transport without per-frame business data maintenance.";

        public IBenchmarkScenarioSession CreateSession(BenchmarkScalePreset scale, int seed)
            => new Session(scale, seed);

        private sealed class Session : IPreparedBenchmarkScenarioSession
        {
            private readonly BenchmarkScalePreset _scale;
            private readonly SyntheticMarketSeries _series;

            public Session(BenchmarkScalePreset scale, int seed)
            {
                _scale = scale;
                _series = SyntheticMarketDataGenerator.Create(scale.TotalBars, seed);
            }

            public string Summary =>
                $"{_scale.TotalBars:n0} bars, {_scale.VisibleBars:n0} visible, {_scale.IndicatorLineCount} overlays, {_scale.IndicatorPanelCount} panels, encode-only replay";

            public void Dispose()
            {
            }

            public void RenderFrame(IDrawingContext ctx, BenchmarkFrameContext frame)
            {
                var preparedFrame = PrepareFrame(new BenchmarkRenderSurface(ctx.Width, ctx.Height), frame);
                RenderPreparedFrame(ctx, preparedFrame);
            }

            public IPreparedBenchmarkFrame PrepareFrame(BenchmarkRenderSurface surface, BenchmarkFrameContext frame)
            {
                int width = surface.Width;
                int height = surface.Height;

                var rectangles = new List<RectCommand>(_scale.VisibleBars * 2 + 8);
                var lines = new List<LineCommand>((_scale.VisibleBars * Math.Max(1, _scale.IndicatorLineCount + 3)) + 64);

                rectangles.Add(new RectCommand(BenchmarkDrawingPalette.CanvasBackground, null, new Rect(0, 0, width, height)));

                Rect priceRect = new(0, 0, width, height * 0.68);
                Rect volumeRect = new(0, priceRect.Bottom, width, height * 0.18);
                Rect oscillatorRect = _scale.IndicatorPanelCount >= 2
                    ? new Rect(0, volumeRect.Bottom, width, Math.Max(0, height - volumeRect.Bottom))
                    : Rect.Empty;

                rectangles.Add(new RectCommand(BenchmarkDrawingPalette.PanelBackground, null, priceRect));
                rectangles.Add(new RectCommand(BenchmarkDrawingPalette.PanelBackground, null, volumeRect));
                if (!oscillatorRect.IsEmpty)
                    rectangles.Add(new RectCommand(BenchmarkDrawingPalette.PanelBackground, null, oscillatorRect));

                AppendGrid(lines, priceRect, 8, 6);
                AppendGrid(lines, volumeRect, 8, 3);
                if (!oscillatorRect.IsEmpty)
                    AppendGrid(lines, oscillatorRect, 8, 3);

                int visible = Math.Min(_scale.VisibleBars, _series.Bars.Length - 2);
                int maxStart = Math.Max(1, _series.Bars.Length - visible - 1);
                int start = (int)(frame.Elapsed.TotalSeconds * Math.Max(24, visible * 0.22)) % maxStart;
                int endExclusive = start + visible;

                double minPrice = double.MaxValue;
                double maxPrice = double.MinValue;
                double maxVolume = 0;

                for (int i = start; i < endExclusive; i++)
                {
                    var bar = _series.Bars[i];
                    minPrice = Math.Min(minPrice, bar.Low);
                    maxPrice = Math.Max(maxPrice, bar.High);
                    maxVolume = Math.Max(maxVolume, bar.Volume);
                }

                double pricePadding = Math.Max(1, (maxPrice - minPrice) * 0.08);
                minPrice -= pricePadding;
                maxPrice += pricePadding;
                maxVolume = Math.Max(maxVolume, 1);

                AppendCandles(rectangles, lines, priceRect, start, endExclusive, minPrice, maxPrice);
                AppendMovingAverages(lines, priceRect, start, endExclusive, minPrice, maxPrice);
                AppendVolume(rectangles, volumeRect, start, endExclusive, maxVolume);
                if (!oscillatorRect.IsEmpty)
                    AppendOscillator(lines, oscillatorRect, start, endExclusive);

                return new PreparedFrame(rectangles.ToArray(), lines.ToArray());
            }

            public void RenderPreparedFrame(IDrawingContext ctx, IPreparedBenchmarkFrame preparedFrame)
            {
                var frame = (PreparedFrame)preparedFrame;

                foreach (var rect in frame.Rectangles)
                    ctx.DrawRectangle(rect.Brush, rect.Pen!, rect.Rectangle);

                foreach (var line in frame.Lines)
                    ctx.DrawLine(line.Pen, line.Start, line.End);
            }

            private static void AppendGrid(List<LineCommand> lines, Rect area, int verticalLines, int horizontalLines)
            {
                if (area.Width <= 0 || area.Height <= 0)
                    return;

                for (int i = 0; i <= verticalLines; i++)
                {
                    double x = area.Left + area.Width * i / Math.Max(1, verticalLines);
                    lines.Add(new LineCommand(BenchmarkDrawingPalette.GridPen, new Point(x, area.Top), new Point(x, area.Bottom)));
                }

                for (int i = 0; i <= horizontalLines; i++)
                {
                    double y = area.Top + area.Height * i / Math.Max(1, horizontalLines);
                    lines.Add(new LineCommand(BenchmarkDrawingPalette.GridPen, new Point(area.Left, y), new Point(area.Right, y)));
                }
            }

            private void AppendCandles(List<RectCommand> rectangles, List<LineCommand> lines, Rect area, int start, int endExclusive, double minPrice, double maxPrice)
            {
                double barWidth = area.Width / Math.Max(1, endExclusive - start);
                double bodyWidth = Math.Max(1, Math.Min(12, barWidth * 0.72));

                for (int i = start; i < endExclusive; i++)
                {
                    int visibleIndex = i - start;
                    double centerX = area.Left + visibleIndex * barWidth + barWidth * 0.5;
                    var bar = _series.Bars[i];
                    bool isBull = bar.Close >= bar.Open;
                    var pen = isBull ? BenchmarkDrawingPalette.BullPen : BenchmarkDrawingPalette.BearPen;
                    var brush = isBull ? BenchmarkDrawingPalette.BullBrush : BenchmarkDrawingPalette.BearBrush;

                    double openY = MapPrice(area, bar.Open, minPrice, maxPrice);
                    double closeY = MapPrice(area, bar.Close, minPrice, maxPrice);
                    double highY = MapPrice(area, bar.High, minPrice, maxPrice);
                    double lowY = MapPrice(area, bar.Low, minPrice, maxPrice);
                    double bodyTop = Math.Min(openY, closeY);
                    double bodyHeight = Math.Max(1, Math.Abs(closeY - openY));

                    lines.Add(new LineCommand(pen, new Point(centerX, highY), new Point(centerX, lowY)));
                    rectangles.Add(new RectCommand(brush, pen, new Rect(centerX - bodyWidth * 0.5, bodyTop, bodyWidth, bodyHeight)));
                }
            }

            private void AppendMovingAverages(List<LineCommand> lines, Rect area, int start, int endExclusive, double minPrice, double maxPrice)
            {
                if (_scale.IndicatorLineCount <= 0)
                    return;

                AppendSeriesLine(lines, area, start, endExclusive, _series.MaFast, minPrice, maxPrice, BenchmarkDrawingPalette.AccentPen);
                if (_scale.IndicatorLineCount >= 2)
                    AppendSeriesLine(lines, area, start, endExclusive, _series.MaMid, minPrice, maxPrice, BenchmarkDrawingPalette.SecondaryAccentPen);
                if (_scale.IndicatorLineCount >= 3)
                    AppendSeriesLine(lines, area, start, endExclusive, _series.MaSlow, minPrice, maxPrice, BenchmarkDrawingPalette.TertiaryAccentPen);
                if (_scale.IndicatorLineCount >= 4)
                    AppendCompositeLine(lines, area, start, endExclusive, minPrice, maxPrice);
            }

            private static void AppendSeriesLine(List<LineCommand> lines, Rect area, int start, int endExclusive, double[] values, double minPrice, double maxPrice, Pen pen)
            {
                double step = area.Width / Math.Max(1, endExclusive - start);
                double prevX = area.Left + step * 0.5;
                double prevY = MapPrice(area, values[start], minPrice, maxPrice);

                for (int i = start + 1; i < endExclusive; i++)
                {
                    double x = area.Left + (i - start) * step + step * 0.5;
                    double y = MapPrice(area, values[i], minPrice, maxPrice);
                    lines.Add(new LineCommand(pen, new Point(prevX, prevY), new Point(x, y)));
                    prevX = x;
                    prevY = y;
                }
            }

            private void AppendCompositeLine(List<LineCommand> lines, Rect area, int start, int endExclusive, double minPrice, double maxPrice)
            {
                double step = area.Width / Math.Max(1, endExclusive - start);
                double prevX = area.Left + step * 0.5;
                double prevY = MapPrice(area, CompositeAverage(start), minPrice, maxPrice);

                for (int i = start + 1; i < endExclusive; i++)
                {
                    double x = area.Left + (i - start) * step + step * 0.5;
                    double y = MapPrice(area, CompositeAverage(i), minPrice, maxPrice);
                    lines.Add(new LineCommand(BenchmarkDrawingPalette.NeutralPen, new Point(prevX, prevY), new Point(x, y)));
                    prevX = x;
                    prevY = y;
                }
            }

            private void AppendVolume(List<RectCommand> rectangles, Rect area, int start, int endExclusive, double maxVolume)
            {
                double barWidth = area.Width / Math.Max(1, endExclusive - start);

                for (int i = start; i < endExclusive; i++)
                {
                    int visibleIndex = i - start;
                    double x = area.Left + visibleIndex * barWidth;
                    double height = (_series.Bars[i].Volume / maxVolume) * Math.Max(1, area.Height - 2);
                    double y = area.Bottom - height;
                    rectangles.Add(new RectCommand(BenchmarkDrawingPalette.VolumeBrush, null, new Rect(x, y, Math.Max(1, barWidth - 1), height)));
                }
            }

            private void AppendOscillator(List<LineCommand> lines, Rect area, int start, int endExclusive)
            {
                double step = area.Width / Math.Max(1, endExclusive - start);
                double centerY = area.Top + area.Height * 0.5;
                lines.Add(new LineCommand(BenchmarkDrawingPalette.MutedPen, new Point(area.Left, centerY), new Point(area.Right, centerY)));

                double prevX = area.Left + step * 0.5;
                double prevY = MapOscillator(area, _series.Oscillator[start]);

                for (int i = start + 1; i < endExclusive; i++)
                {
                    double x = area.Left + (i - start) * step + step * 0.5;
                    double y = MapOscillator(area, _series.Oscillator[i]);
                    var pen = _series.Oscillator[i] >= 0
                        ? BenchmarkDrawingPalette.SecondaryAccentPen
                        : BenchmarkDrawingPalette.TertiaryAccentPen;
                    lines.Add(new LineCommand(pen, new Point(prevX, prevY), new Point(x, y)));
                    prevX = x;
                    prevY = y;
                }
            }

            private static double MapPrice(Rect area, double value, double minPrice, double maxPrice)
            {
                if (maxPrice <= minPrice)
                    return area.Bottom;

                double ratio = (value - minPrice) / (maxPrice - minPrice);
                return area.Bottom - ratio * area.Height;
            }

            private static double MapOscillator(Rect area, double value)
            {
                double clamped = Math.Max(-1.2, Math.Min(1.2, value));
                double ratio = (clamped + 1.2) / 2.4;
                return area.Bottom - ratio * area.Height;
            }

            private double CompositeAverage(int index)
                => (_series.MaFast[index] * 0.45) + (_series.MaMid[index] * 0.35) + (_series.MaSlow[index] * 0.20);

            private sealed class PreparedFrame : IPreparedBenchmarkFrame
            {
                public PreparedFrame(RectCommand[] rectangles, LineCommand[] lines)
                {
                    Rectangles = rectangles;
                    Lines = lines;
                }

                public RectCommand[] Rectangles { get; }

                public LineCommand[] Lines { get; }
            }

            private readonly struct RectCommand
            {
                public RectCommand(Brush brush, Pen? pen, Rect rectangle)
                {
                    Brush = brush;
                    Pen = pen;
                    Rectangle = rectangle;
                }

                public Brush Brush { get; }

                public Pen? Pen { get; }

                public Rect Rectangle { get; }
            }

            private readonly struct LineCommand
            {
                public LineCommand(Pen pen, Point start, Point end)
                {
                    Pen = pen;
                    Start = start;
                    End = end;
                }

                public Pen Pen { get; }

                public Point Start { get; }

                public Point End { get; }
            }
        }
    }
}
