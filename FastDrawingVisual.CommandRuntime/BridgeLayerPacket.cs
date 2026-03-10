using System;
using System.Runtime.InteropServices;

namespace FastDrawingVisual.CommandRuntime
{
    [StructLayout(LayoutKind.Sequential)]
    public struct BridgeLayerPacket
    {
        public IntPtr CommandPointer;
        public int CommandBytes;
        public IntPtr BlobPointer;
        public int BlobBytes;
        public int CommandCount;

        public bool HasCommands => CommandPointer != IntPtr.Zero && CommandBytes > 0 && CommandCount > 0;

        public static BridgeLayerPacket FromCommandPacket(BridgeCommandPacket packet)
        {
            if (packet.CommandPointer == IntPtr.Zero || packet.CommandBytes <= 0 || packet.CommandCount <= 0)
                return default;

            return new BridgeLayerPacket
            {
                CommandPointer = packet.CommandPointer,
                CommandBytes = packet.CommandBytes,
                BlobPointer = packet.BlobPointer,
                BlobBytes = packet.BlobBytes,
                CommandCount = packet.CommandCount
            };
        }
    }
}
