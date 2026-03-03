using System;
using System.IO;
using System.Runtime.InteropServices;
using Proxy = FastDrawingVisual.NativeProxy.NativeProxy;

namespace FastDrawingVisual.Rendering.NativeD3D9
{
    internal static class NativeD3D9Capability
    {
        [Flags]
        private enum NativeProxyCapability
        {
            None = 0,
            CommandStream = 1 << 0,
            PresentSurface = 1 << 1,
            FrontBufferNotifications = 1 << 2,
        }

        private const NativeProxyCapability RequiredCapabilities =
            NativeProxyCapability.CommandStream |
            NativeProxyCapability.PresentSurface |
            NativeProxyCapability.FrontBufferNotifications;

        private static readonly Lazy<bool> _isAvailable = new(CheckAvailable, isThreadSafe: true);

        public static bool IsAvailable => _isAvailable.Value;

        private static bool CheckAvailable()
        {
            if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                return false;

            var sysDir = Environment.GetFolderPath(Environment.SpecialFolder.System);
            if (!File.Exists(Path.Combine(sysDir, "d3d9.dll")))
                return false;

            var proxyAssemblyPath = typeof(Proxy).Assembly.Location; ;
            if (string.IsNullOrWhiteSpace(proxyAssemblyPath) || !File.Exists(proxyAssemblyPath))
                return false;

            return ProbeBridgeReady();
        }

        private static bool ProbeBridgeReady()
        {
            try
            {
                if (!Proxy.IsBridgeReady())
                    return false;

                var capabilities = (NativeProxyCapability)Proxy.GetBridgeCapabilities();
                return (capabilities & RequiredCapabilities) == RequiredCapabilities;
            }
            catch
            {
                return false;
            }
        }
    }
}
