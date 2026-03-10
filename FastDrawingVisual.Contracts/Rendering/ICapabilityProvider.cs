namespace FastDrawingVisual.Rendering
{
    public interface ICapabilityProvider
    {
        bool TryGetCapability<TCapability>(out TCapability? capability)
            where TCapability : class;
    }
}
