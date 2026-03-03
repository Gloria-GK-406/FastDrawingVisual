using System.Configuration;
using System.Data;
using System;
using System.Windows;

namespace FastDrawingVisualApp
{
    /// <summary>
    /// Interaction logic for App.xaml
    /// </summary>
    public partial class App : Application
    {
        protected override void OnStartup(StartupEventArgs e)
        {
            Environment.SetEnvironmentVariable("FDV_RENDERER", "DCompD3D11");
            base.OnStartup(e);
        }
    }

}
