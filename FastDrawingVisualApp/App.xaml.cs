using System.Runtime.InteropServices;
using System.Windows;

namespace FastDrawingVisualApp
{
    /// <summary>
    /// Interaction logic for App.xaml
    /// </summary>
    public partial class App : Application
    {
        private const uint TimerResolutionMs = 1;
        private static bool _timerResolutionRaised;

        protected override void OnStartup(StartupEventArgs e)
        {
            _timerResolutionRaised = NativeMethods.timeBeginPeriod(TimerResolutionMs) == 0;
            base.OnStartup(e);
        }

        protected override void OnExit(ExitEventArgs e)
        {
            if (_timerResolutionRaised)
            {
                NativeMethods.timeEndPeriod(TimerResolutionMs);
                _timerResolutionRaised = false;
            }

            base.OnExit(e);
        }

        private static class NativeMethods
        {
            [DllImport("winmm.dll", ExactSpelling = true)]
            internal static extern uint timeBeginPeriod(uint uPeriod);

            [DllImport("winmm.dll", ExactSpelling = true)]
            internal static extern uint timeEndPeriod(uint uPeriod);
        }
    }

}
