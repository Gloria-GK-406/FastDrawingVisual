using System.Collections.Generic;

namespace FastDrawingVisualApp.Benchmark
{
    internal sealed class BenchmarkScalePreset
    {
        public static readonly BenchmarkScalePreset Small = new(
            key: "S",
            displayName: "S / 2k bars",
            summary: "2k total, 240 visible, 1 indicator, light annotation",
            totalBars: 2_000,
            visibleBars: 240,
            indicatorLineCount: 1,
            indicatorPanelCount: 1,
            annotationStride: 24,
            primitiveDensity: 1);

        public static readonly BenchmarkScalePreset Medium = new(
            key: "M",
            displayName: "M / 8k bars",
            summary: "8k total, 520 visible, 3 indicators, dual panels",
            totalBars: 8_000,
            visibleBars: 520,
            indicatorLineCount: 3,
            indicatorPanelCount: 2,
            annotationStride: 18,
            primitiveDensity: 2);

        public static readonly BenchmarkScalePreset Large = new(
            key: "L",
            displayName: "L / 32k bars",
            summary: "32k total, 1.2k visible, 4 indicators, dense overlays",
            totalBars: 32_000,
            visibleBars: 1_200,
            indicatorLineCount: 4,
            indicatorPanelCount: 2,
            annotationStride: 12,
            primitiveDensity: 3);

        public static readonly BenchmarkScalePreset Extreme = new(
            key: "XL",
            displayName: "XL / 120k bars",
            summary: "120k total, 2.4k visible, 4 indicators, max density",
            totalBars: 120_000,
            visibleBars: 2_400,
            indicatorLineCount: 4,
            indicatorPanelCount: 3,
            annotationStride: 8,
            primitiveDensity: 4);

        public static IReadOnlyList<BenchmarkScalePreset> All { get; } =
            new[] { Small, Medium, Large, Extreme };

        public BenchmarkScalePreset(
            string key,
            string displayName,
            string summary,
            int totalBars,
            int visibleBars,
            int indicatorLineCount,
            int indicatorPanelCount,
            int annotationStride,
            int primitiveDensity)
        {
            Key = key;
            DisplayName = displayName;
            Summary = summary;
            TotalBars = totalBars;
            VisibleBars = visibleBars;
            IndicatorLineCount = indicatorLineCount;
            IndicatorPanelCount = indicatorPanelCount;
            AnnotationStride = annotationStride;
            PrimitiveDensity = primitiveDensity;
        }

        public string Key { get; }

        public string DisplayName { get; }

        public string Summary { get; }

        public int TotalBars { get; }

        public int VisibleBars { get; }

        public int IndicatorLineCount { get; }

        public int IndicatorPanelCount { get; }

        public int AnnotationStride { get; }

        public int PrimitiveDensity { get; }

        public override string ToString() => DisplayName;
    }
}
