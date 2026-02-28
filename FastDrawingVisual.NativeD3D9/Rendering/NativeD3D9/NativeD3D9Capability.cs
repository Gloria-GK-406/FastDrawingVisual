using System;
using System.IO;
using System.Runtime.InteropServices;

namespace FastDrawingVisual.Rendering.NativeD3D9
{
    internal static class NativeD3D9Capability
    {
        private static readonly Lazy<bool> _isAvailable = new(CheckAvailable, isThreadSafe: true);

        public static bool IsAvailable => _isAvailable.Value;

        private static bool CheckAvailable()
        {
            if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                return false;

            var sysDir = Environment.GetFolderPath(Environment.SpecialFolder.System);
            if (!File.Exists(Path.Combine(sysDir, "d3d9.dll")))
                return false;

            var baseDir = AppContext.BaseDirectory;
            var candidate = Path.Combine(baseDir, "FastDrawingVisual.NativeD3D9Bridge.dll");
            if (File.Exists(candidate))
                return ProbeBridgeReady();

            candidate = Path.Combine(Environment.CurrentDirectory, "FastDrawingVisual.NativeD3D9Bridge.dll");
            return File.Exists(candidate) && ProbeBridgeReady();
        }

        private static bool ProbeBridgeReady()
        {
            try
            {
                return NativeD3D9Bridge.IsBridgeReady();
            }
            catch
            {
                return false;
            }
        }
    }
}
