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
}
