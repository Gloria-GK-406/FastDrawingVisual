using System.Windows.Media;

namespace FastDrawingVisual.Rendering
{
    /// <summary>
    /// Visual host abstraction implemented by WPF controls that host renderer visuals.
    /// </summary>
    public interface IVisualHostElement
    {
        bool AttachVisual(Visual visual);

        bool DetachVisual(Visual visual);
    }
}
