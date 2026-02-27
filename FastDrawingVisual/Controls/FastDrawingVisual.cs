using FastDrawingVisual.Rendering;
using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace FastDrawingVisual.Controls
{
    /// <summary>
    /// 基于 <see cref="IFastImage"/> 的高性能 WPF 渲染控件。
    /// <para>
    /// 职责：
    ///   1. 通过 <see cref="FastImageFactory"/> 创建最优 <see cref="IFastImage"/> 实现；
    ///   2. 在 Loaded / SizeChanged 时完成初始化与 Resize（DPI 感知）；
    ///   3. 将 <see cref="IFastImage.Source"/> 绘制到 WPF 视觉树；
    ///   4. 将 <see cref="IFastImage.TryOpenRender"/> 暴露给调用方。
    /// </para>
    /// </summary>
    public class FastDrawingVisual : Image, IDisposable
    {
        private IFastImage? _image;
        private bool _isDisposed;

        /// <summary>
        /// 当前图像是否已成功初始化并可以接受绘制请求。
        /// </summary>
        public bool IsReady => _image != null && _image.IsInitialized;

        /// <summary>
        /// 尝试获取一个可立即使用的绘图上下文（透传至底层 <see cref="IFastImage"/>）。
        /// 可在任意线程调用。
        /// </summary>
        /// <returns>
        /// 成功时返回 <see cref="IDrawingContext"/>；
        /// 控件尚未就绪、底层帧忙或设备丢失时返回 <c>null</c>，调用方可跳过本帧。
        /// </returns>
        public IDrawingContext? TryOpenRender()
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(FastDrawingVisual));
            return _image?.TryOpenRender();
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
            if (px <= 0 || py <= 0) return;

            if (_image == null)
            {
                // 首次：通过工厂创建
                _image = FastImageFactory.CreateIFastImage(px, py);
            }
            else if ((int)_image.Width != px || (int)_image.Height != py)
            {
                // 尺寸变化：Resize 复用现有实现
                _image.Resize(px, py);
            }

            this.Source = _image?.Source;

            // 通知 WPF 重新调用 OnRender
            InvalidateVisual();
        }

        private void OnSizeChanged(object sender, SizeChangedEventArgs e)
            => EnsureInitialized();

        protected override Size MeasureOverride(Size availableSize) => availableSize;

        protected override Size ArrangeOverride(Size finalSize) => finalSize;

        /// <summary>
        /// 获取控件的 DPI 感知实际像素尺寸。
        /// </summary>
        private (int width, int height) GetPixelSize()
        {
            var dpi = VisualTreeHelper.GetDpi(this);
            return (
                (int)Math.Round(ActualWidth * dpi.DpiScaleX),
                (int)Math.Round(ActualHeight * dpi.DpiScaleY)
            );
        }


        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            Loaded -= OnLoaded;
            Unloaded -= OnUnloaded;
            SizeChanged -= OnSizeChanged;

            (_image as IDisposable)?.Dispose();
            _image = null;
        }
    }
}
