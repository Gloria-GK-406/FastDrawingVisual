using System;

namespace FastDrawingVisual.Rendering
{
    public static class CapabilityProviderExtensions
    {
        public static TCapability GetRequiredCapability<TCapability>(this ICapabilityProvider provider, string consumerName)
            where TCapability : class
        {
            if (provider == null) throw new ArgumentNullException(nameof(provider));
            if (provider.TryGetCapability<TCapability>(out var capability) && capability != null)
                return capability;

            throw new InvalidOperationException($"{consumerName} requires capability {typeof(TCapability).FullName}.");
        }
    }
}
