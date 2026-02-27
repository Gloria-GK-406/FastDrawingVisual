using System;
using System.IO;
using System.Runtime.InteropServices;

namespace FastDrawingVisual
{
    /// <summary>
    /// 运行时渲染能力检测。
    /// <para>
    /// 此类仅使用 BCL / Win32 API，不引用 SkiaSharp、SharpDX 或任何 D3D 类型，
    /// 因此可以在任何系统上安全调用，不会触发高版本 DLL 的加载。
    /// </para>
    /// </summary>
    public static class RendererCapability
    {
        private static readonly Lazy<bool> _isAcceleratedAvailable =
            new Lazy<bool>(CheckAccelerated, isThreadSafe: true);

        /// <summary>
        /// 获取当前系统是否支持 D3D11 + SkiaSharp 3.x 加速渲染路径。
        /// 结果在进程生命周期内缓存，仅计算一次。
        /// </summary>
        public static bool IsAcceleratedAvailable => _isAcceleratedAvailable.Value;

        private static bool CheckAccelerated()
        {
            if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                return false;

            var sysDir = Environment.GetFolderPath(Environment.SpecialFolder.System);

            // SkiaSharp 3.116+ 的 libSkiaSharp.dll 对 d3d12.dll 有静态链接依赖
            // Windows 10 (1703+) 起系统自带；Win7/8.1 无此文件
            if (!File.Exists(Path.Combine(sysDir, "d3d12.dll")))
                return false;

            if (!File.Exists(Path.Combine(sysDir, "d3d11.dll")))
                return false;

            if (!File.Exists(Path.Combine(sysDir, "d3d9.dll")))
                return false;

            // OS 版本宽松校验（辅助守卫）
            // 注意：需要应用 manifest 声明兼容 Win10，否则 OSVersion 可能返回 6.2
            var version = Environment.OSVersion.Version;
            if (version.Major < 10)
                return false;

            return true;
        }
    }
}
