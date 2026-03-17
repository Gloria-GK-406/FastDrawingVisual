namespace FastDrawingVisual.CommandRuntime
{
    public readonly struct CommandColorArgb8
    {
        public readonly byte A;
        public readonly byte R;
        public readonly byte G;
        public readonly byte B;

        public CommandColorArgb8(byte a, byte r, byte g, byte b)
        {
            A = a;
            R = r;
            G = g;
            B = b;
        }
    }
}
