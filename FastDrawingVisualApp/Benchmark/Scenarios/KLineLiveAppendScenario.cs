using FastDrawingVisual.Rendering;
using System;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisualApp.Benchmark.Scenarios
{
    internal sealed class KLineLiveAppendScenario : IBenchmarkScenario
    {
        public string Key => "kline-live-append";

        public string DisplayName => "K-Line Live Append";

        public string Description =>
            "Simulates a real-time feed with a right-anchored visible window, intrabar mutation, and periodic bar rollovers. Useful for pressure on latest-wins freshness rather than pure replay throughput.";

        public IBenchmarkScenarioSession CreateSession(BenchmarkScalePreset scale, int seed)
            => new Session(scale, seed);

        private sealed class Session : IBenchmarkScenarioSession
        {
            private readonly BenchmarkScalePreset _scale;
            private readonly SyntheticBar[] _bars;
            private readonly double[] _maFast;
            private readonly double[] _maMid;
            private readonly double[] _maSlow;
            private readonly double[] _oscillator;
            private readonly Random _random;

            private int _logicalCount;
            private double _workingOpen;
            private double _workingHigh;
            private double _workingLow;
            private double _workingClose;
            private double _workingVolume;
            private long _lastCommittedStep = -1;

            public Session(BenchmarkScalePreset scale, int seed)
            {
                _scale = scale;
                _random = new Random(seed + 101);

                var seedSeries = SyntheticMarketDataGenerator.Create(scale.TotalBars, seed);
                _bars = (SyntheticBar[])seedSeries.Bars.Clone();
                _maFast = new double[_bars.Length];
                _maMid = new double[_bars.Length];
                _maSlow = new double[_bars.Length];
                _oscillator = new double[_bars.Length];

                _logicalCount = Math.Min(Math.Max(scale.VisibleBars * 2, 256), _bars.Length);
                if (_logicalCount < 4)
                    _logicalCount = Math.Min(4, _bars.Length);

                var last = _bars[_logicalCount - 1];
                _workingOpen = last.Open;
                _workingHigh = last.High;
                _workingLow = last.Low;
                _workingClose = last.Close;
                _workingVolume = last.Volume;
                RebuildIndicators();
            }

            public string Summary =>
                $"{_logicalCount:n0} rolling bars, right-anchored window, intrabar mutation every frame";

            public void Dispose()
            {
            }

            public void RenderFrame(IDrawingContext ctx, BenchmarkFrameContext frame)
            {
                AdvanceFeed(frame);

                int width = ctx.Width;
                int height = ctx.Height;

                ctx.DrawRectangle(BenchmarkDrawingPalette.CanvasBackground, null!, new Rect(0, 0, width, height));

                Rect priceRect = new(0, 0, width, height * 0.7);
                Rect volumeRect = new(0, priceRect.Bottom, width, height * 0.15);
                Rect oscillatorRect = new(0, volumeRect.Bottom, width, Math.Max(0, height - volumeRect.Bottom));

                ctx.DrawRectangle(BenchmarkDrawingPalette.PanelBackground, null!, priceRect);
                ctx.DrawRectangle(BenchmarkDrawingPalette.PanelBackground, null!, volumeRect);
                ctx.DrawRectangle(BenchmarkDrawingPalette.PanelBackground, null!, oscillatorRect);

                DrawGrid(ctx, priceRect, 10, 6);
                DrawGrid(ctx, volumeRect, 10, 2);
                DrawGrid(ctx, oscillatorRect, 10, 2);

                int visible = Math.Min(_scale.VisibleBars, _logicalCount);
                int start = Math.Max(0, _logicalCount - visible);
                int endExclusive = _logicalCount;

                double minPrice = double.MaxValue;
                double maxPrice = double.MinValue;
                double maxVolume = 1;

                for (int i = start; i < endExclusive; i++)
                {
                    var bar = _bars[i];
                    minPrice = Math.Min(minPrice, bar.Low);
                    maxPrice = Math.Max(maxPrice, bar.High);
                    maxVolume = Math.Max(maxVolume, bar.Volume);
                }

                double pricePadding = Math.Max(1, (maxPrice - minPrice) * 0.1);
                minPrice -= pricePadding;
                maxPrice += pricePadding;

                DrawCandles(ctx, priceRect, start, endExclusive, minPrice, maxPrice);
                DrawSeriesLine(ctx, priceRect, start, endExclusive, _maFast, minPrice, maxPrice, BenchmarkDrawingPalette.AccentPen);
                if (_scale.IndicatorLineCount >= 2)
                    DrawSeriesLine(ctx, priceRect, start, endExclusive, _maMid, minPrice, maxPrice, BenchmarkDrawingPalette.SecondaryAccentPen);
                if (_scale.IndicatorLineCount >= 3)
                    DrawSeriesLine(ctx, priceRect, start, endExclusive, _maSlow, minPrice, maxPrice, BenchmarkDrawingPalette.TertiaryAccentPen);

                DrawVolume(ctx, volumeRect, start, endExclusive, maxVolume);
                DrawOscillator(ctx, oscillatorRect, start, endExclusive);
                DrawLatestMarker(ctx, priceRect, start, endExclusive, minPrice, maxPrice);
            }

            private void AdvanceFeed(BenchmarkFrameContext frame)
            {
                long logicalStep = frame.ExecutedFrameIndex;
                if (logicalStep == _lastCommittedStep)
                    return;

                _lastCommittedStep = logicalStep;
                bool commitNewBar = logicalStep % 18 == 0;
                double drift = (_random.NextDouble() - 0.5) * 1.4
                    + Math.Sin(frame.Phase * 2.1) * 0.35
                    + Math.Cos(frame.Phase * 0.65) * 0.18;

                _workingClose = Math.Max(4, _workingClose + drift);
                _workingHigh = Math.Max(_workingHigh, _workingClose + _random.NextDouble() * 0.8);
                _workingLow = Math.Min(_workingLow, _workingClose - _random.NextDouble() * 0.8);
                _workingVolume += 120 + Math.Abs(drift) * 1_300 + _random.NextDouble() * 180;

                if (commitNewBar)
                {
                    CommitWorkingBar();
                    _workingOpen = _workingClose;
                    _workingHigh = _workingClose;
                    _workingLow = _workingClose;
                    _workingVolume = 220 + _random.NextDouble() * 150;
                }
                else if (_logicalCount > 0)
                {
                    _bars[_logicalCount - 1] = new SyntheticBar(
                        _workingOpen,
                        _workingHigh,
                        _workingLow,
                        _workingClose,
                        _workingVolume);
                    RebuildIndicators();
                }
            }

            private void CommitWorkingBar()
            {
                var committed = new SyntheticBar(
                    _workingOpen,
                    _workingHigh,
                    _workingLow,
                    _workingClose,
                    _workingVolume);

                if (_logicalCount < _bars.Length)
                {
                    _bars[_logicalCount] = committed;
                    _logicalCount++;
                }
                else
                {
                    Array.Copy(_bars, 1, _bars, 0, _bars.Length - 1);
                    _bars[^1] = committed;
                }

                RebuildIndicators();
            }

            private void RebuildIndicators()
            {
                RebuildMovingAverage(_maFast, 8);
                RebuildMovingAverage(_maMid, 21);
                RebuildMovingAverage(_maSlow, 55);
                RebuildOscillator();
            }

            private void RebuildMovingAverage(double[] target, int period)
            {
                double sum = 0;
                for (int i = 0; i < _logicalCount; i++)
                {
                    sum += _bars[i].Close;
                    if (i >= period)
                        sum -= _bars[i - period].Close;

                    int divisor = Math.Min(i + 1, period);
                    target[i] = sum / divisor;
                }
            }

            private void RebuildOscillator()
            {
                for (int i = 0; i < _logicalCount; i++)
                {
                    double fast = i > 3 ? _bars[i].Close - _bars[i - 3].Close : 0;
                    double slow = i > 13 ? _bars[i].Close - _bars[i - 13].Close : fast;
                    double scale = Math.Max(1, Math.Abs(slow));
                    _oscillator[i] = Math.Max(-1.2, Math.Min(1.2, fast / scale));
                }
            }

            private void DrawGrid(IDrawingContext ctx, Rect area, int verticalLines, int horizontalLines)
            {
                for (int i = 0; i <= verticalLines; i++)
                {
                    double x = area.Left + area.Width * i / Math.Max(1, verticalLines);
                    ctx.DrawLine(BenchmarkDrawingPalette.GridPen, new Point(x, area.Top), new Point(x, area.Bottom));
                }

                for (int i = 0; i <= horizontalLines; i++)
                {
                    double y = area.Top + area.Height * i / Math.Max(1, horizontalLines);
                    ctx.DrawLine(BenchmarkDrawingPalette.GridPen, new Point(area.Left, y), new Point(area.Right, y));
                }
            }

            private void DrawCandles(IDrawingContext ctx, Rect area, int start, int endExclusive, double minPrice, double maxPrice)
            {
                double barWidth = area.Width / Math.Max(1, endExclusive - start);
                double bodyWidth = Math.Max(1, Math.Min(10, barWidth * 0.7));

                for (int i = start; i < endExclusive; i++)
                {
                    int visibleIndex = i - start;
                    double centerX = area.Left + visibleIndex * barWidth + barWidth * 0.5;
                    var bar = _bars[i];
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

            private void DrawSeriesLine(IDrawingContext ctx, Rect area, int start, int endExclusive, double[] values, double minPrice, double maxPrice, Pen pen)
            {
                double step = area.Width / Math.Max(1, endExclusive - start);
                double prevX = area.Left + step * 0.5;
                double prevY = MapPrice(area, values[start], minPrice, maxPrice);

                for (int i = start + 1; i < endExclusive; i++)
                {
                    double x = area.Left + (i - start) * step + step * 0.5;
                    double y = MapPrice(area, values[i], minPrice, maxPrice);
                    ctx.DrawLine(pen, new Point(prevX, prevY), new Point(x, y));
                    prevX = x;
                    prevY = y;
                }
            }

            private void DrawVolume(IDrawingContext ctx, Rect area, int start, int endExclusive, double maxVolume)
            {
                double barWidth = area.Width / Math.Max(1, endExclusive - start);
                for (int i = start; i < endExclusive; i++)
                {
                    int visibleIndex = i - start;
                    double x = area.Left + visibleIndex * barWidth;
                    double height = (_bars[i].Volume / maxVolume) * Math.Max(1, area.Height - 2);
                    double y = area.Bottom - height;
                    ctx.DrawRectangle(BenchmarkDrawingPalette.VolumeBrush, null!, new Rect(x, y, Math.Max(1, barWidth - 1), height));
                }
            }

            private void DrawOscillator(IDrawingContext ctx, Rect area, int start, int endExclusive)
            {
                double step = area.Width / Math.Max(1, endExclusive - start);
                double centerY = area.Top + area.Height * 0.5;
                ctx.DrawLine(BenchmarkDrawingPalette.MutedPen, new Point(area.Left, centerY), new Point(area.Right, centerY));

                double prevX = area.Left + step * 0.5;
                double prevY = MapOscillator(area, _oscillator[start]);

                for (int i = start + 1; i < endExclusive; i++)
                {
                    double x = area.Left + (i - start) * step + step * 0.5;
                    double y = MapOscillator(area, _oscillator[i]);
                    var pen = _oscillator[i] >= 0
                        ? BenchmarkDrawingPalette.SecondaryAccentPen
                        : BenchmarkDrawingPalette.TertiaryAccentPen;
                    ctx.DrawLine(pen, new Point(prevX, prevY), new Point(x, y));
                    prevX = x;
                    prevY = y;
                }
            }

            private void DrawLatestMarker(IDrawingContext ctx, Rect area, int start, int endExclusive, double minPrice, double maxPrice)
            {
                int lastIndex = endExclusive - 1;
                double step = area.Width / Math.Max(1, endExclusive - start);
                double lastX = area.Left + (lastIndex - start) * step + step * 0.5;
                double lastY = MapPrice(area, _bars[lastIndex].Close, minPrice, maxPrice);
                ctx.DrawEllipse(BenchmarkDrawingPalette.AccentBrush, BenchmarkDrawingPalette.NeutralPen, new Point(lastX, lastY), 4.5, 4.5);
                ctx.DrawLine(BenchmarkDrawingPalette.NeutralPen, new Point(lastX, area.Top), new Point(lastX, area.Bottom));
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
        }
    }
}
