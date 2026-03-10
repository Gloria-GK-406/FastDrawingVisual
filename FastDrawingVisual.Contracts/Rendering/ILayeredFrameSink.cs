using FastDrawingVisual.CommandRuntime;

namespace FastDrawingVisual.Rendering
{
    public interface ILayeredFrameSink
    {
        void SubmitFrame(in BridgeLayeredFramePacket frame);
    }
}
