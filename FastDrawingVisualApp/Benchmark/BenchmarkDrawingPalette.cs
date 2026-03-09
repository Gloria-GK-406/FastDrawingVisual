using System.Windows;
using System.Windows.Media;

namespace FastDrawingVisualApp.Benchmark
{
    internal static class BenchmarkDrawingPalette
    {
        public static readonly SolidColorBrush CanvasBackground = Freeze(new SolidColorBrush(Color.FromRgb(0x10, 0x14, 0x1B)));
        public static readonly SolidColorBrush PanelBackground = Freeze(new SolidColorBrush(Color.FromRgb(0x16, 0x1D, 0x26)));
        public static readonly SolidColorBrush GridBrush = Freeze(new SolidColorBrush(Color.FromArgb(0x34, 0x8C, 0xA3, 0xB3)));
        public static readonly SolidColorBrush BullBrush = Freeze(new SolidColorBrush(Color.FromRgb(0x3E, 0xC5, 0x9F)));
        public static readonly SolidColorBrush BearBrush = Freeze(new SolidColorBrush(Color.FromRgb(0xF0, 0x6B, 0x6B)));
        public static readonly SolidColorBrush AccentBrush = Freeze(new SolidColorBrush(Color.FromRgb(0xFF, 0xB4, 0x54)));
        public static readonly SolidColorBrush SecondaryAccentBrush = Freeze(new SolidColorBrush(Color.FromRgb(0x55, 0xC7, 0xF7)));
        public static readonly SolidColorBrush TertiaryAccentBrush = Freeze(new SolidColorBrush(Color.FromRgb(0xBF, 0x8B, 0xFF)));
        public static readonly SolidColorBrush NeutralBrush = Freeze(new SolidColorBrush(Color.FromRgb(0xD4, 0xDB, 0xE4)));
        public static readonly SolidColorBrush MutedBrush = Freeze(new SolidColorBrush(Color.FromRgb(0x78, 0x88, 0x99)));
        public static readonly SolidColorBrush VolumeBrush = Freeze(new SolidColorBrush(Color.FromRgb(0x4E, 0x89, 0xD8)));

        public static readonly Pen GridPen = Freeze(new Pen(GridBrush, 1));
        public static readonly Pen BullPen = Freeze(new Pen(BullBrush, 1));
        public static readonly Pen BearPen = Freeze(new Pen(BearBrush, 1));
        public static readonly Pen AccentPen = Freeze(new Pen(AccentBrush, 1.6));
        public static readonly Pen SecondaryAccentPen = Freeze(new Pen(SecondaryAccentBrush, 1.4));
        public static readonly Pen TertiaryAccentPen = Freeze(new Pen(TertiaryAccentBrush, 1.2));
        public static readonly Pen MutedPen = Freeze(new Pen(MutedBrush, 1));
        public static readonly Pen NeutralPen = Freeze(new Pen(NeutralBrush, 1));

        private static T Freeze<T>(T freezable) where T : Freezable
        {
            freezable.Freeze();
            return freezable;
        }
    }
}
