namespace FastDrawingVisual.Rendering
{
    public interface ID3D9PresentationController
    {
        bool CopyReadyToPresentSurface();

        void NotifyFrontBufferAvailable(bool available);
    }
}
