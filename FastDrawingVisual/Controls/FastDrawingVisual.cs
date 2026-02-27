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
        /// 向内部调度器提交一个绘制委托（透传至底层 <see cref="IFastImage"/>）。
        /// 内部 DrawingWorker 将在下一个与 WPF VSync 对齐的绘制窗口执行该委托。
        /// 可在任意线程调用，线程安全。
        /// </summary>
        /// <param name="drawAction">
        /// 绘制逻辑委托；在后台绘制线程执行，请勿访问 UI 元素。
        /// 控件未就绪时调用为空操作（不抛出异常）。
        /// </param>
        public void SubmitDrawing(Action<IDrawingContext> drawAction)
        {
            if (_isDisposed) throw new ObjectDisposedException(nameof(FastDrawingVisual));
            if (_image == null || !_image.IsInitialized) return; // 未就绪时静默丢弃
            _image.SubmitDrawing(drawAction);
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
