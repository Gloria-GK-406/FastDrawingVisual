using FastDrawingVisual.Rendering;
using System;

namespace FastDrawingVisual.Controls
{
    /// <summary>
    /// <see cref="IFastImage"/> 工厂。
    /// 按照能力优先级依次尝试创建最优实现，若高级实现不可用则自动降级。
    /// </summary>
    /// <remarks>
    /// 当前降级链：
    ///   D3D9（D3DFastImage）→ 将来可扩展为 Software（WriteableBitmap）
    /// </remarks>
    public static class FastImageFactory
    {
        /// <summary>
        /// 创建当前环境下最优的 <see cref="IFastImage"/> 实现。
        /// </summary>
        /// <param name="width">初始像素宽度。</param>
        /// <param name="height">初始像素高度。</param>
        /// <returns>
        /// 成功初始化的 <see cref="IFastImage"/> 实例；
        /// 若所有实现均初始化失败则返回 <c>null</c>。
        /// </returns>
        public static IFastImage? CreateIFastImage(int width, int height)
        {
            if (width <= 0 || height <= 0)
                throw new ArgumentException($"宽高必须大于 0，当前值：{width}x{height}。");

            // ── 第一优先级：D3D9 硬件加速路径 ──────────────────────────────
            if (TryCreate<D3DFastImage>(width, height, out var d3d))
                return d3d;

            // ── 第二优先级：Software 降级（保留扩展点，暂未实现）─────────────
            // if (TryCreate<SoftwareFastImage>(width, height, out var sw))
            //     return sw;

            return null;
        }

        // ─────────────────────────────────────────────────────────────────────
        // 私有辅助
        // ─────────────────────────────────────────────────────────────────────

        private static bool TryCreate<T>(int width, int height, out IFastImage? result)
            where T : IFastImage, new()
        {
            try
            {
                var instance = new T();
                if (instance.Initialize(width, height))
                {
                    result = instance;
                    return true;
                }

                // Initialize 返回 false，需要释放
                (instance as IDisposable)?.Dispose();
            }
            catch (Exception)
            {
                // 当前实现不可用，静默降级
            }

            result = null;
            return false;
        }
    }
}
