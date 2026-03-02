using FastDrawingVisual.Rendering;
using System;
using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisual.Controls
{
    /// <summary>
    /// High performance WPF drawing control.
    /// </summary>
    public class FastDrawingVisual : FrameworkElement, IVisualHostElement, IDisposable
    {
        private IRenderer? _renderer;
        private Visual? _attachedVisual;
        private bool _isAttached;
        private bool _isDisposed;

        public bool IsReady => _renderer != null && _isAttached && !_isDisposed;

        protected override int VisualChildrenCount => _attachedVisual != null ? 1 : 0;

        protected override Visual GetVisualChild(int index)
        {
            if (index != 0 || _attachedVisual == null)
                throw new ArgumentOutOfRangeException(nameof(index));
            return _attachedVisual;
        }

        public FastDrawingVisual()
        {
            Loaded += OnLoaded;
            Unloaded += OnUnloaded;
        }

        private void OnLoaded(object sender, RoutedEventArgs e)
        {
            SizeChanged += OnSizeChanged;
            EnsureInitialized();
        }

        private void OnUnloaded(object sender, RoutedEventArgs e)
        {
            SizeChanged -= OnSizeChanged;
        }

        private void EnsureInitialized()
        {
            var (px, py) = GetPixelSize();
            if (px <= 0 || py <= 0)
                return;

            if (_renderer == null)
            {
                _renderer = RendererFactory.Create();
                if (_renderer.Initialize(px, py))
                {
                    _isAttached = _renderer.AttachToElement(this);
                    if (!_isAttached)
                    {
                        _renderer.Dispose();
                        _renderer = null;
                    }
                }
            }
            else if (_isAttached)
            {
                _renderer.Resize(px, py);
            }
        }

        private void OnSizeChanged(object sender, SizeChangedEventArgs e)
            => EnsureInitialized();

        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(FastDrawingVisual));
            if (!IsReady) return;
            _renderer!.SubmitDrawing(drawAction);
        }

        protected override Size MeasureOverride(Size availableSize)
            => availableSize;

        protected override Size ArrangeOverride(Size finalSize)
        {
            return finalSize;
        }

        private (int width, int height) GetPixelSize()
        {
            var dpi = VisualTreeHelper.GetDpi(this);
            return (
                (int)Math.Round(ActualWidth * dpi.DpiScaleX),
                (int)Math.Round(ActualHeight * dpi.DpiScaleY));
        }

        public bool AttachVisual(Visual visual)
        {
            if (_isDisposed || visual == null)
                return false;

            if (ReferenceEquals(_attachedVisual, visual))
                return true;

            if (_attachedVisual != null)
                RemoveVisualChild(_attachedVisual);

            _attachedVisual = visual;
            AddVisualChild(visual);
            InvalidateMeasure();
            InvalidateArrange();
            InvalidateVisual();
            return true;
        }

        public bool DetachVisual(Visual visual)
        {
            if (!ReferenceEquals(_attachedVisual, visual) || _attachedVisual == null)
                return false;

            RemoveVisualChild(_attachedVisual);
            _attachedVisual = null;
            _isAttached = false;
            InvalidateMeasure();
            InvalidateArrange();
            InvalidateVisual();
            return true;
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            Loaded -= OnLoaded;
            Unloaded -= OnUnloaded;
            SizeChanged -= OnSizeChanged;

            _renderer?.Dispose();
            _renderer = null;

            if (_attachedVisual != null)
            {
                RemoveVisualChild(_attachedVisual);
                _attachedVisual = null;
            }

            _isAttached = false;
        }
    }
}
