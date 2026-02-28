using FastDrawingVisual.Rendering.Composition;

namespace FastDrawingVisual.Rendering.DComp.Backends
{
    /// <summary>
    /// D3D9 适配骨架。后续在此接入 NativeD3D9 + DComp surface bridge。
    /// </summary>
    internal sealed class D3D9CompositionBackend : BufferedGraphicsBackendBase
    {
        public override GraphicsBackendKind Kind => GraphicsBackendKind.D3D9;
    }
}
