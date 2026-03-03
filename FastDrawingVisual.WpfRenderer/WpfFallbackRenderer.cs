using FastDrawingVisual.Rendering;
using System;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace FastDrawingVisual.WpfRenderer
{
    public sealed class WpfFallbackRenderer : IRenderer
    {
        private readonly DrawingVisual _visual;
        private readonly Dispatcher _uiDispatcher;

        private IVisualHostElement? _attachedHost;
        private volatile Action<IDrawingContext>? _pendingDrawAction;
        private int _callbackQueued;

        private int _width;
        private int _height;
        private bool _isInitialized;
        private bool _isDisposed;

        public WpfFallbackRenderer()
        {
            _uiDispatcher = Dispatcher.CurrentDispatcher;
            _visual = new DrawingVisual();
        }

        public bool AttachToElement(ContentControl element)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(WpfFallbackRenderer));
            if (element == null) throw new ArgumentNullException(nameof(element));

            if (element is not IVisualHostElement host)
                return false;

            if (ReferenceEquals(_attachedHost, host))
                return true;

            DetachFromHost();
            if (!host.AttachVisual(_visual))
                return false;

            _attachedHost = host;
            return true;
        }

        public bool Initialize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(WpfFallbackRenderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");

            _width = width;
            _height = height;
            _isInitialized = true;
            return true;
        }

        public void Resize(int width, int height)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(WpfFallbackRenderer));
            if (width <= 0 || height <= 0) throw new ArgumentException("Width and height must be greater than zero.");

            _width = width;
            _height = height;
        }

        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(WpfFallbackRenderer));
            if (!_isInitialized) throw new InvalidOperationException("Call Initialize first.");
            if (drawAction == null) throw new ArgumentNullException(nameof(drawAction));

            Interlocked.Exchange(ref _pendingDrawAction, drawAction);

            if (Interlocked.CompareExchange(ref _callbackQueued, 1, 0) == 0)
                _uiDispatcher.InvokeAsync(ExecutePendingDraw, DispatcherPriority.Render);
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;
            DetachFromHost();
        }

        private void ExecutePendingDraw()
        {
            Interlocked.Exchange(ref _callbackQueued, 0);

            if (_isDisposed || !_isInitialized)
                return;

            var action = Interlocked.Exchange(ref _pendingDrawAction, null);
            if (action == null)
                return;

            using var dc = _visual.RenderOpen();
            using var ctx = new WpfDrawingContext(dc, _width, _height);
            action(ctx);
        }

        private void DetachFromHost()
        {
            if (_attachedHost == null)
                return;

            _attachedHost.DetachVisual(_visual);
            _attachedHost = null;
        }
    }
}
