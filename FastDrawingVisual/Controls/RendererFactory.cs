using FastDrawingVisual.Rendering;
using FastDrawingVisual.Rendering.NativeD3D9;
using FastDrawingVisual.WpfRenderer;
using System.Runtime.CompilerServices;
using D3DSkiaRenderer = FastDrawingVisual.SkiaSharp.D3DSkiaRenderer;
using NativeD3D9Renderer = FastDrawingVisual.Rendering.NativeD3D9.NativeD3D9Renderer;

namespace FastDrawingVisual.Controls
{
    /// <summary>
    /// <see cref="IRenderer"/> 工厂。
    /// 根据运行时环境自动选择最优实现：
    /// <list type="bullet">
    ///   <item>Windows 10+（有 d3d12.dll + D3D11）→ D3DSkiaRenderer（GPU 加速）</item>
    ///   <item>其余 → WpfFallbackRenderer（WPF 纯软件降级）</item>
    /// </list>
    /// <para>
    /// 【DLL 隔离保证】<br/>
    /// <see cref="TryCreateSkia"/> 被标记为 <see cref="MethodImplOptions.NoInlining"/>，
    /// CLR 仅在该方法实际被调用时才 JIT 编译其方法体，从而延迟 SkiaSharp / SharpDX
    /// 原生 DLL 的加载至首次调用时。<br/>
    /// 在 Win7 / 无 d3d12.dll 环境下，<see cref="RendererCapability.IsAcceleratedAvailable"/>
    /// 返回 <c>false</c>，<see cref="TryCreateSkia"/> 永远不被调用，
    /// libSkiaSharp.dll / d3d12.dll 永远不被 OS 加载。
    /// </para>
    /// </summary>
    internal static class RendererFactory
    {
        internal static IRenderer Create()
        {
            if (RendererCapability.IsAcceleratedAvailable)
            {
                var r = TryCreateSkia();
                if (r != null) return r;
            }

            if (NativeD3D9Capability.IsAvailable)
            {
                var native = TryCreateNativeD3D9();
                if (native != null) return native;
            }

            if (RendererCapability.IsAcceleratedAvailable)
            {
                var r = TryCreateSkia();
                if (r != null) return r;
            }

            // 降级路径：纯 WPF，零 D3D/Skia 依赖
            return new WpfFallbackRenderer();
        }

        /// <summary>
        /// 尝试创建 D3D11 + SkiaSharp 加速渲染器。
        /// </summary>
        /// <remarks>
        /// [NoInlining] 确保此方法的 JIT 编译（以及 D3DSkiaRenderer 类型解析/SharpDX+Skia 程序集加载）
        /// 被推迟到本方法第一次被实际调用时，而非在外层方法编译时发生。
        /// </remarks>
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static IRenderer? TryCreateNativeD3D9()
        {
            try
            {
                return new NativeD3D9Renderer();
            }
            catch
            {
                return null;
            }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static IRenderer? TryCreateSkia()
        {
            try
            {
                var renderer = new D3DSkiaRenderer();
                return renderer;
            }
            catch
            {
                // GPU 设备初始化失败（驱动异常、TDR 等）→ 降级
                return null;
            }
        }
    }
}
