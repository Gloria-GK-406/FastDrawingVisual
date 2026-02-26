using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Media;

namespace FastDrawingVisual.Rendering
{
    /// <summary>
    /// 表示可用于快速渲染的图像抽象。实现者应提供底层图像源、尺寸信息以及用于渲染的绘图上下文。
    /// </summary>
    public interface IFastImage
    {
        /// <summary>
        /// 获取用于在 WPF 中显示或绘制的图像源。
        /// </summary>
        ImageSource Source { get; }

        /// <summary>
        /// 获取图像资源是否已成功初始化。
        /// </summary>
        bool IsInitialized { get; }

        /// <summary>
        /// 获取图像的像素宽度（或呈现时采用的宽度）。
        /// </summary>
        double Width { get; }

        /// <summary>
        /// 获取图像的像素高度（或呈现时采用的高度）。
        /// </summary>
        double Height { get; }

        /// <summary>
        /// 尝试打开一个用于执行绘制操作的 <see cref="IDrawingContext"/> 实例。
        /// 调用方应在完成绘制后正确释放或关闭该上下文。
        /// </summary>
        /// <returns>
        /// 成功时返回 <see cref="IDrawingContext"/> 对象；
        /// 当前无可用渲染帧（所有帧均忙）或设备丢失时返回 <c>null</c>，调用方可跳过本帧稍后重试。
        /// </returns>
        IDrawingContext? TryOpenRender();

        /// <summary>
        /// 初始化图像的内部资源，使其具有指定的像素宽度和高度。
        /// </summary>
        /// <param name="width">目标像素宽度。应为正整数。</param>
        /// <param name="height">目标像素高度。应为正整数。</param>
        /// <returns>如果初始化成功则返回 <c>true</c>；如果失败则返回 <c>false</c>。</returns>
        bool Initialize(int width, int height);

        /// <summary>
        /// 调整图像的内部缓冲区或资源以匹配指定的像素宽度和高度。
        /// 实现应在必要时重新分配或重新配置底层图像资源以反映新尺寸。
        /// </summary>
        /// <param name="width">新的目标像素宽度。应为正整数。</param>
        /// <param name="height">新的目标像素高度。应为正整数。</param>
        void Resize(int width, int height);
    }
}
