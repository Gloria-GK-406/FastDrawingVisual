using FastDrawingVisual.Rendering;
using System;
using System.Windows;

namespace FastDrawingVisual.DCompD3D11
{
    /// <summary>
    /// D3D11 DComp renderer placeholder. Real interop is intentionally deferred.
    /// </summary>
    public sealed class DCompD3D11Renderer : IRenderer
    {
        private bool _isDisposed;

        public bool AttachToElement(FrameworkElement element)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            return false;
        }

        public bool Initialize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
            return false;
        }

        public void Resize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
        }

        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(DCompD3D11Renderer));
        }

        public void Dispose()
        {
            _isDisposed = true;
        }
    }
}
