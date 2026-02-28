using System;

namespace FastDrawingVisual.Rendering.Composition
{
    /// <summary>
    /// 后端帧对象（通常对应一块可复用缓冲）。
    /// </summary>
    public interface ICompositionFrame
    {
        int Width { get; }

        int Height { get; }

        /// <summary>
        /// 原生表面句柄（例如纹理句柄/表面指针）；具体语义由后端定义。
        /// </summary>
        IntPtr NativeSurfaceHandle { get; }

        IDrawingContext OpenDrawingContext();
    }
}
