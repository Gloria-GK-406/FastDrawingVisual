using FastDrawingVisual.Rendering;
using System;
using System.Windows;

namespace FastDrawingVisualApp.Benchmark.Scenarios
{
    internal sealed class KLineGrid16Scenario : IBenchmarkScenario
    {
        public const string ScenarioId = "kline-grid-16";

        public string Key => ScenarioId;

        public string DisplayName => "K-Line Grid x16";

        public string Description =>
            "Sixteen FastDrawingVisual tiles render candle-only market playback in parallel. Per-tile data windows are intentionally lighter than the single-canvas stress path.";

        public IBenchmarkScenarioSession CreateSession(BenchmarkScalePreset scale, int seed)
            => new Session(scale, seed);

        private sealed class Session : IBenchmarkScenarioSession
        {
            private readonly SyntheticMarketSeries _series;
            private readonly int _scrollUnitsPerSecond;
            private readonly int _totalBars;
            private readonly int _visibleBars;

            public Session(BenchmarkScalePreset scale, int seed)
            {
                _visibleBars = Math.Clamp(scale.VisibleBars / 4, 80, 180);
                _totalBars = Math.Max(_visibleBars * 10, Math.Max(800, scale.TotalBars / 5));
                _scrollUnitsPerSecond = 12 + Math.Abs(seed % 11);
                _series = SyntheticMarketDataGenerator.Create(_totalBars, seed);
            }

            public string Summary => $"{_totalBars:n0} bars/tile, {_visibleBars:n0} visible, candles only";

            public void Dispose()
            {
            }

            public void RenderFrame(IDrawingContext ctx, BenchmarkFrameContext frame)
            {
                int width = ctx.Width;
                int height = ctx.Height;
                if (width <= 0 || height <= 0)
                    return;

                Rect chartRect = new(0, 0, width, height);
                ctx.DrawRectangle(BenchmarkDrawingPalette.CanvasBackground, null!, chartRect);

                int visible = Math.Min(_visibleBars, _series.Bars.Length - 2);
                int maxStart = Math.Max(1, _series.Bars.Length - visible - 1);
                int start = (int)(frame.Elapsed.TotalSeconds * _scrollUnitsPerSecond) % maxStart;
                int endExclusive = start + visible;

                double minPrice = double.MaxValue;
                double maxPrice = double.MinValue;

                for (int i = start; i < endExclusive; i++)
                {
                    var bar = _series.Bars[i];
                    minPrice = Math.Min(minPrice, bar.Low);
                    maxPrice = Math.Max(maxPrice, bar.High);
                }

                double pricePadding = Math.Max(1, (maxPrice - minPrice) * 0.12);
                DrawCandles(ctx, chartRect, start, endExclusive, minPrice - pricePadding, maxPrice + pricePadding);
            }

            private void DrawCandles(IDrawingContext ctx, Rect area, int start, int endExclusive, double minPrice, double maxPrice)
            {
                double barWidth = area.Width / Math.Max(1, endExclusive - start);
                double bodyWidth = Math.Max(1, Math.Min(10, barWidth * 0.7));

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

                    ctx.DrawLine(pen, new Point(centerX, highY), new Point(centerX, lowY));
                    ctx.DrawRectangle(brush, pen, new Rect(centerX - bodyWidth * 0.5, bodyTop, bodyWidth, bodyHeight));
                }
            }

            private static double MapPrice(Rect area, double value, double minPrice, double maxPrice)
            {
                if (maxPrice <= minPrice)
                    return area.Bottom;

                double ratio = (value - minPrice) / (maxPrice - minPrice);
                return area.Bottom - ratio * area.Height;
            }
        }
    }
}
