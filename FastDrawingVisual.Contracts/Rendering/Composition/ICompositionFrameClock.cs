using System;

namespace FastDrawingVisual.Rendering.Composition
{
    /// <summary>
    /// 帧驱动时钟，通常由 DWM 或渲染循环触发。
    /// </summary>
    public interface ICompositionFrameClock : IDisposable
    {
        event EventHandler<FrameClockTickEventArgs>? Tick;

        bool IsRunning { get; }

        void Start();

        void Stop();
    }
}
