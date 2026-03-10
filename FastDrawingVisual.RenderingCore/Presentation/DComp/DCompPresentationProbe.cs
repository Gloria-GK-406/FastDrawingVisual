using System;

namespace FastDrawingVisual.Rendering.Presentation
{
    public static class DCompPresentationProbe
    {
        private static readonly Lazy<bool> s_isAvailable = new(CheckAvailable, isThreadSafe: true);

        public static bool IsAvailable => s_isAvailable.Value;

        private static bool CheckAvailable()
        {
            if (Environment.OSVersion.Version.Major < 10)
                return false;

            try
            {
                return WinRTProxyNative.FDV_WinRTProxy_IsReady();
            }
            catch
            {
                return false;
            }
        }
    }
}
