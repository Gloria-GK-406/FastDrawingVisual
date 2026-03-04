using System;
using System.Buffers;
using System.Buffers.Binary;
using System.Text;
using System.Windows;
using System.Windows.Media;
using FastDrawingVisual.CommandProtocol;

namespace FastDrawingVisual.DCompD3D11
{
    internal sealed class DCompCommandBuffer
    {
        // Experimental text command consumed by NativeProxy.D3D11 manual parser.
        private const byte DrawTextCommandId = 7;
        private const int DrawTextHeaderBytes = 24;
        private const int DrawTextXOffset = 0;
        private const int DrawTextYOffset = 4;
        private const int DrawTextFontSizeOffset = 8;
        private const int DrawTextColorOffset = 12;
        private const int DrawTextTextLengthOffset = 16;
        private const int DrawTextFontLengthOffset = 20;

        private readonly ArrayBufferWriter<byte> _writer = new();

        public ReadOnlyMemory<byte> WrittenMemory => _writer.WrittenMemory;

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

        public void WriteDrawText(string text, Point origin, string fontFamily, float fontSize, Color color)
        {
            if (string.IsNullOrEmpty(text))
                return;

            if (string.IsNullOrWhiteSpace(fontFamily))
                fontFamily = "Segoe UI";

            if (fontSize <= 0f)
                fontSize = 12f;

            var textBytes = Encoding.UTF8.GetBytes(text);
            var fontBytes = Encoding.UTF8.GetBytes(fontFamily);
            var commandBytes = 1 + DrawTextHeaderBytes + textBytes.Length + fontBytes.Length;

            var span = _writer.GetSpan(commandBytes);
            span[0] = DrawTextCommandId;
            WriteSingle(span.Slice(1 + DrawTextXOffset), (float)origin.X);
            WriteSingle(span.Slice(1 + DrawTextYOffset), (float)origin.Y);
            WriteSingle(span.Slice(1 + DrawTextFontSizeOffset), fontSize);
            WriteColor(span.Slice(1 + DrawTextColorOffset), color);
            BinaryPrimitives.WriteInt32LittleEndian(span.Slice(1 + DrawTextTextLengthOffset), textBytes.Length);
            BinaryPrimitives.WriteInt32LittleEndian(span.Slice(1 + DrawTextFontLengthOffset), fontBytes.Length);
            textBytes.CopyTo(span.Slice(1 + DrawTextHeaderBytes));
            fontBytes.CopyTo(span.Slice(1 + DrawTextHeaderBytes + textBytes.Length));
            _writer.Advance(commandBytes);
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
