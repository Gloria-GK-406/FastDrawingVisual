using FastDrawingVisual.Rendering;
using FastDrawingVisual.Rendering.Composition;
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace FastDrawingVisual.Controls
{
    /// <summary>
    /// DComp 渲染控件骨架。通过独立 HWND 承载后端，后续可接入真实 DirectComposition 视觉树。
    /// </summary>
    public sealed class FastDrawingVisualDComp : HwndHost
    {
        private const int WsChild = unchecked((int)0x40000000);
        private const int WsVisible = unchecked((int)0x10000000);
        private const int WsClipSiblings = unchecked((int)0x04000000);
        private const int WsClipChildren = unchecked((int)0x02000000);

        private ICompositionRenderer? _renderer;
        private IntPtr _hostHwnd;
        private bool _isInitialized;
        private bool _isDisposed;

        public static readonly DependencyProperty PreferredBackendProperty =
            DependencyProperty.Register(
                nameof(PreferredBackend),
                typeof(GraphicsBackendKind),
                typeof(FastDrawingVisualDComp),
                new FrameworkPropertyMetadata(GraphicsBackendKind.Auto));

        /// <summary>
        /// 指定后端优先级。默认 Auto（D3D11 -> D3D12 -> D3D9）。
        /// </summary>
        public GraphicsBackendKind PreferredBackend
        {
            get => (GraphicsBackendKind)GetValue(PreferredBackendProperty);
            set => SetValue(PreferredBackendProperty, value);
        }

        public bool IsReady => _isInitialized && !_isDisposed && _renderer != null;

        public FastDrawingVisualDComp()
        {
            Loaded += OnLoaded;
            Unloaded += OnUnloaded;
            SizeChanged += OnSizeChanged;
        }

        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(FastDrawingVisualDComp));
            if (!IsReady || drawAction == null) return;
            _renderer!.SubmitDrawing(drawAction);
        }

        protected override void Dispose(bool disposing)
        {
            if (!_isDisposed)
            {
                _isDisposed = true;

                Loaded -= OnLoaded;
                Unloaded -= OnUnloaded;
                SizeChanged -= OnSizeChanged;

                _renderer?.Dispose();
                _renderer = null;
                _isInitialized = false;
            }

            base.Dispose(disposing);
        }

        protected override HandleRef BuildWindowCore(HandleRef hwndParent)
        {
            if (_isDisposed)
                throw new ObjectDisposedException(nameof(FastDrawingVisualDComp));

            _hostHwnd = CreateWindowEx(
                0,
                "static",
                string.Empty,
                WsChild | WsVisible | WsClipSiblings | WsClipChildren,
                0,
                0,
                1,
                1,
                hwndParent.Handle,
                IntPtr.Zero,
                IntPtr.Zero,
                IntPtr.Zero);

            if (_hostHwnd == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Failed to create host HWND for FastDrawingVisualDComp.");

            EnsureInitialized();
            return new HandleRef(this, _hostHwnd);
        }

        protected override void DestroyWindowCore(HandleRef hwnd)
        {
            _renderer?.Dispose();
            _renderer = null;
            _isInitialized = false;

            if (hwnd.Handle != IntPtr.Zero)
                DestroyWindow(hwnd.Handle);

            _hostHwnd = IntPtr.Zero;
        }

        protected override void OnWindowPositionChanged(Rect rcBoundingBox)
        {
            base.OnWindowPositionChanged(rcBoundingBox);

            if (_hostHwnd != IntPtr.Zero)
            {
                SetWindowPos(
                    _hostHwnd,
                    IntPtr.Zero,
                    0,
                    0,
                    Math.Max(1, (int)rcBoundingBox.Width),
                    Math.Max(1, (int)rcBoundingBox.Height),
                    0);
            }

            EnsureInitialized();
        }

        private void OnLoaded(object sender, RoutedEventArgs e) => EnsureInitialized();

        private void OnUnloaded(object sender, RoutedEventArgs e)
        {
            _renderer?.Dispose();
            _renderer = null;
            _isInitialized = false;
        }

        private void OnSizeChanged(object sender, SizeChangedEventArgs e) => EnsureInitialized();

        private void EnsureInitialized()
        {
            if (_isDisposed) return;
            if (_hostHwnd == IntPtr.Zero) return;

            var (px, py) = GetPixelSize();
            if (px <= 0 || py <= 0) return;

            if (_renderer == null)
            {
                _renderer = DCompRendererFactory.Create(PreferredBackend);
                _isInitialized = _renderer.Initialize(_hostHwnd, px, py);
                if (!_isInitialized)
                {
                    _renderer.Dispose();
                    _renderer = null;
                }
            }
            else if (_isInitialized)
            {
                _renderer.Resize(px, py);
            }
        }

        private (int width, int height) GetPixelSize()
        {
            var dpi = VisualTreeHelper.GetDpi(this);
            return (
                (int)Math.Round(ActualWidth * dpi.DpiScaleX),
                (int)Math.Round(ActualHeight * dpi.DpiScaleY));
        }

        [DllImport("user32.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern IntPtr CreateWindowEx(
            int dwExStyle,
            string lpClassName,
            string lpWindowName,
            int dwStyle,
            int x,
            int y,
            int nWidth,
            int nHeight,
            IntPtr hWndParent,
            IntPtr hMenu,
            IntPtr hInstance,
            IntPtr lpParam);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool DestroyWindow(IntPtr hWnd);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool SetWindowPos(
            IntPtr hWnd,
            IntPtr hWndInsertAfter,
            int x,
            int y,
            int cx,
            int cy,
            uint uFlags);
    }
}
