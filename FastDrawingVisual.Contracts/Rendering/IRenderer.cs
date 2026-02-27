using System;
using System.Windows.Media;

namespace FastDrawingVisual.Rendering
{
    /// <summary>
    /// 渲染器抽象接口。每个实现负责：
    /// <list type="bullet">
    ///   <item>提供供 <see cref="Controls.FastDrawingVisual"/> 托管的 <see cref="DrawingVisual"/> 节点；</item>
    ///   <item>自主管理绘制任务的调度与帧呈现时序；</item>
    ///   <item>响应宿主控件传入的尺寸变化。</item>
    /// </list>
    /// </summary>
    /// <remarks>
    /// <para>
    /// 实现者职责：<br/>
    ///   • <see cref="Initialize"/> 在首次获得有效像素尺寸时调用（UI 线程）；<br/>
    ///   • <see cref="Resize"/> 在宿主尺寸变化时调用（UI 线程）；<br/>
    ///   • <see cref="SubmitDrawing"/> 可在任意线程调用，内部应为线程安全。
    /// </para>
    /// <para>
    /// 关于 <see cref="Visual"/>：<br/>
    ///   宿主在 <see cref="Initialize"/> 成功后将 <see cref="Visual"/> AddVisualChild 进视觉树。
    ///   <see cref="Visual"/> 的内容由实现者自主更新（不同实现的更新方式各异）。
    /// </para>
    /// </remarks>
    public interface IRenderer : IDisposable
    {
        /// <summary>
        /// 提供给 <see cref="Controls.FastDrawingVisual"/> 托管的视觉节点。
        /// <para>
        /// D3D/Skia 实现：内部含 <see cref="System.Windows.Media.Imaging.BitmapSource"/>（D3DImage）的 ImageDrawing，
        /// 帧更新通过 D3DImage.Lock/AddDirtyRect/Unlock 完成，DrawingVisual 内容不变。<br/>
        /// WPF 降级实现：直接作为绘制目标，每帧 RenderOpen 替换内容。
        /// </para>
        /// </summary>
        DrawingVisual Visual { get; }

        /// <summary>
        /// 初始化渲染器（首次调用，在 UI 线程）。
        /// </summary>
        /// <param name="width">像素宽度（DPI 感知）。</param>
        /// <param name="height">像素高度（DPI 感知）。</param>
        /// <returns>成功返回 <c>true</c>；资源分配失败返回 <c>false</c>（宿主将降级）。</returns>
        bool Initialize(int width, int height);

        /// <summary>
        /// 响应宿主控件的尺寸变化（UI 线程）。
        /// 实现应重建相关 GPU/Skia 资源以匹配新尺寸。
        /// </summary>
        void Resize(int width, int height);

        /// <summary>
        /// 提交绘制委托（可在任意线程调用，线程安全）。
        /// <para>
        /// Replace 语义：若上一个委托尚未执行，新委托将原子替换旧委托（新委托是旧委托的后继，丢弃安全）。
        /// </para>
        /// </summary>
        /// <param name="drawAction">绘制逻辑委托；在实现者管理的线程上执行，请勿访问 UI 元素。</param>
        void SubmitDrawing(Action<IDrawingContext> drawAction);
    }
}
