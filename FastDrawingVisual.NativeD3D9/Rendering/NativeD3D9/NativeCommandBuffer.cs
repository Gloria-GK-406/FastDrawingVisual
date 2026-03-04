using System;
using System.Buffers;
using System.Buffers.Binary;
using System.Windows;
using System.Windows.Media;
using FastDrawingVisual.CommandProtocol;

namespace FastDrawingVisual.Rendering.NativeD3D9
{
    internal sealed class NativeCommandBuffer
    {
        private readonly ArrayBufferWriter<byte> _writer = new();

        public ReadOnlyMemory<byte> WrittenMemory => _writer.WrittenMemory;

        public void Reset() => _writer.Clear();

        public void WriteClear(Color color)
        {
            var span = _writer.GetSpan(BridgeCommandLayout.ClearCommandBytes);
            span[0] = (byte)BridgeCommandType.Clear;
            WriteColor(span.Slice(1 + BridgeCommandLayout.ClearColorOffset), color);
            _writer.Advance(BridgeCommandLayout.ClearCommandBytes);
        }

        public void WriteFillRect(Rect rect, Color color)
        {
            var span = _writer.GetSpan(BridgeCommandLayout.FillRectCommandBytes);
            span[0] = (byte)BridgeCommandType.FillRect;
            WriteRect(span.Slice(1 + BridgeCommandLayout.FillRectXOffset), rect);
            WriteColor(span.Slice(1 + BridgeCommandLayout.FillRectColorOffset), color);
            _writer.Advance(BridgeCommandLayout.FillRectCommandBytes);
        }

        public void WriteStrokeRect(Rect rect, float thickness, Color color)
        {
            var span = _writer.GetSpan(BridgeCommandLayout.StrokeRectCommandBytes);
            span[0] = (byte)BridgeCommandType.StrokeRect;
            WriteRect(span.Slice(1 + BridgeCommandLayout.StrokeRectXOffset), rect);
            WriteSingle(span.Slice(1 + BridgeCommandLayout.StrokeRectThicknessOffset), thickness);
            WriteColor(span.Slice(1 + BridgeCommandLayout.StrokeRectColorOffset), color);
            _writer.Advance(BridgeCommandLayout.StrokeRectCommandBytes);
        }

        public void WriteFillEllipse(Point center, float radiusX, float radiusY, Color color)
        {
            var span = _writer.GetSpan(BridgeCommandLayout.FillEllipseCommandBytes);
            span[0] = (byte)BridgeCommandType.FillEllipse;
            WritePoint(span.Slice(1 + BridgeCommandLayout.FillEllipseCenterXOffset), center);
            WriteSingle(span.Slice(1 + BridgeCommandLayout.FillEllipseRadiusXOffset), radiusX);
            WriteSingle(span.Slice(1 + BridgeCommandLayout.FillEllipseRadiusYOffset), radiusY);
            WriteColor(span.Slice(1 + BridgeCommandLayout.FillEllipseColorOffset), color);
            _writer.Advance(BridgeCommandLayout.FillEllipseCommandBytes);
        }

        public void WriteStrokeEllipse(Point center, float radiusX, float radiusY, float thickness, Color color)
        {
            var span = _writer.GetSpan(BridgeCommandLayout.StrokeEllipseCommandBytes);
            span[0] = (byte)BridgeCommandType.StrokeEllipse;
            WritePoint(span.Slice(1 + BridgeCommandLayout.StrokeEllipseCenterXOffset), center);
            WriteSingle(span.Slice(1 + BridgeCommandLayout.StrokeEllipseRadiusXOffset), radiusX);
            WriteSingle(span.Slice(1 + BridgeCommandLayout.StrokeEllipseRadiusYOffset), radiusY);
            WriteSingle(span.Slice(1 + BridgeCommandLayout.StrokeEllipseThicknessOffset), thickness);
            WriteColor(span.Slice(1 + BridgeCommandLayout.StrokeEllipseColorOffset), color);
            _writer.Advance(BridgeCommandLayout.StrokeEllipseCommandBytes);
        }

        public void WriteLine(Point p0, Point p1, float thickness, Color color)
        {
            var span = _writer.GetSpan(BridgeCommandLayout.LineCommandBytes);
            span[0] = (byte)BridgeCommandType.Line;
            WritePoint(span.Slice(1 + BridgeCommandLayout.LineX0Offset), p0);
            WritePoint(span.Slice(1 + BridgeCommandLayout.LineX1Offset), p1);
            WriteSingle(span.Slice(1 + BridgeCommandLayout.LineThicknessOffset), thickness);
            WriteColor(span.Slice(1 + BridgeCommandLayout.LineColorOffset), color);
            _writer.Advance(BridgeCommandLayout.LineCommandBytes);
        }

        private static void WriteRect(Span<byte> target, Rect rect)
        {
            WriteSingle(target.Slice(0), (float)rect.X);
            WriteSingle(target.Slice(4), (float)rect.Y);
            WriteSingle(target.Slice(8), (float)rect.Width);
            WriteSingle(target.Slice(12), (float)rect.Height);
        }

        private static void WritePoint(Span<byte> target, Point point)
        {
            WriteSingle(target.Slice(0), (float)point.X);
            WriteSingle(target.Slice(4), (float)point.Y);
        }

        private static void WriteColor(Span<byte> target, Color color)
        {
            // A,R,G,B order for easy conversion to D3DCOLOR_ARGB in native code.
            target[0] = color.A;
            target[1] = color.R;
            target[2] = color.G;
            target[3] = color.B;
        }

        private static void WriteSingle(Span<byte> target, float value)
        {
            BinaryPrimitives.WriteInt32LittleEndian(target, BitConverter.SingleToInt32Bits(value));
        }
    }
}
