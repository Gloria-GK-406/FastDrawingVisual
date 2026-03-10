using System;

namespace FastDrawingVisual.Rendering
{
    public interface IRenderBackendReadiness
    {
        bool IsReadyForRendering { get; }

        event Action? ReadyStateChanged;
    }
}
