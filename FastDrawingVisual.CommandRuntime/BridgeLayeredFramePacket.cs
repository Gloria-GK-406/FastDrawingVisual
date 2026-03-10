using System.Runtime.InteropServices;

namespace FastDrawingVisual.CommandRuntime
{
    [StructLayout(LayoutKind.Sequential)]
    public struct BridgeLayeredFramePacket
    {
        public const int MaxLayerCount = 8;

        public BridgeLayerPacket Layer0;
        public BridgeLayerPacket Layer1;
        public BridgeLayerPacket Layer2;
        public BridgeLayerPacket Layer3;
        public BridgeLayerPacket Layer4;
        public BridgeLayerPacket Layer5;
        public BridgeLayerPacket Layer6;
        public BridgeLayerPacket Layer7;

        public bool HasAnyCommands =>
            Layer0.HasCommands ||
            Layer1.HasCommands ||
            Layer2.HasCommands ||
            Layer3.HasCommands ||
            Layer4.HasCommands ||
            Layer5.HasCommands ||
            Layer6.HasCommands ||
            Layer7.HasCommands;

        public void SetLayer(int layerIndex, BridgeLayerPacket packet)
        {
            switch (layerIndex)
            {
                case 0:
                    Layer0 = packet;
                    break;
                case 1:
                    Layer1 = packet;
                    break;
                case 2:
                    Layer2 = packet;
                    break;
                case 3:
                    Layer3 = packet;
                    break;
                case 4:
                    Layer4 = packet;
                    break;
                case 5:
                    Layer5 = packet;
                    break;
                case 6:
                    Layer6 = packet;
                    break;
                case 7:
                    Layer7 = packet;
                    break;
                default:
                    throw new System.ArgumentOutOfRangeException(nameof(layerIndex));
            }
        }
    }
}
