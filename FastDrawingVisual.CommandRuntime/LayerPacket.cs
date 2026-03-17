using System;
using System.Runtime.InteropServices;

namespace FastDrawingVisual.CommandRuntime
{
    [StructLayout(LayoutKind.Sequential)]
    public struct LayerPacket
    {
        public IntPtr CommandPointer;
        public int CommandBytes;
        public IntPtr BlobPointer;
        public int BlobBytes;
        public int CommandCount;

        public bool HasCommands => CommandPointer != IntPtr.Zero && CommandBytes > 0 && CommandCount > 0;

    }
}
