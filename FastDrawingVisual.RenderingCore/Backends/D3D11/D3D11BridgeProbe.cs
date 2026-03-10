extern alias D3D11Proxy;

using System;
using System.IO;
using System.Runtime.InteropServices;
using Proxy = D3D11Proxy::FastDrawingVisual.NativeProxy.NativeProxy;

namespace FastDrawingVisual.Rendering.Backends
{
    public static class D3D11BridgeProbe
    {
        [Flags]
        private enum NativeProxyCapability
        {
            None = 0,
            CommandStream = 1 << 0,
            SwapChain = 1 << 3,
            Resize = 1 << 5,
        }

        private const NativeProxyCapability RequiredCapabilities =
            NativeProxyCapability.CommandStream |
            NativeProxyCapability.SwapChain |
            NativeProxyCapability.Resize;

        private static readonly Lazy<bool> s_isAvailable = new(CheckAvailable, isThreadSafe: true);

        public static bool IsAvailable => s_isAvailable.Value;

        private static bool CheckAvailable()
        {
            if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                return false;

            var sysDir = Environment.GetFolderPath(Environment.SpecialFolder.System);
            if (!File.Exists(Path.Combine(sysDir, "d3d11.dll")))
                return false;

            var proxyAssemblyPath = typeof(Proxy).Assembly.Location;
            if (string.IsNullOrWhiteSpace(proxyAssemblyPath) || !File.Exists(proxyAssemblyPath))
                return false;

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
