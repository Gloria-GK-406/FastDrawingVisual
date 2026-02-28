using FastDrawingVisual.Rendering.Composition;

namespace FastDrawingVisual.Rendering.DComp.Backends
{
    /// <summary>
    /// D3D11 适配骨架。后续在此接入 IDXGISwapChain1 / IDCompositionSurface。
    /// </summary>
    internal sealed class D3D11CompositionBackend : BufferedGraphicsBackendBase
    {
        public override GraphicsBackendKind Kind => GraphicsBackendKind.D3D11;
    }
}
