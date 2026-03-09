using System.Collections.Generic;

namespace FastDrawingVisualApp.Benchmark
{
    internal sealed class SubmitPacingPreset
    {
        public static readonly SubmitPacingPreset P60 = new(
            "60 Hz", "Matches typical display cadence.", 60);

        public static readonly SubmitPacingPreset P120 = new(
            "120 Hz", "Moderate overload against a 60 Hz present path.", 120);

        public static readonly SubmitPacingPreset P240 = new(
            "240 Hz", "Heavy overload to expose latest-wins pressure.", 240);

        public static readonly SubmitPacingPreset Unlimited = new(
            "Unlimited", "Submit as fast as the producer thread can run.", null);

        public static IReadOnlyList<SubmitPacingPreset> All { get; } =
            new[] { P60, P120, P240, Unlimited };

        public SubmitPacingPreset(string displayName, string description, int? targetHz)
        {
            DisplayName = displayName;
            Description = description;
            TargetHz = targetHz;
        }

        public string DisplayName { get; }

        public string Description { get; }

        public int? TargetHz { get; }

        public override string ToString() => DisplayName;
    }
}
