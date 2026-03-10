using System.Collections.Generic;

namespace FastDrawingVisualApp.Benchmark.Scenarios
{
    internal static class BenchmarkScenarioCatalog
    {
        public static IReadOnlyList<IBenchmarkScenario> All { get; } =
            new IBenchmarkScenario[]
            {
                new PrimitiveStressScenario(),
                new KLineStressScenario(),
                new KLineEncodeOnlyScenario(),
                new KLineLiveAppendScenario(),
            };
    }
}
