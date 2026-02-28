using System;

namespace FastDrawingVisual.Rendering.Composition
{
    /// <summary>
    /// 图形后端统一适配层（D3D9/11/12）。
    /// </summary>
    public interface IGraphicsCompositionBackend : IDisposable
    {
        GraphicsBackendKind Kind { get; }

        bool Initialize(IntPtr hostHwnd, int width, int height, int bufferCount);

        void Resize(int width, int height);

        ICompositionFrame? TryAcquireForDrawing();

        void CompleteDrawing(ICompositionFrame frame, bool success);

        ICompositionFrame? TryAcquireForPresent();

        void Present(ICompositionFrame frame, FramePresentationInfo info);

        void CompletePresent(ICompositionFrame frame);
    }
}
