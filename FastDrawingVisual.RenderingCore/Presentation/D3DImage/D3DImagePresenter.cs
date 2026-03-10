using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;

namespace FastDrawingVisual.Rendering.Presentation
{
    public sealed class D3DImagePresenter : IRenderPresenter
    {
        private readonly D3DImage _d3dImage = new();
        private readonly DrawingVisual _visual = new();
        private IVisualHostElement? _attachedHost;
        private ID3D9PresentationSource? _presentationSource;
        private bool _isBackBufferBound;
        private bool _isDisposed;
        private int _width;
        private int _height;
        private bool _lastReadyState;

        public D3DImagePresenter()
        {
            _d3dImage.IsFrontBufferAvailableChanged += OnFrontBufferAvailableChanged;
            CompositionTarget.Rendering += OnCompositionTargetRendering;
        }

        public bool IsPresentationReady =>
            !_isDisposed &&
            _attachedHost != null &&
            _presentationSource != null;

        public event Action? ReadyStateChanged;

        public bool AttachToElement(ContentControl element, ICapabilityProvider capabilityProvider)
        {
            ThrowIfDisposed();
            if (element == null) throw new ArgumentNullException(nameof(element));
            if (capabilityProvider == null) throw new ArgumentNullException(nameof(capabilityProvider));

            if (element is not IVisualHostElement host)
                return false;

            if (!capabilityProvider.TryGetCapability<ID3D9PresentationSource>(out var presentationSource) || presentationSource == null)
                return false;

            if (ReferenceEquals(_attachedHost, host))
            {
                _presentationSource = presentationSource;
                UpdateReadyState();
                return true;
            }

            DetachFromHost();
            if (!host.AttachVisual(_visual))
                return false;

            _attachedHost = host;
            _presentationSource = presentationSource;
            BindD3DImageToVisual();
            UpdateReadyState();
            return true;
        }

        public void Resize(int width, int height)
        {
            ThrowIfDisposed();
            _width = width;
            _height = height;
            _isBackBufferBound = false;
            BindD3DImageToVisual();
            UpdateReadyState();
        }

        public void Dispose()
        {
            if (_isDisposed)
                return;

            CompositionTarget.Rendering -= OnCompositionTargetRendering;
            _d3dImage.IsFrontBufferAvailableChanged -= OnFrontBufferAvailableChanged;
            UnbindBackBuffer();
            DetachFromHost();
            _presentationSource = null;
            _isDisposed = true;
            UpdateReadyState();
        }

        private void OnCompositionTargetRendering(object? sender, EventArgs e)
        {
            TrySubmitFrame();
        }

        private void TrySubmitFrame()
        {
            if (!IsPresentationReady || _width <= 0 || _height <= 0)
                return;

            if (!_d3dImage.TryLock(new Duration(TimeSpan.Zero)))
            {
                // WPF's D3DImage still requires Unlock after a failed TryLock.
                _d3dImage.Unlock();
                return;
            }

            try
            {
                if (!EnsureBackBufferBound())
                    return;

                if (_presentationSource?.CopyReadyToPresentSurface() != true)
                    return;

                _d3dImage.AddDirtyRect(new Int32Rect(0, 0, _width, _height));
            }
            finally
            {
                _d3dImage.Unlock();
            }
        }

        private bool EnsureBackBufferBound()
        {
            if (_isBackBufferBound)
                return true;

            var surface = _presentationSource?.GetSurface9() ?? IntPtr.Zero;
            if (surface == IntPtr.Zero)
                return false;

            _d3dImage.SetBackBuffer(D3DResourceType.IDirect3DSurface9, surface);
            _isBackBufferBound = true;
            return true;
        }

        private void UnbindBackBuffer()
        {
            if (_isDisposed)
                return;

            _d3dImage.Lock();
            try
            {
                _d3dImage.SetBackBuffer(D3DResourceType.IDirect3DSurface9, IntPtr.Zero);
                _isBackBufferBound = false;
            }
            finally
            {
                _d3dImage.Unlock();
            }
        }

        private void OnFrontBufferAvailableChanged(object sender, DependencyPropertyChangedEventArgs e)
        {
            var available = (bool)e.NewValue;
            _isBackBufferBound = false;

            if (!available)
                UnbindBackBuffer();

            _presentationSource?.NotifyFrontBufferAvailable(available);
            BindD3DImageToVisual();
            UpdateReadyState();
        }

        private void BindD3DImageToVisual()
        {
            if (_attachedHost == null || _width <= 0 || _height <= 0)
                return;

            using var dc = _visual.RenderOpen();
            dc.DrawImage(_d3dImage, new Rect(0, 0, _width, _height));
        }

        private void DetachFromHost()
        {
            if (_attachedHost == null)
                return;

            _attachedHost.DetachVisual(_visual);
            _attachedHost = null;
            _isBackBufferBound = false;
        }

        private void UpdateReadyState()
        {
            var isReady = IsPresentationReady;
            if (_lastReadyState == isReady)
                return;

            _lastReadyState = isReady;
            ReadyStateChanged?.Invoke();
        }

        private void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(D3DImagePresenter));
        }
    }
}
