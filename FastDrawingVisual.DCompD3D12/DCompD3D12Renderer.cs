using FastDrawingVisual.Rendering;
using System;
using System.Windows;

namespace FastDrawingVisual.DCompD3D12
{
    /// <summary>
    /// D3D12 DComp renderer placeholder. Real interop is intentionally deferred.
    /// </summary>
    public sealed class DCompD3D12Renderer : IRenderer
    {
        private bool _isDisposed;

        public bool AttachToElement(FrameworkElement element)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D12Renderer));
            return false;
        }

        public bool Initialize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D12Renderer));
            return false;
        }

        public void Resize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D12Renderer));
        }

        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D12Renderer));
        }

        public void Dispose()
        {
            _isDisposed = true;
        }
    }
}
