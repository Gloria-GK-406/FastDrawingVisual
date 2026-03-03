using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace FastDrawingVisual
{
    internal static class WinRTProxyNative
    {
        private const string DllName = "FastDrawingVisual.WINRTProxy";
        private const string DllFileName = "FastDrawingVisual.WINRTProxy.dll";
        private static bool _resolverRegistered;

        internal static void EnsureResolverRegistered()
        {
            if (_resolverRegistered)
                return;

            NativeLibrary.SetDllImportResolver(typeof(WinRTProxyNative).Assembly, ResolveDllImport);
            _resolverRegistered = true;
        }

        private static IntPtr ResolveDllImport(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
        {
            if (!string.Equals(libraryName, DllName, StringComparison.OrdinalIgnoreCase))
                return IntPtr.Zero;

            var arch = Environment.Is64BitProcess ? "x64" : "x86";
            var baseDir = AppContext.BaseDirectory;
            var candidate = Path.Combine(baseDir, "lib", arch, DllFileName);
            if (File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out var handle))
                return handle;

            var fallback = Path.Combine(baseDir, DllFileName);
            if (File.Exists(fallback) && NativeLibrary.TryLoad(fallback, out handle))
                return handle;

            return IntPtr.Zero;
        }

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "FDV_WinRTProxy_IsReady")]
        [return: MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
        internal static extern bool FDV_WinRTProxy_IsReady();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "FDV_WinRTProxy_Create")]
        internal static extern IntPtr FDV_WinRTProxy_Create();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "FDV_WinRTProxy_Destroy")]
        internal static extern void FDV_WinRTProxy_Destroy(IntPtr proxy);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "FDV_WinRTProxy_EnsureDispatcherQueue")]
        [return: MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
        internal static extern bool FDV_WinRTProxy_EnsureDispatcherQueue(IntPtr proxy);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "FDV_WinRTProxy_EnsureDesktopTarget")]
        [return: MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
        internal static extern bool FDV_WinRTProxy_EnsureDesktopTarget(
            IntPtr proxy,
            IntPtr hwnd,
            [MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)] bool isTopmost);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "FDV_WinRTProxy_BindSwapChain")]
        [return: MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
        internal static extern bool FDV_WinRTProxy_BindSwapChain(IntPtr proxy, IntPtr swapChain);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "FDV_WinRTProxy_UpdateSpriteRect")]
        [return: MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
        internal static extern bool FDV_WinRTProxy_UpdateSpriteRect(
            IntPtr proxy,
            float offsetX,
            float offsetY,
            float width,
            float height);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "FDV_WinRTProxy_GetLastErrorHr")]
        internal static extern int FDV_WinRTProxy_GetLastErrorHr(IntPtr proxy);
    }
}
