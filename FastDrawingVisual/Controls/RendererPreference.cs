namespace FastDrawingVisual.Controls
{
    /// <summary>
    /// Preferred renderer selection for <see cref="FastDrawingVisual"/>.
    /// </summary>
    public enum RendererPreference
    {
        /// <summary>
        /// Automatically selects the best available renderer.
        /// Prefers Skia, then D3D9, and falls back to WPF when needed.
        /// </summary>
        Auto = 0,
        /// <summary>
        /// Uses the Skia-based accelerated renderer.
        /// </summary>
        Skia = 1,
        /// <summary>
        /// Uses the D3D9-based renderer.
        /// This mode avoids WPF airspace limitations.
        /// </summary>
        D3D9 = 2,
        /// <summary>
        /// Uses the D3D airspace-based renderer.
        /// This mode may provide higher performance in some scenarios,
        /// but has airspace behavior/limitations when hosted in WPF.
        /// </summary>
        D3D11AirSpace = 3,
        /// <summary>
        /// Uses the native WPF rendering path based on DrawingVisual.
        /// This mode integrates best with WPF and serves as the most compatible fallback.
        /// </summary>
        Wpf = 4,
        /// <summary>
        /// Uses a D3D11-led renderer that exports shared surfaces for D3D9/D3DImage presentation.
        /// This mode is experimental and intended for D3D9-vs-D3D11-led comparison.
        /// </summary>
        D3D11ShareD3D9 = 5,
    }
}
