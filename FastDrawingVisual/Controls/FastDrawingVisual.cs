using FastDrawingVisual.Rendering;
using FastDrawingVisual.WpfRenderer;
using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace FastDrawingVisual.Controls
{
    /// <summary>
    /// High performance WPF drawing control.
    /// </summary>
    public class FastDrawingVisual : ContentControl, IVisualHostElement, IDisposable
    {
        private IRenderer? _renderer;
        private Visual? _attachedVisual;
        private SingleVisualHost? _visualHost;
        private bool _isAttached;
        private bool _isDisposed;

        public static readonly DependencyProperty PreferredRendererProperty =
            DependencyProperty.Register(
                nameof(PreferredRenderer),
                typeof(RendererPreference),
                typeof(FastDrawingVisual),
                new PropertyMetadata(RendererPreference.Auto, OnPreferredRendererChanged),
                IsValidPreferredRenderer);

        public bool IsReady => _renderer != null && _isAttached && !_isDisposed;

        public RendererPreference PreferredRenderer
        {
            get => (RendererPreference)GetValue(PreferredRendererProperty);
            set => SetValue(PreferredRendererProperty, value);
        }

        public FastDrawingVisual()
        {
            UseLayoutRounding = true;
            SnapsToDevicePixels = true;
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
            if (_isDisposed)
                return;

            var (px, py) = GetPixelSize();
            if (px <= 0 || py <= 0)
                return;

            if (_renderer == null)
            {
                TryInitializeRenderer(px, py);
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

            if (_visualHost != null && ReferenceEquals(Content, _visualHost))
                Content = null;

            _visualHost = new SingleVisualHost(visual);
            _attachedVisual = visual;
            Content = _visualHost;
            return true;
        }

        public bool DetachVisual(Visual visual)
        {
            if (!ReferenceEquals(_attachedVisual, visual))
                return false;

            if (_visualHost != null && ReferenceEquals(Content, _visualHost))
                Content = null;

            _visualHost?.ClearVisual();
            _visualHost = null;
            _attachedVisual = null;
            _isAttached = false;
            return true;
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            Loaded -= OnLoaded;
            Unloaded -= OnUnloaded;
            SizeChanged -= OnSizeChanged;

            ResetRendererState();
        }

        private static void OnPreferredRendererChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            if (d is FastDrawingVisual visual)
                visual.HandlePreferredRendererChanged();
        }

        private static bool IsValidPreferredRenderer(object value)
            => value is RendererPreference preference && Enum.IsDefined(typeof(RendererPreference), preference);

        private void HandlePreferredRendererChanged()
        {
            if (_isDisposed)
                return;

            ResetRendererState();
            if (IsLoaded)
                EnsureInitialized();
        }

        private void TryInitializeRenderer(int width, int height)
        {
            var renderer = RendererFactory.Create(PreferredRenderer);
            if (TryAttachRenderer(renderer, width, height))
                return;

            if (renderer is WpfFallbackRenderer)
                return;

            TryAttachRenderer(new WpfFallbackRenderer(), width, height);
        }

        private bool TryAttachRenderer(IRenderer renderer, int width, int height)
        {
            ResetRendererState();

            try
            {
                if (!renderer.Initialize(width, height))
                {
                    renderer.Dispose();
                    return false;
                }

                if (!renderer.AttachToElement(this))
                {
                    renderer.Dispose();
                    return false;
                }

                _renderer = renderer;
                _isAttached = true;
                return true;
            }
            catch
            {
                renderer.Dispose();
                return false;
            }
        }

        private void ResetRendererState()
        {
            _renderer?.Dispose();
            _renderer = null;

            if (_visualHost != null && ReferenceEquals(Content, _visualHost))
                Content = null;

            _visualHost?.ClearVisual();
            _visualHost = null;
            _attachedVisual = null;
            _isAttached = false;
        }

        private sealed class SingleVisualHost : FrameworkElement
        {
            private Visual? _visual;

            internal SingleVisualHost(Visual visual)
            {
                _visual = visual ?? throw new ArgumentNullException(nameof(visual));
                AddVisualChild(_visual);
            }

            internal void ClearVisual()
            {
                if (_visual == null)
                    return;

                RemoveVisualChild(_visual);
                _visual = null;
            }

            protected override int VisualChildrenCount => _visual != null ? 1 : 0;

            protected override Visual GetVisualChild(int index)
            {
                if (index != 0 || _visual == null)
                    throw new ArgumentOutOfRangeException(nameof(index));

                return _visual;
            }

            protected override Size MeasureOverride(Size availableSize) => availableSize;

            protected override Size ArrangeOverride(Size finalSize) => finalSize;
        }
    }
}
