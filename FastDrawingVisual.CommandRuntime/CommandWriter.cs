using FastDrawingVisual.CommandProtocol;
using System;
using System.Buffers.Binary;
using System.Text;

namespace FastDrawingVisual.CommandRuntime
{
    public sealed unsafe class CommandWriter : IDisposable
    {
        private const int DefaultCommandCapacityBytes = 256 * 1024; //256 KB for commands
        private const int DefaultBlobCapacityBytes = 64 * 1024; //64 KB for blobs (e.g. text)

        private readonly UnmanagedByteBuffer _commandBuffer;
        private readonly UnmanagedByteBuffer _blobBuffer;
        private bool _isDisposed;

        public int CommandCount { get; private set; }

        public CommandWriter(
            int initialCommandCapacityBytes = DefaultCommandCapacityBytes,
            int initialBlobCapacityBytes = DefaultBlobCapacityBytes)
        {
            _commandBuffer = new UnmanagedByteBuffer(initialCommandCapacityBytes);
            _blobBuffer = new UnmanagedByteBuffer(initialBlobCapacityBytes);
        }

        public void Reset()
        {
            ThrowIfDisposed();
            _commandBuffer.Clear();
            _blobBuffer.Clear();
            CommandCount = 0;
        }

        public LayerPacket BuildPacket()
        {
            ThrowIfDisposed();
            if (_commandBuffer.Pointer == IntPtr.Zero || _commandBuffer.Length <= 0 || CommandCount <= 0)
                return default;

            return new LayerPacket
            {
                CommandPointer = _commandBuffer.Pointer,
                CommandBytes = _commandBuffer.Length,
                BlobPointer = _blobBuffer.Pointer,
                BlobBytes = _blobBuffer.Length,
                CommandCount = CommandCount
            };
        }

        public void WriteClear(CommandColorArgb8 color)
        {
            var span = BeginCommand((ushort)BridgeCommandType.Clear, BridgeCommandLayout.ClearSlotCount, 0);
            WriteColor(span.Slice(BridgeCommandLayout.ClearColorOffset), color);
        }

        public void WriteFillRect(float x, float y, float width, float height, CommandColorArgb8 color)
        {
            var span = BeginCommand((ushort)BridgeCommandType.FillRect, BridgeCommandLayout.FillRectSlotCount, 0);
            WriteSingle(span.Slice(BridgeCommandLayout.FillRectXOffset), x);
            WriteSingle(span.Slice(BridgeCommandLayout.FillRectYOffset), y);
            WriteSingle(span.Slice(BridgeCommandLayout.FillRectWidthOffset), width);
            WriteSingle(span.Slice(BridgeCommandLayout.FillRectHeightOffset), height);
            WriteColor(span.Slice(BridgeCommandLayout.FillRectColorOffset), color);
        }

        public void WriteStrokeRect(float x, float y, float width, float height, float thickness, CommandColorArgb8 color)
        {
            var span = BeginCommand((ushort)BridgeCommandType.StrokeRect, BridgeCommandLayout.StrokeRectSlotCount, 0);
            WriteSingle(span.Slice(BridgeCommandLayout.StrokeRectXOffset), x);
            WriteSingle(span.Slice(BridgeCommandLayout.StrokeRectYOffset), y);
            WriteSingle(span.Slice(BridgeCommandLayout.StrokeRectWidthOffset), width);
            WriteSingle(span.Slice(BridgeCommandLayout.StrokeRectHeightOffset), height);
            WriteSingle(span.Slice(BridgeCommandLayout.StrokeRectThicknessOffset), thickness);
            WriteColor(span.Slice(BridgeCommandLayout.StrokeRectColorOffset), color);
        }

        public void WriteFillEllipse(float centerX, float centerY, float radiusX, float radiusY, CommandColorArgb8 color)
        {
            var span = BeginCommand((ushort)BridgeCommandType.FillEllipse, BridgeCommandLayout.FillEllipseSlotCount, 0);
            WriteSingle(span.Slice(BridgeCommandLayout.FillEllipseCenterXOffset), centerX);
            WriteSingle(span.Slice(BridgeCommandLayout.FillEllipseCenterYOffset), centerY);
            WriteSingle(span.Slice(BridgeCommandLayout.FillEllipseRadiusXOffset), radiusX);
            WriteSingle(span.Slice(BridgeCommandLayout.FillEllipseRadiusYOffset), radiusY);
            WriteColor(span.Slice(BridgeCommandLayout.FillEllipseColorOffset), color);
        }

