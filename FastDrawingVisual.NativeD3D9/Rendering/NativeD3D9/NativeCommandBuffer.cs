using System;
using System.Buffers;
using System.Buffers.Binary;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisual.Rendering.NativeD3D9
{
    internal sealed class NativeCommandBuffer
    {
        private readonly ArrayBufferWriter<byte> _writer = new();

        public ReadOnlyMemory<byte> WrittenMemory => _writer.WrittenMemory;

        public void Reset() => _writer.Clear();

        public void WriteClear(Color color)
        {
            var span = _writer.GetSpan(1 + 4);
            span[0] = (byte)NativeCommandType.Clear;
            WriteColor(span.Slice(1), color);
            _writer.Advance(5);
        }

        public void WriteFillRect(Rect rect, Color color)
        {
            var span = _writer.GetSpan(1 + 16 + 4);
            span[0] = (byte)NativeCommandType.FillRect;
            WriteRect(span.Slice(1), rect);
            WriteColor(span.Slice(17), color);
            _writer.Advance(21);
        }

        public void WriteStrokeRect(Rect rect, float thickness, Color color)
        {
            var span = _writer.GetSpan(1 + 16 + 4 + 4);
            span[0] = (byte)NativeCommandType.StrokeRect;
            WriteRect(span.Slice(1), rect);
            WriteSingle(span.Slice(17), thickness);
            WriteColor(span.Slice(21), color);
            _writer.Advance(25);
        }

        public void WriteFillEllipse(Point center, float radiusX, float radiusY, Color color)
        {
            var span = _writer.GetSpan(1 + 16 + 4);
            span[0] = (byte)NativeCommandType.FillEllipse;
            WritePoint(span.Slice(1), center);
            WriteSingle(span.Slice(9), radiusX);
            WriteSingle(span.Slice(13), radiusY);
            WriteColor(span.Slice(17), color);
            _writer.Advance(21);
        }

        public void WriteStrokeEllipse(Point center, float radiusX, float radiusY, float thickness, Color color)
        {
            var span = _writer.GetSpan(1 + 16 + 4 + 4);
            span[0] = (byte)NativeCommandType.StrokeEllipse;
            WritePoint(span.Slice(1), center);
            WriteSingle(span.Slice(9), radiusX);
            WriteSingle(span.Slice(13), radiusY);
            WriteSingle(span.Slice(17), thickness);
            WriteColor(span.Slice(21), color);
            _writer.Advance(25);
        }

        public void WriteLine(Point p0, Point p1, float thickness, Color color)
        {
            var span = _writer.GetSpan(1 + 20 + 4);
            span[0] = (byte)NativeCommandType.Line;
            WritePoint(span.Slice(1), p0);
            WritePoint(span.Slice(9), p1);
            WriteSingle(span.Slice(17), thickness);
            WriteColor(span.Slice(21), color);
            _writer.Advance(25);
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
