using System;

namespace FastDrawingVisual.Rendering.Composition
{
    /// <summary>
    /// 帧呈现元数据。
    /// </summary>
    public readonly struct FramePresentationInfo
    {
        public FramePresentationInfo(long frameId, DateTime submitTimeUtc)
        {
            FrameId = frameId;
            SubmitTimeUtc = submitTimeUtc;
        }

        public long FrameId { get; }

        public DateTime SubmitTimeUtc { get; }
    }
}
