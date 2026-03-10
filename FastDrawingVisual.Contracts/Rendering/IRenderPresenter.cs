using System;
using System.Windows.Controls;

namespace FastDrawingVisual.Rendering
{
    public interface IRenderPresenter : IDisposable
    {
        bool IsPresentationReady { get; }

        event Action? ReadyStateChanged;

        bool AttachToElement(ContentControl element, ICapabilityProvider capabilityProvider);

        void Resize(int width, int height);
    }
}
