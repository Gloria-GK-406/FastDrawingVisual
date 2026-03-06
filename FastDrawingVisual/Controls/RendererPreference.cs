namespace FastDrawingVisual.Controls
{
    /// <summary>
    /// Preferred renderer selection for <see cref="FastDrawingVisual"/>.
    /// </summary>
    public enum RendererPreference
    {
        Auto = 0,
        Skia = 1,
        NativeD3D9 = 2,
        DCompD3D11 = 3,
        WpfFallback = 4,
    }
}
