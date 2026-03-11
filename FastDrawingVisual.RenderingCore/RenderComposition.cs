using System;
using System.Windows.Controls;

namespace FastDrawingVisual.Rendering
{
    public sealed class RenderComposition : IRenderer
    {
        private readonly IRenderBackend _backend;
        private readonly IRenderPresenter _presenter;
        private readonly IRenderBackendReadiness? _backendReadiness;
        private readonly LatestWinsRenderWorker _worker;
        private int _width;
        private int _height;
        private bool _isInitialized;
        private bool _isFaulted;
        private bool _isDisposed;

        public RenderComposition(IRenderBackend backend, IRenderPresenter presenter)
        {
            _backend = backend ?? throw new ArgumentNullException(nameof(backend));
            _presenter = presenter ?? throw new ArgumentNullException(nameof(presenter));
            _backendReadiness = backend as IRenderBackendReadiness;
            _worker = new LatestWinsRenderWorker(CanExecute, CreateDrawingContext, OnWorkerFault);

            _presenter.ReadyStateChanged += OnReadyStateChanged;
            if (_backendReadiness != null)
                _backendReadiness.ReadyStateChanged += OnReadyStateChanged;
        }

        public bool AttachToElement(ContentControl element)
        {
            ThrowIfDisposed();
            if (!_isInitialized)
                throw new InvalidOperationException("Call Initialize first.");

            var attached = _presenter.AttachToElement(element, _backend);
            if (attached)
                _worker.SignalIfPending();
            return attached;
        }

        public bool Initialize(int width, int height)
        {
            ThrowIfDisposed();
            if (width <= 0 || height <= 0)
                throw new ArgumentException("Width and height must be greater than zero.");

            _width = width;
            _height = height;
            _isFaulted = false;

            if (!_backend.Initialize(width, height))
                return false;

            _presenter.Resize(width, height);
            _isInitialized = true;
            _worker.Start();
            _worker.SignalIfPending();
            return true;
        }

        public void Resize(int width, int height)
        {
            ThrowIfDisposed();
            if (!_isInitialized)
                return;

            if (width <= 0 || height <= 0)
                throw new ArgumentException("Width and height must be greater than zero.");

            _width = width;
            _height = height;
            _backend.Resize(width, height);
            _presenter.Resize(width, height);
            _worker.SignalIfPending();
        }

        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            ThrowIfDisposed();
            if (!_isInitialized)
                throw new InvalidOperationException("Call Initialize first.");
            if (_isFaulted || drawAction == null)
                return;

            _worker.Submit(drawAction);
        }

        public void Dispose()
        {
            if (_isDisposed)
                return;

            _isDisposed = true;
            _worker.Dispose();
            _presenter.ReadyStateChanged -= OnReadyStateChanged;
            if (_backendReadiness != null)
                _backendReadiness.ReadyStateChanged -= OnReadyStateChanged;
            _presenter.Dispose();
            _backend.Dispose();
        }

        private IDrawingContext? CreateDrawingContext()
        {
            return _backend.CreateDrawingContext(_width, _height);
        }

        private bool CanExecute()
        {
            if (_isDisposed || !_isInitialized || _isFaulted)
                return false;

            if (!_presenter.IsPresentationReady)
                return false;

            return _backendReadiness?.IsReadyForRendering ?? true;
        }

        private void OnWorkerFault(Exception _)
        {
            _isFaulted = true;
        }

        private void OnReadyStateChanged()
        {
            if (CanExecute())
                _worker.SignalIfPending();
        }

        private void ThrowIfDisposed()
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(RenderComposition));
        }
    }
}
