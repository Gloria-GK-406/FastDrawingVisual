using System;

namespace FastDrawingVisualApp.Benchmark
{
    internal readonly struct BenchmarkFrameContext
    {
        public BenchmarkFrameContext(long submittedSequence, long executedFrameIndex, TimeSpan elapsed)
        {
            SubmittedSequence = submittedSequence;
            ExecutedFrameIndex = executedFrameIndex;
            Elapsed = elapsed;
        }

        public long SubmittedSequence { get; }

        public long ExecutedFrameIndex { get; }

        public TimeSpan Elapsed { get; }

        public double Phase => Elapsed.TotalSeconds;
    }
}