        public void WriteStrokeEllipse(float centerX, float centerY, float radiusX, float radiusY, float thickness, CommandColorArgb8 color)
        {
            var span = BeginCommand((ushort)BridgeCommandType.StrokeEllipse, BridgeCommandLayout.StrokeEllipseSlotCount, 0);
            WriteSingle(span.Slice(BridgeCommandLayout.StrokeEllipseCenterXOffset), centerX);
            WriteSingle(span.Slice(BridgeCommandLayout.StrokeEllipseCenterYOffset), centerY);
            WriteSingle(span.Slice(BridgeCommandLayout.StrokeEllipseRadiusXOffset), radiusX);
            WriteSingle(span.Slice(BridgeCommandLayout.StrokeEllipseRadiusYOffset), radiusY);
            WriteSingle(span.Slice(BridgeCommandLayout.StrokeEllipseThicknessOffset), thickness);
            WriteColor(span.Slice(BridgeCommandLayout.StrokeEllipseColorOffset), color);
        }

        public void WriteLine(float x0, float y0, float x1, float y1, float thickness, CommandColorArgb8 color)
        {
            var span = BeginCommand((ushort)BridgeCommandType.Line, BridgeCommandLayout.LineSlotCount, 0);
            WriteSingle(span.Slice(BridgeCommandLayout.LineX0Offset), x0);
            WriteSingle(span.Slice(BridgeCommandLayout.LineY0Offset), y0);
            WriteSingle(span.Slice(BridgeCommandLayout.LineX1Offset), x1);
            WriteSingle(span.Slice(BridgeCommandLayout.LineY1Offset), y1);
            WriteSingle(span.Slice(BridgeCommandLayout.LineThicknessOffset), thickness);
            WriteColor(span.Slice(BridgeCommandLayout.LineColorOffset), color);
        }

        public void WriteDrawTextRun(float x, float y, float fontSize, CommandColorArgb8 color, string textUtf8, string fontFamilyUtf8)
        {
            var textUtf8Ref = AppendUtf8(textUtf8);
            var fontFamilyUtf8Ref = AppendUtf8(fontFamilyUtf8);
            var span = BeginCommand((ushort)BridgeCommandType.DrawTextRun, BridgeCommandLayout.DrawTextRunSlotCount, 0);
            WriteSingle(span.Slice(BridgeCommandLayout.DrawTextRunXOffset), x);
            WriteSingle(span.Slice(BridgeCommandLayout.DrawTextRunYOffset), y);
            WriteSingle(span.Slice(BridgeCommandLayout.DrawTextRunFontSizeOffset), fontSize);
            WriteColor(span.Slice(BridgeCommandLayout.DrawTextRunColorOffset), color);
            WriteBlobRef(span.Slice(BridgeCommandLayout.DrawTextRunTextUtf8Offset), textUtf8Ref);
            WriteBlobRef(span.Slice(BridgeCommandLayout.DrawTextRunFontFamilyUtf8Offset), fontFamilyUtf8Ref);
        }

        public void Dispose()
        {
            if (_isDisposed)
                return;

            _commandBuffer.Dispose();
            _blobBuffer.Dispose();
            _isDisposed = true;
        }

        private Span<byte> BeginCommand(ushort kind, int slotCount, ushort flags)
        {
            ThrowIfDisposed();
            var commandBytes = checked(slotCount * BridgeCommandProtocol.SlotBytes);
            var span = _commandBuffer.Allocate(commandBytes, clear: true);
            BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(BridgeCommandProtocol.CommandKindOffset), kind);
            BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(BridgeCommandProtocol.CommandSlotCountOffset), checked((ushort)slotCount));
            BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(BridgeCommandProtocol.CommandFlagsOffset), flags);
            BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(BridgeCommandProtocol.CommandReservedOffset), 0);
            CommandCount++;
            return span;
        }

        private BridgeCommandBlobRef AppendUtf8(string value)
        {
            if (string.IsNullOrEmpty(value))
                return default;

            AlignBlobBuffer();
            var byteCount = Encoding.UTF8.GetByteCount(value);
            var offset = _blobBuffer.Length;
            var target = _blobBuffer.Allocate(byteCount);
            Encoding.UTF8.GetBytes(value.AsSpan(), target);
            return new BridgeCommandBlobRef(offset, byteCount);
        }

        private void AlignBlobBuffer()
        {
            var padding = PaddingFor(_blobBuffer.Length, BridgeCommandProtocol.BlobAlignment);
            if (padding == 0)
                return;

            _blobBuffer.Allocate(padding, clear: true);
        }

        private static int PaddingFor(int size, int alignment)
        {
            var remainder = size % alignment;
            return remainder == 0 ? 0 : alignment - remainder;
        }

        private static void WriteColor(Span<byte> target, CommandColorArgb8 color)
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

        private static void WriteBlobRef(Span<byte> target, BridgeCommandBlobRef blobRef)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(target.Slice(0, 4), checked((uint)blobRef.Offset));
            BinaryPrimitives.WriteUInt32LittleEndian(target.Slice(4, 4), checked((uint)blobRef.Length));
        }

        private void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(CommandWriter));
        }

        private readonly struct BridgeCommandBlobRef
        {
            public readonly int Offset;
            public readonly int Length;

            public BridgeCommandBlobRef(int offset, int length)
            {
                Offset = offset;
                Length = length;
            }
        }
    }
}
