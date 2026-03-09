using FastDrawingVisual.Controls;
using FastDrawingVisualApp.Benchmark.Scenarios;

namespace FastDrawingVisualApp.Benchmark
{
    internal sealed class BenchmarkConfig
    {
        public BenchmarkConfig(
            RendererPreference renderer,
            IBenchmarkScenario scenario,
            BenchmarkScalePreset scale,
            SubmitPacingPreset pacing,
            RunDurationPreset duration,
            int seed)
        {
            Renderer = renderer;
            Scenario = scenario;
            Scale = scale;
            Pacing = pacing;
            Duration = duration;
            Seed = seed;
        }

        public RendererPreference Renderer { get; }

        public IBenchmarkScenario Scenario { get; }

        public BenchmarkScalePreset Scale { get; }

        public SubmitPacingPreset Pacing { get; }

        public RunDurationPreset Duration { get; }

        public int Seed { get; }

        public string Summary =>
            $"{Renderer} | {Scenario.DisplayName} | {Scale.DisplayName} | {Pacing.DisplayName} | {Duration.DisplayName}";
    }
}
