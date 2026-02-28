using FastDrawingVisual.Rendering.Composition;

namespace FastDrawingVisual.Rendering.DComp.Backends
{
    /// <summary>
    /// D3D12 适配骨架。后续在此接入 D3D12 + DComp 呈现通路。
    /// </summary>
    internal sealed class D3D12CompositionBackend : BufferedGraphicsBackendBase
    {
        public override GraphicsBackendKind Kind => GraphicsBackendKind.D3D12;
    }
}
