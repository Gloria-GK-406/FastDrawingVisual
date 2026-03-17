using System.Runtime.InteropServices;

namespace FastDrawingVisual.CommandRuntime
{
    [StructLayout(LayoutKind.Sequential)]
    public struct LayeredFramePacket
    {
        public const int MaxLayerCount = 8;

        public LayerPacket Layer0;
        public LayerPacket Layer1;
        public LayerPacket Layer2;
        public LayerPacket Layer3;
        public LayerPacket Layer4;
        public LayerPacket Layer5;
        public LayerPacket Layer6;
        public LayerPacket Layer7;

        public bool HasAnyCommands =>
            Layer0.HasCommands ||
            Layer1.HasCommands ||
            Layer2.HasCommands ||
            Layer3.HasCommands ||
            Layer4.HasCommands ||
            Layer5.HasCommands ||
            Layer6.HasCommands ||
            Layer7.HasCommands;

        public void SetLayer(int layerIndex, LayerPacket packet)
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

        public LayerPacket GetLayer(int layerIndex)
        {
            return layerIndex switch
            {
                0 => Layer0,
                1 => Layer1,
                2 => Layer2,
                3 => Layer3,
                4 => Layer4,
                5 => Layer5,
                6 => Layer6,
                7 => Layer7,
                _ => throw new System.ArgumentOutOfRangeException(nameof(layerIndex)),
            };
        }
    }
}
