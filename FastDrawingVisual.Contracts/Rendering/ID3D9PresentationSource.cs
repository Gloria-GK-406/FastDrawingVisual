using System;

namespace FastDrawingVisual.Rendering
{
    public interface ID3D9PresentationSource
    {
        IntPtr GetSurface9();

        bool CopyReadyToPresentSurface();

        void NotifyFrontBufferAvailable(bool available);
    }
}
