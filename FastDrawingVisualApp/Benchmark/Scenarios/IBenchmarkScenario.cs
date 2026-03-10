using FastDrawingVisual.Rendering;
using System;

namespace FastDrawingVisualApp.Benchmark.Scenarios
{
    internal interface IBenchmarkScenario
    {
        string Key { get; }

        string DisplayName { get; }

        string Description { get; }

        IBenchmarkScenarioSession CreateSession(BenchmarkScalePreset scale, int seed);
    }

    internal interface IBenchmarkScenarioSession : IDisposable
    {
        string Summary { get; }

        void RenderFrame(IDrawingContext ctx, BenchmarkFrameContext frame);
    }

    internal readonly struct BenchmarkRenderSurface
    {
        public BenchmarkRenderSurface(int width, int height)
        {
            Width = width;
            Height = height;
        }

        public int Width { get; }

        public int Height { get; }
    }

    internal interface IPreparedBenchmarkFrame
    {
    }

    internal interface IPreparedBenchmarkScenarioSession : IBenchmarkScenarioSession
    {
        IPreparedBenchmarkFrame PrepareFrame(BenchmarkRenderSurface surface, BenchmarkFrameContext frame);

        void RenderPreparedFrame(IDrawingContext ctx, IPreparedBenchmarkFrame preparedFrame);
    }
}
