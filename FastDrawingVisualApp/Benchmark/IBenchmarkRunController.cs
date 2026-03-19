namespace FastDrawingVisualApp.Benchmark
{
    internal interface IBenchmarkRunController : System.IDisposable
    {
        BenchmarkConfig Config { get; }

        string ScenarioSummary { get; }

        bool IsRunning { get; }

        BenchmarkMetricsSnapshot CreateSnapshot();

        void RequestManualStop();
    }
}
