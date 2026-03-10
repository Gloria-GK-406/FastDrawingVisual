using System;

namespace FastDrawingVisual.CommandRuntime
{
    public readonly struct BridgeCommandPacket
    {
        public IntPtr CommandPointer { get; }
        public int CommandBytes { get; }
        public IntPtr BlobPointer { get; }
        public int BlobBytes { get; }
        public int CommandCount { get; }

        internal BridgeCommandPacket(IntPtr commandPointer, int commandBytes, IntPtr blobPointer, int blobBytes, int commandCount)
        {
            CommandPointer = commandPointer;
            CommandBytes = commandBytes;
            BlobPointer = blobPointer;
            BlobBytes = blobBytes;
            CommandCount = commandCount;
        }
    }
}
