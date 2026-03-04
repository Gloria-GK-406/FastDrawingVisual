using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows.Interop;

namespace FastDrawingVisual
{
    internal sealed class DCompHostHwnd : HwndHost
    {
        private const int WsChild = unchecked((int)0x40000000);
        private const int WsVisible = unchecked((int)0x10000000);
        private const int WsClipSiblings = 0x04000000;
        private const int WsClipChildren = 0x02000000;

        private IntPtr _hwnd;

        internal IntPtr HostHandle => _hwnd;
        internal event Action<IntPtr>? HandleCreated;
        internal event Action? HandleDestroyed;

        protected override HandleRef BuildWindowCore(HandleRef hwndParent)
        {
            _hwnd = NativeMethods.CreateWindowEx(
                0,
                "static",
                string.Empty,
                WsChild | WsVisible | WsClipSiblings | WsClipChildren,
                0,
                0,
                1,
                1,
                hwndParent.Handle,
                IntPtr.Zero,
                IntPtr.Zero,
                IntPtr.Zero);

            if (_hwnd == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateWindowEx for DCompHostHwnd failed.");

            HandleCreated?.Invoke(_hwnd);
            return new HandleRef(this, _hwnd);
        }

        protected override void DestroyWindowCore(HandleRef hwnd)
        {
            HandleDestroyed?.Invoke();

            if (hwnd.Handle != IntPtr.Zero)
                NativeMethods.DestroyWindow(hwnd.Handle);

            _hwnd = IntPtr.Zero;
        }

        private static class NativeMethods
        {
            [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
            internal static extern IntPtr CreateWindowEx(
                int exStyle,
                string className,
                string windowName,
                int style,
                int x,
                int y,
                int width,
                int height,
                IntPtr hwndParent,
                IntPtr hMenu,
                IntPtr hInstance,
                IntPtr lpParam);

            [DllImport("user32.dll", SetLastError = true)]
            [return: MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
            internal static extern bool DestroyWindow(IntPtr hwnd);
        }
    }
}
