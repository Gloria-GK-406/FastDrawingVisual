using System;
using System.Windows;
using System.Windows.Controls;

namespace FastDrawingVisual.Rendering
{
    /// <summary>
    /// Unified renderer contract used by host controls.
    /// </summary>
    public interface IRenderer : IDisposable
    {
        /// <summary>
        /// Attach renderer output to a host element.
        /// </summary>
        bool AttachToElement(ContentControl element);

        /// <summary>
        /// Initialize renderer resources.
        /// </summary>
        bool Initialize(int width, int height);

        /// <summary>
        /// Resize renderer resources.
        /// </summary>
        void Resize(int width, int height);

        /// <summary>
        /// Submit one drawing delegate with replace semantics.
        /// </summary>
        void SubmitDrawing(Action<IDrawingContext> drawAction);
    }
}
