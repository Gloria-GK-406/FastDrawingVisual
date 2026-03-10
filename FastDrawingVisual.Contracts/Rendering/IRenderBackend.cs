namespace FastDrawingVisual.Rendering
{
    public interface IRenderBackend : System.IDisposable, ICapabilityProvider
    {
        bool Initialize(int width, int height);

        void Resize(int width, int height);
    }
}
