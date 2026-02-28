using System;
using System.Runtime.InteropServices;

namespace FastDrawingVisual.Rendering.NativeD3D9
{
    internal static class NativeD3D9Bridge
    {
        private const string DllName = "FastDrawingVisual.NativeD3D9Bridge";

        [DllImport(DllName, EntryPoint = "FDV_IsBridgeReady", CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool IsBridgeReady();

        [DllImport(DllName, EntryPoint = "FDV_CreateRenderer", CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr CreateRenderer(IntPtr hwnd, int width, int height);

        [DllImport(DllName, EntryPoint = "FDV_DestroyRenderer", CallingConvention = CallingConvention.Cdecl)]
        internal static extern void DestroyRenderer(IntPtr renderer);

        [DllImport(DllName, EntryPoint = "FDV_Resize", CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool Resize(IntPtr renderer, int width, int height);

        [DllImport(DllName, EntryPoint = "FDV_SubmitCommands", CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool SubmitCommands(IntPtr renderer, IntPtr commands, int commandBytes);

        [DllImport(DllName, EntryPoint = "FDV_TryAcquirePresentSurface", CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool TryAcquirePresentSurface(IntPtr renderer, out IntPtr surface9);

        [DllImport(DllName, EntryPoint = "FDV_OnSurfacePresented", CallingConvention = CallingConvention.Cdecl)]
        internal static extern void OnSurfacePresented(IntPtr renderer);

        [DllImport(DllName, EntryPoint = "FDV_OnFrontBufferAvailable", CallingConvention = CallingConvention.Cdecl)]
        internal static extern void OnFrontBufferAvailable(IntPtr renderer, [MarshalAs(UnmanagedType.I1)] bool available);
    }
}
