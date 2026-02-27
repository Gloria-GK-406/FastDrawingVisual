namespace FastDrawingVisual.Rendering.D3D
{
    /// <summary>
    /// 渲染帧的生命周期状态。
    /// </summary>
    internal enum FrameState
    {
        /// <summary>可被分配用于绘制。</summary>
        Ready = 0,

        /// <summary>正在被绘制（由渲染线程持有）。</summary>
        Drawing = 1,

        /// <summary>绘制已完成，等待被 WPF 呈现。</summary>
        ReadyForPresent = 2,

        /// <summary>正在被 WPF D3DImage 引用，不可用于绘制。</summary>
        Presenting = 3,
    }
}
