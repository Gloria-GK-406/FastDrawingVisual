using FastDrawingVisual.Rendering.Composition;
using FastDrawingVisual.Rendering.DComp;
using FastDrawingVisual.Rendering.DComp.Backends;
using FastDrawingVisual.Rendering.DComp.Clock;
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Threading;

namespace FastDrawingVisual.Controls
{
    internal static class DCompRendererFactory
    {
        internal static ICompositionRenderer Create(GraphicsBackendKind preferredBackend)
        {
            var backend = CreateBackend(preferredBackend);
            var clock = CreateFrameClock();
            return new DCompRenderer(backend, clock);
        }

        private static IGraphicsCompositionBackend CreateBackend(GraphicsBackendKind preferredBackend)
        {
            if (preferredBackend != GraphicsBackendKind.Auto)
                return CreateConcreteBackend(preferredBackend);

            if (IsBackendAvailable(GraphicsBackendKind.D3D11))
                return new D3D11CompositionBackend();

            if (IsBackendAvailable(GraphicsBackendKind.D3D12))
                return new D3D12CompositionBackend();

            return new D3D9CompositionBackend();
        }

        private static IGraphicsCompositionBackend CreateConcreteBackend(GraphicsBackendKind kind)
            => kind switch
            {
                GraphicsBackendKind.D3D9 => new D3D9CompositionBackend(),
                GraphicsBackendKind.D3D11 => new D3D11CompositionBackend(),
                GraphicsBackendKind.D3D12 => new D3D12CompositionBackend(),
                _ => new D3D11CompositionBackend()
            };

        private static bool IsBackendAvailable(GraphicsBackendKind kind)
        {
            if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                return false;

            var sysDir = Environment.GetFolderPath(Environment.SpecialFolder.System);

            return kind switch
            {
                GraphicsBackendKind.D3D9 => File.Exists(Path.Combine(sysDir, "d3d9.dll")),
                GraphicsBackendKind.D3D11 => File.Exists(Path.Combine(sysDir, "d3d11.dll")),
                GraphicsBackendKind.D3D12 => File.Exists(Path.Combine(sysDir, "d3d12.dll")),
                _ => false
            };
        }

        private static ICompositionFrameClock CreateFrameClock()
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                return new DwmFlushFrameClock(Dispatcher.CurrentDispatcher);

            return new CompositionTargetFrameClock();
        }
    }
}
