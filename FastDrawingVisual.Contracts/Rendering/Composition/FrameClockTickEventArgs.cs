using System;

namespace FastDrawingVisual.Rendering.Composition
{
    /// <summary>
    /// 帧时钟 Tick 参数。
    /// </summary>
    public sealed class FrameClockTickEventArgs : EventArgs
    {
        public FrameClockTickEventArgs(long sequenceId, DateTime timestampUtc)
        {
            SequenceId = sequenceId;
            TimestampUtc = timestampUtc;
        }

        public long SequenceId { get; }

        public DateTime TimestampUtc { get; }
    }
}
