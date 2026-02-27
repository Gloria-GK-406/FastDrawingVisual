using System;

namespace FastDrawingVisual.Rendering.D3D
{
    /// <summary>
    /// 渲染帧池。管理三个 <see cref="RenderFrame"/>，
    /// 通过状态协调实现三重缓冲：
    ///   - 一帧供渲染线程绘制（Drawing）
    ///   - 一帧等待 WPF 呈现（ReadyForPresent）
    ///   - 一帧正被 WPF 引用（Presenting）
    /// </summary>
    internal sealed class RenderFramePool : IDisposable
    {
        private const int FrameCount = 3;

        private readonly RenderFrame[] _frames;

        // 用于保护 ReadyForPresent 唯一性 和 Presenting 交换的锁
        // 两个方法都很短，不会形成竞争瓶颈
        private readonly object _lock = new object();

        // 快速路径标志：避免 Rendering 回调在无新帧时进入锁
        private volatile bool _hasReadyFrame;

        private bool _isDisposed;

        public RenderFramePool(D3DDeviceManager deviceManager)
        {
            if (deviceManager == null) throw new ArgumentNullException(nameof(deviceManager));

            _frames = new RenderFrame[FrameCount];
            for (int i = 0; i < FrameCount; i++)
                _frames[i] = new RenderFrame(deviceManager, OnFrameDrawingComplete);
        }

        /// <summary>为所有帧创建（或重建）GPU 资源。</summary>
        public void CreateResources(int width, int height)
        {
            if (width <= 0 || height <= 0)
                throw new ArgumentException("宽高必须大于 0。");

            lock (_lock)
            {
                _hasReadyFrame = false;
                foreach (var frame in _frames)
                    frame.CreateResources(width, height);
            }
        }

        /// <summary>释放所有帧的 GPU 资源（设备丢失时调用）。</summary>
        public void ReleaseResources()
        {
            lock (_lock)
            {
                _hasReadyFrame = false;
                foreach (var frame in _frames)
                    frame.ReleaseResources();
            }
        }

        /// <summary>
        /// 获取一个可用于绘制的帧，并原子性地将其从 Ready → Drawing。
        /// 若所有帧都忙则返回 null（调用方可稍后重试或跳过本帧）。
        /// </summary>
        public RenderFrame? AcquireForDrawing()
        {
            // 无需锁：TryTransitionTo 使用 CAS，天然原子
            foreach (var frame in _frames)
            {
                if (frame.TryTransitionTo(FrameState.Ready, FrameState.Drawing))
                    return frame;
            }
            return null;
        }

        /// <summary>
        /// 由 <see cref="RenderFrame"/> 在画布关闭后回调。
        /// 保证同一时刻最多只有一帧处于 ReadyForPresent，
        /// 新帧到来时旧的 ReadyForPresent 帧被抢占回 Ready（帧丢弃策略，始终呈现最新帧）。
        /// </summary>
        private void OnFrameDrawingComplete(RenderFrame completedFrame)
        {
            lock (_lock)
            {
                // 将其他 ReadyForPresent 帧抢占回 Ready（最多只有一个）
                foreach (var frame in _frames)
                {
                    if (frame != completedFrame && frame.State == FrameState.ReadyForPresent)
                    {
                        frame.ForceSetState(FrameState.Ready);
                        break;
                    }
                }

                // 将完成的帧推进到 ReadyForPresent
                completedFrame.ForceSetState(FrameState.ReadyForPresent);
                _hasReadyFrame = true;
            }
        }

        public void MarkPresentedFrameAsReady(RenderFrame presentingFrame)
        {
            lock (_lock)
            {
                //当前可能有两个presenting帧，一个是正在present的，一个是上次present但还未被抢占回ready的
                //实际上这里需要操作修改的可能只有一个，但为了保险起见，还是遍历所有帧进行状态检查和修改
                _frames.Where(frame=>frame != presentingFrame && frame.State == FrameState.Presenting)
                    .ToList()
                    .ForEach(frame => frame.ForceSetState(FrameState.Ready));
            }
        }

        public void ResetToReadyForPresent(RenderFrame frame)
        {
            frame.ForceSetState(FrameState.ReadyForPresent);
             _hasReadyFrame = true;
        }

        /// <summary>
        /// 尝试获取一个待呈现的帧（ReadyForPresent → Presenting）。
        /// 同时将上一个 Presenting 帧归还为 Ready（因为 SetBackBuffer 后 WPF 会释放旧表面引用）。
        /// 无新帧时返回 null。
        /// 仅应在 UI 线程（CompositionTarget.Rendering 回调）调用。
        /// </summary>
        public RenderFrame? TryAcquireForPresent()
        {
            // 快速路径：无新帧，不进锁
            if (!_hasReadyFrame)
                return null;

            lock (_lock)
            {
                // 双重检查
                if (!_hasReadyFrame)
                    return null;

                RenderFrame? readyFrame = _frames.Where(frame => frame.State == FrameState.ReadyForPresent).FirstOrDefault();

                if (readyFrame == null)
                {
                    _hasReadyFrame = false;
                    return null;
                }

                // 新帧进入 Presenting
                readyFrame.ForceSetState(FrameState.Presenting);
                _hasReadyFrame = false;

                return readyFrame;
            }
        }

        public void Dispose()
        {
            if (_isDisposed) return;
            _isDisposed = true;

            foreach (var frame in _frames)
                frame.Dispose();
        }
    }
}
