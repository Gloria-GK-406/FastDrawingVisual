using System;

namespace FastDrawingVisualApp.Benchmark
{
    internal sealed class BenchmarkRunRecord
    {
        public BenchmarkRunRecord(
            DateTime endedAt,
            BenchmarkConfig config,
            string scenarioSummary,
            BenchmarkMetricsSnapshot snapshot)
        {
            EndedAt = endedAt;
            Config = config;
            ScenarioSummary = scenarioSummary;
            Snapshot = snapshot;
        }

        public DateTime EndedAt { get; }

        public BenchmarkConfig Config { get; }

        public string ScenarioSummary { get; }

        public BenchmarkMetricsSnapshot Snapshot { get; }

        public string Header =>
            $"{EndedAt:HH:mm:ss}  {Config.Scenario.DisplayName}  {Config.Scale.Key}  {Config.Renderer}";

        public string Detail =>
            $"submit {Snapshot.SubmitHz:F1}Hz | exec {Snapshot.ExecuteHz:F1}Hz | drop {Snapshot.RecentDropRatePercent:F1}% | draw p95 {Snapshot.DrawDuration.P95:F2}ms | latency p95 {Snapshot.EndToEndLatency.P95:F2}ms | {Snapshot.StopReason}";
    }
}
