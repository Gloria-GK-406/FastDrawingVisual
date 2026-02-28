using System;

namespace FastDrawingVisual.Rendering.Composition
{
    /// <summary>
    /// DComp 渲染器抽象（不依赖 WPF 的 DrawingVisual/D3DImage）。
    /// </summary>
    public interface ICompositionRenderer : IDisposable
    {
        bool Initialize(IntPtr hostHwnd, int width, int height);

        void Resize(int width, int height);

        void SubmitDrawing(Action<IDrawingContext> drawAction);
    }
}
