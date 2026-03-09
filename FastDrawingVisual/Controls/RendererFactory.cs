using FastDrawingVisual.Rendering;
using FastDrawingVisual.WpfRenderer;
using System.Runtime.CompilerServices;
using DCompD3D11Renderer = FastDrawingVisual.DCompD3D11.DCompD3D11Renderer;
using D3DSkiaRenderer = FastDrawingVisual.SkiaSharp.D3DSkiaRenderer;
using NativeD3D9Renderer = FastDrawingVisual.Rendering.NativeD3D9.NativeD3D9Renderer;

namespace FastDrawingVisual.Controls
{
    internal static class RendererFactory
    {
        internal static IRenderer Create(RendererPreference preference)
        {
            var capability = RendererCapability.Current;
            return preference == RendererPreference.Auto
                ? CreateAuto(capability)
                : CreatePreferred(preference, capability);
        }

        private static IRenderer CreateAuto(RendererCapabilityInfo capability)
        {
            if (capability.CanUseSkia)
            {
                var skia = TryCreateSkia();
                if (skia != null)
                    return skia;
            }

            if (capability.CanUseNativeD3D9)
            {
                var native = TryCreateNativeD3D9();
                if (native != null)
                    return native;
            }

            return new WpfFallbackRenderer();
        }

        private static IRenderer CreatePreferred(RendererPreference preference, RendererCapabilityInfo capability)
        {
            if (!RendererCapability.Supports(preference, capability))
                return new WpfFallbackRenderer();

            return preference switch
            {
                RendererPreference.Skia => TryCreateSkia() ?? new WpfFallbackRenderer(),
                RendererPreference.D3D9 => TryCreateNativeD3D9() ?? new WpfFallbackRenderer(),
                RendererPreference.D3D11AirSpace => TryCreateDCompD3D11() ?? new WpfFallbackRenderer(),
                RendererPreference.Wpf => new WpfFallbackRenderer(),
                _ => CreateAuto(capability),
            };
        }

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
                return new D3DSkiaRenderer();
            }
            catch
            {
                return null;
            }
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static IRenderer? TryCreateDCompD3D11()
        {
            try
            {
                return new DCompD3D11Renderer();
            }
            catch
            {
                return null;
            }
        }
    }
}
