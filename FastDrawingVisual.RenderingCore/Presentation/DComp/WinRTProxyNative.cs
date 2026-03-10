using System;
using System.Runtime.InteropServices;

namespace FastDrawingVisual
{
    internal static class WinRTProxyNative
    {
        private const string DllName = "FastDrawingVisual.WINRTProxy";

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
