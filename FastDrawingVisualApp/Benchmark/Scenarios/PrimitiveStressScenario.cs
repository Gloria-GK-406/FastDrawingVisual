using FastDrawingVisual.Rendering;
using System;
using System.Windows;

namespace FastDrawingVisualApp.Benchmark.Scenarios
{
    internal sealed class PrimitiveStressScenario : IBenchmarkScenario
    {
        public string Key => "primitive-core";

        public string DisplayName => "Primitive Core";

        public string Description =>
            "Baseline primitive pressure: dense grid, wave bundles, pulse markers, and bar fields. Keeps the load within shared line and rectangle APIs.";

        public IBenchmarkScenarioSession CreateSession(BenchmarkScalePreset scale, int seed)
            => new Session(scale);

        private sealed class Session : IBenchmarkScenarioSession
        {
            private readonly BenchmarkScalePreset _scale;

            public Session(BenchmarkScalePreset scale)
            {
                _scale = scale;
            }

            public string Summary =>
                $"{_scale.VisibleBars} columns, density x{_scale.PrimitiveDensity}, {_scale.IndicatorLineCount + 2} wave bundles";

            public void Dispose()
            {
            }

            public void RenderFrame(IDrawingContext ctx, BenchmarkFrameContext frame)
            {
                int width = ctx.Width;
                int height = ctx.Height;
                double phase = frame.Phase;

                ctx.DrawRectangle(BenchmarkDrawingPalette.CanvasBackground, null!, new Rect(0, 0, width, height));
                DrawGrid(ctx, width, height);
                DrawBarField(ctx, width, height, phase);
                DrawWaveBundles(ctx, width, height, phase);
                DrawPulseMarkers(ctx, width, height, phase);
            }

            private void DrawGrid(IDrawingContext ctx, int width, int height)
            {
                int stepX = Math.Max(18, width / (12 + _scale.PrimitiveDensity * 10));
                int stepY = Math.Max(18, height / (8 + _scale.PrimitiveDensity * 6));

                for (int x = 0; x <= width; x += stepX)
                    ctx.DrawLine(BenchmarkDrawingPalette.GridPen, new Point(x, 0), new Point(x, height));

                for (int y = 0; y <= height; y += stepY)
                    ctx.DrawLine(BenchmarkDrawingPalette.GridPen, new Point(0, y), new Point(width, y));
            }

            private void DrawBarField(IDrawingContext ctx, int width, int height, double phase)
            {
                int columnCount = Math.Min(_scale.VisibleBars, Math.Max(80, width / 2));
                double columnWidth = width / (double)columnCount;
                double baseLine = height * 0.82;
                double bandHeight = height * 0.28;

                for (int i = 0; i < columnCount; i++)
                {
                    double t = i / (double)columnCount;
                    double sin = Math.Sin(t * Math.PI * (8 + _scale.PrimitiveDensity * 3) + phase * 1.8);
                    double cos = Math.Cos(t * Math.PI * (5 + _scale.PrimitiveDensity * 2) - phase * 0.7);
                    double magnitude = Math.Abs(sin * 0.7 + cos * 0.3);
                    double barHeight = 4 + magnitude * bandHeight;
                    double x = i * columnWidth;
                    double y = baseLine - barHeight;
                    var brush = sin >= 0 ? BenchmarkDrawingPalette.VolumeBrush : BenchmarkDrawingPalette.SecondaryAccentBrush;
                    ctx.DrawRectangle(brush, null!, new Rect(x, y, Math.Max(1, columnWidth - 1), barHeight));
                }
            }

            private void DrawWaveBundles(IDrawingContext ctx, int width, int height, double phase)
            {
                int bundleCount = _scale.IndicatorLineCount + 2;
                int segments = Math.Max(180, _scale.VisibleBars / 2);
                var pens = new[]
                {
                    BenchmarkDrawingPalette.AccentPen,
                    BenchmarkDrawingPalette.SecondaryAccentPen,
                    BenchmarkDrawingPalette.TertiaryAccentPen,
                    BenchmarkDrawingPalette.NeutralPen
                };

                for (int bundle = 0; bundle < bundleCount; bundle++)
                {
                    double centerY = height * (0.2 + bundle * 0.12);
                    double amplitude = height * (0.05 + bundle * 0.01);
                    double frequency = 3 + bundle * 1.2 + _scale.PrimitiveDensity * 0.8;
                    double prevX = 0;
                    double prevY = centerY + Math.Sin(phase * (1.1 + bundle * 0.08)) * amplitude;
                    var pen = pens[bundle % pens.Length];

                    for (int i = 1; i <= segments; i++)
                    {
                        double t = i / (double)segments;
                        double x = t * width;
                        double y = centerY
                            + Math.Sin(t * Math.PI * frequency + phase * (1.0 + bundle * 0.15)) * amplitude
                            + Math.Cos(t * Math.PI * (frequency * 0.35) - phase * 0.5) * amplitude * 0.35;

                        ctx.DrawLine(pen, new Point(prevX, prevY), new Point(x, y));
                        prevX = x;
                        prevY = y;
                    }
                }
            }

            private void DrawPulseMarkers(IDrawingContext ctx, int width, int height, double phase)
            {
                int markerCount = 12 * _scale.PrimitiveDensity;
                double radius = 4 + Math.Abs(Math.Sin(phase * 1.7)) * 8;

                for (int i = 0; i < markerCount; i++)
                {
                    double t = i / (double)Math.Max(1, markerCount - 1);
                    double x = 32 + t * Math.Max(0, width - 64);
                    double y = height * 0.12 + Math.Sin(phase * 0.9 + i * 0.45) * height * 0.05;
                    double markerRadius = radius * (0.45 + (i % 4) * 0.18);
                    var brush = (i % 3) switch
                    {
                        0 => BenchmarkDrawingPalette.AccentBrush,
                        1 => BenchmarkDrawingPalette.SecondaryAccentBrush,
                        _ => BenchmarkDrawingPalette.TertiaryAccentBrush,
                    };

                    ctx.DrawEllipse(brush, BenchmarkDrawingPalette.MutedPen, new Point(x, y), markerRadius, markerRadius);
                }
            }
        }
    }
}
