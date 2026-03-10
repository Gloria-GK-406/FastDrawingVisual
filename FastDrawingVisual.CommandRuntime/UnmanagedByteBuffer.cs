using System;
using System.Runtime.InteropServices;

namespace FastDrawingVisual.CommandRuntime
{
    internal sealed unsafe class UnmanagedByteBuffer : IDisposable
    {
        private byte* _pointer;
        private int _capacity;
        private bool _isDisposed;

        public int Length { get; private set; }

        public IntPtr Pointer => Length == 0 ? IntPtr.Zero : (IntPtr)_pointer;

        public UnmanagedByteBuffer(int initialCapacity)
        {
            if (initialCapacity <= 0)
                throw new ArgumentOutOfRangeException(nameof(initialCapacity));

            _pointer = (byte*)NativeMemory.Alloc((nuint)initialCapacity);
            _capacity = initialCapacity;
        }

        public void Clear()
        {
            ThrowIfDisposed();
            Length = 0;
        }

        public Span<byte> Allocate(int size, bool clear = false)
        {
            ThrowIfDisposed();
            if (size < 0)
                throw new ArgumentOutOfRangeException(nameof(size));
            if (size == 0)
                return Span<byte>.Empty;

            var offset = Length;
            EnsureCapacity(checked(offset + size));
            Length = checked(offset + size);

            var span = new Span<byte>(_pointer + offset, size);
            if (clear)
                span.Clear();

            return span;
        }

        public void Dispose()
        {
            if (_isDisposed)
                return;

            NativeMemory.Free(_pointer);
            _pointer = null;
            _capacity = 0;
            Length = 0;
            _isDisposed = true;
        }

        private void EnsureCapacity(int requiredCapacity)
        {
            if (requiredCapacity <= _capacity)
                return;

            var newCapacity = _capacity;
            while (newCapacity < requiredCapacity)
            {
                var doubled = newCapacity <= (int.MaxValue / 2) ? newCapacity * 2 : int.MaxValue;
                newCapacity = Math.Max(doubled, requiredCapacity);
                if (newCapacity <= 0)
                    throw new OutOfMemoryException("Buffer capacity overflow.");
            }

            _pointer = (byte*)NativeMemory.Realloc(_pointer, (nuint)newCapacity);
            _capacity = newCapacity;
        }

        private void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(UnmanagedByteBuffer));
        }
    }
}
