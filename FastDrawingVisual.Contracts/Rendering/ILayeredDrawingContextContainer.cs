using System;

namespace FastDrawingVisual.Rendering
{
    /// <summary>
    /// Optional drawing capability that exposes ordered drawing buckets.
    /// </summary>
    public interface ILayeredDrawingContextContainer
    {
        /// <summary>
        /// Gets the fixed number of available layers.
        /// </summary>
        int LayerCount { get; }

        /// <summary>
        /// Gets the drawing context bound to one logical layer bucket.
        /// </summary>
        IDrawingContext GetLayer(int layerIndex);
    }
}
