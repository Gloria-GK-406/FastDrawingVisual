using FastDrawingVisual.CommandProtocol;
using System;
using System.Buffers.Binary;
using System.Text;
using CommandProtocolConstants = FastDrawingVisual.CommandProtocol.CommandProtocol;

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
            var span = BeginCommand((ushort)CommandType.Clear, CommandLayout.ClearSlotCount, 0);
            WriteColor(span.Slice(CommandLayout.ClearColorOffset), color);
        }

        public void WriteFillRect(float x, float y, float width, float height, CommandColorArgb8 color)
        {
            var span = BeginCommand((ushort)CommandType.FillRect, CommandLayout.FillRectSlotCount, 0);
            WriteSingle(span.Slice(CommandLayout.FillRectXOffset), x);
            WriteSingle(span.Slice(CommandLayout.FillRectYOffset), y);
            WriteSingle(span.Slice(CommandLayout.FillRectWidthOffset), width);
            WriteSingle(span.Slice(CommandLayout.FillRectHeightOffset), height);
            WriteColor(span.Slice(CommandLayout.FillRectColorOffset), color);
        }

        public void WriteStrokeRect(float x, float y, float width, float height, float thickness, CommandColorArgb8 color)
        {
            var span = BeginCommand((ushort)CommandType.StrokeRect, CommandLayout.StrokeRectSlotCount, 0);
            WriteSingle(span.Slice(CommandLayout.StrokeRectXOffset), x);
            WriteSingle(span.Slice(CommandLayout.StrokeRectYOffset), y);
            WriteSingle(span.Slice(CommandLayout.StrokeRectWidthOffset), width);
            WriteSingle(span.Slice(CommandLayout.StrokeRectHeightOffset), height);
            WriteSingle(span.Slice(CommandLayout.StrokeRectThicknessOffset), thickness);
            WriteColor(span.Slice(CommandLayout.StrokeRectColorOffset), color);
        }

        public void WriteFillRoundedRect(float x, float y, float width, float height, float radiusX, float radiusY, CommandColorArgb8 color)
        {
            var span = BeginCommand((ushort)CommandType.FillRoundedRect, CommandLayout.FillRoundedRectSlotCount, 0);
            WriteSingle(span.Slice(CommandLayout.FillRoundedRectXOffset), x);
            WriteSingle(span.Slice(CommandLayout.FillRoundedRectYOffset), y);
            WriteSingle(span.Slice(CommandLayout.FillRoundedRectWidthOffset), width);
            WriteSingle(span.Slice(CommandLayout.FillRoundedRectHeightOffset), height);
            WriteSingle(span.Slice(CommandLayout.FillRoundedRectRadiusXOffset), radiusX);
            WriteSingle(span.Slice(CommandLayout.FillRoundedRectRadiusYOffset), radiusY);
            WriteColor(span.Slice(CommandLayout.FillRoundedRectColorOffset), color);
        }

        public void WriteStrokeRoundedRect(float x, float y, float width, float height, float radiusX, float radiusY, float thickness, CommandColorArgb8 color)
        {
            var span = BeginCommand((ushort)CommandType.StrokeRoundedRect, CommandLayout.StrokeRoundedRectSlotCount, 0);
            WriteSingle(span.Slice(CommandLayout.StrokeRoundedRectXOffset), x);
            WriteSingle(span.Slice(CommandLayout.StrokeRoundedRectYOffset), y);
            WriteSingle(span.Slice(CommandLayout.StrokeRoundedRectWidthOffset), width);
            WriteSingle(span.Slice(CommandLayout.StrokeRoundedRectHeightOffset), height);
            WriteSingle(span.Slice(CommandLayout.StrokeRoundedRectRadiusXOffset), radiusX);
            WriteSingle(span.Slice(CommandLayout.StrokeRoundedRectRadiusYOffset), radiusY);
            WriteSingle(span.Slice(CommandLayout.StrokeRoundedRectThicknessOffset), thickness);
            WriteColor(span.Slice(CommandLayout.StrokeRoundedRectColorOffset), color);
        }

        public void WriteFillEllipse(float centerX, float centerY, float radiusX, float radiusY, CommandColorArgb8 color)
        {
            var span = BeginCommand((ushort)CommandType.FillEllipse, CommandLayout.FillEllipseSlotCount, 0);
            WriteSingle(span.Slice(CommandLayout.FillEllipseCenterXOffset), centerX);
            WriteSingle(span.Slice(CommandLayout.FillEllipseCenterYOffset), centerY);
            WriteSingle(span.Slice(CommandLayout.FillEllipseRadiusXOffset), radiusX);
            WriteSingle(span.Slice(CommandLayout.FillEllipseRadiusYOffset), radiusY);
            WriteColor(span.Slice(CommandLayout.FillEllipseColorOffset), color);
        }

        public void WriteStrokeEllipse(float centerX, float centerY, float radiusX, float radiusY, float thickness, CommandColorArgb8 color)
        {
            var span = BeginCommand((ushort)CommandType.StrokeEllipse, CommandLayout.StrokeEllipseSlotCount, 0);
            WriteSingle(span.Slice(CommandLayout.StrokeEllipseCenterXOffset), centerX);
            WriteSingle(span.Slice(CommandLayout.StrokeEllipseCenterYOffset), centerY);
            WriteSingle(span.Slice(CommandLayout.StrokeEllipseRadiusXOffset), radiusX);
            WriteSingle(span.Slice(CommandLayout.StrokeEllipseRadiusYOffset), radiusY);
            WriteSingle(span.Slice(CommandLayout.StrokeEllipseThicknessOffset), thickness);
            WriteColor(span.Slice(CommandLayout.StrokeEllipseColorOffset), color);
        }

        public void WriteLine(float x0, float y0, float x1, float y1, float thickness, CommandColorArgb8 color)
        {
            var span = BeginCommand((ushort)CommandType.Line, CommandLayout.LineSlotCount, 0);
            WriteSingle(span.Slice(CommandLayout.LineX0Offset), x0);
            WriteSingle(span.Slice(CommandLayout.LineY0Offset), y0);
            WriteSingle(span.Slice(CommandLayout.LineX1Offset), x1);
            WriteSingle(span.Slice(CommandLayout.LineY1Offset), y1);
            WriteSingle(span.Slice(CommandLayout.LineThicknessOffset), thickness);
            WriteColor(span.Slice(CommandLayout.LineColorOffset), color);
        }

        public void WriteDrawTextRun(float x, float y, float fontSize, CommandColorArgb8 color, string textUtf8, string fontFamilyUtf8)
        {
            var textUtf8Ref = AppendUtf8(textUtf8);
            var fontFamilyUtf8Ref = AppendUtf8(fontFamilyUtf8);
            var span = BeginCommand((ushort)CommandType.DrawTextRun, CommandLayout.DrawTextRunSlotCount, 0);
            WriteSingle(span.Slice(CommandLayout.DrawTextRunXOffset), x);
            WriteSingle(span.Slice(CommandLayout.DrawTextRunYOffset), y);
            WriteSingle(span.Slice(CommandLayout.DrawTextRunFontSizeOffset), fontSize);
            WriteColor(span.Slice(CommandLayout.DrawTextRunColorOffset), color);
            WriteBlobRef(span.Slice(CommandLayout.DrawTextRunTextUtf8Offset), textUtf8Ref);
            WriteBlobRef(span.Slice(CommandLayout.DrawTextRunFontFamilyUtf8Offset), fontFamilyUtf8Ref);
        }

        public void WriteDrawImage(float x, float y, float width, float height, uint pixelWidth, uint pixelHeight, uint stride, ReadOnlySpan<byte> pixels)
        {
            var pixelsRef = AppendBytes(pixels);
            var span = BeginCommand((ushort)CommandType.DrawImage, CommandLayout.DrawImageSlotCount, 0);
            WriteSingle(span.Slice(CommandLayout.DrawImageXOffset), x);
            WriteSingle(span.Slice(CommandLayout.DrawImageYOffset), y);
            WriteSingle(span.Slice(CommandLayout.DrawImageWidthOffset), width);
            WriteSingle(span.Slice(CommandLayout.DrawImageHeightOffset), height);
            WriteUInt32(span.Slice(CommandLayout.DrawImagePixelWidthOffset), pixelWidth);
            WriteUInt32(span.Slice(CommandLayout.DrawImagePixelHeightOffset), pixelHeight);
            WriteUInt32(span.Slice(CommandLayout.DrawImageStrideOffset), stride);
            WriteBlobRef(span.Slice(CommandLayout.DrawImagePixelsOffset), pixelsRef);
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
            var commandBytes = checked(slotCount * CommandProtocolConstants.SlotBytes);
            var span = _commandBuffer.Allocate(commandBytes, clear: true);
            BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(CommandProtocolConstants.CommandKindOffset), kind);
            BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(CommandProtocolConstants.CommandSlotCountOffset), checked((ushort)slotCount));
            BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(CommandProtocolConstants.CommandFlagsOffset), flags);
            BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(CommandProtocolConstants.CommandReservedOffset), 0);
            CommandCount++;
            return span;
        }

        private CommandBlobRef AppendUtf8(string value)
        {
            if (string.IsNullOrEmpty(value))
                return default;

            AlignBlobBuffer();
            var byteCount = Encoding.UTF8.GetByteCount(value);
            var offset = _blobBuffer.Length;
            var target = _blobBuffer.Allocate(byteCount);
            Encoding.UTF8.GetBytes(value.AsSpan(), target);
            return new CommandBlobRef(offset, byteCount);
        }

        private CommandBlobRef AppendBytes(ReadOnlySpan<byte> value)
        {
            if (value.IsEmpty)
                return default;

            AlignBlobBuffer();
            var offset = _blobBuffer.Length;
            value.CopyTo(_blobBuffer.Allocate(value.Length));
            return new CommandBlobRef(offset, value.Length);
        }

        private void AlignBlobBuffer()
        {
            var padding = PaddingFor(_blobBuffer.Length, CommandProtocolConstants.BlobAlignment);
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

        private static void WriteUInt32(Span<byte> target, uint value)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(target, value);
        }

        private static void WriteBlobRef(Span<byte> target, CommandBlobRef blobRef)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(target.Slice(0, 4), checked((uint)blobRef.Offset));
            BinaryPrimitives.WriteUInt32LittleEndian(target.Slice(4, 4), checked((uint)blobRef.Length));
        }

        private void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(CommandWriter));
        }

        private readonly struct CommandBlobRef
        {
            public readonly int Offset;
            public readonly int Length;

            public CommandBlobRef(int offset, int length)
            {
                Offset = offset;
                Length = length;
            }
        }
    }
}
