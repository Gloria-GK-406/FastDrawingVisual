using System;
using System.Collections.Generic;

namespace FastDrawingVisualApp.Benchmark
{
    internal sealed class RunDurationPreset
    {
        public static readonly RunDurationPreset Manual = new("Manual", "Run until stopped manually.", null);
        public static readonly RunDurationPreset Seconds15 = new("15 s", "Short smoke benchmark window.", TimeSpan.FromSeconds(15));
        public static readonly RunDurationPreset Seconds30 = new("30 s", "Default benchmark window.", TimeSpan.FromSeconds(30));
        public static readonly RunDurationPreset Minute1 = new("60 s", "Longer soak for stability and drop behavior.", TimeSpan.FromMinutes(1));

        public static IReadOnlyList<RunDurationPreset> All { get; } =
            new[] { Manual, Seconds15, Seconds30, Minute1 };

        public RunDurationPreset(string displayName, string description, TimeSpan? duration)
        {
            DisplayName = displayName;
            Description = description;
            Duration = duration;
        }

        public string DisplayName { get; }

        public string Description { get; }

        public TimeSpan? Duration { get; }

        public override string ToString() => DisplayName;
    }
}
