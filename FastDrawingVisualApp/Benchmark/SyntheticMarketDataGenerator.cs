using System;

namespace FastDrawingVisualApp.Benchmark
{
    internal static class SyntheticMarketDataGenerator
    {
        public static SyntheticMarketSeries Create(int count, int seed)
        {
            var random = new Random(seed);
            var bars = new SyntheticBar[count];
            var closes = new double[count];
            var volumes = new double[count];

            double price = 120;
            double trend = 0.018;

            for (int i = 0; i < count; i++)
            {
                if ((i % 512) == 0)
                    trend = (random.NextDouble() - 0.5) * 0.18;

                double harmonic = Math.Sin(i * 0.017) * 1.8 + Math.Cos(i * 0.006) * 1.1;
                double noise = (random.NextDouble() - 0.5) * 2.4;
                double spike = (i % 97 == 0) ? (random.NextDouble() - 0.5) * 10 : 0;
                double open = price;
                double close = Math.Max(4, open + trend + harmonic * 0.08 + noise + spike);
                double high = Math.Max(open, close) + 0.5 + random.NextDouble() * 2.5 + Math.Abs(spike) * 0.2;
                double low = Math.Min(open, close) - 0.5 - random.NextDouble() * 2.3 - Math.Abs(spike) * 0.15;
                double volume = 600 + Math.Abs(close - open) * 2_400 + Math.Abs(harmonic) * 180 + random.NextDouble() * 900;

                bars[i] = new SyntheticBar(open, high, low, close, volume);
                closes[i] = close;
                volumes[i] = volume;
                price = close;
            }

            return new SyntheticMarketSeries(
                bars,
                CalculateSimpleMovingAverage(closes, 8),
                CalculateSimpleMovingAverage(closes, 21),
                CalculateSimpleMovingAverage(closes, 55),
                CalculateOscillator(closes),
                CalculateSimpleMovingAverage(volumes, 12));
        }

        private static double[] CalculateSimpleMovingAverage(double[] values, int period)
        {
            var output = new double[values.Length];
            double sum = 0;

            for (int i = 0; i < values.Length; i++)
            {
                sum += values[i];
                if (i >= period)
                    sum -= values[i - period];

                int divisor = Math.Min(i + 1, period);
                output[i] = sum / divisor;
            }

            return output;
        }

        private static double[] CalculateOscillator(double[] closes)
        {
            var output = new double[closes.Length];

            for (int i = 0; i < closes.Length; i++)
            {
                double fast = i > 4 ? closes[i] - closes[i - 4] : 0;
                double slow = i > 16 ? closes[i] - closes[i - 16] : fast;
                double baseLine = slow == 0 ? 1 : Math.Abs(slow);
                output[i] = Math.Max(-1.2, Math.Min(1.2, fast / baseLine));
            }

            return output;
        }
    }

    internal readonly record struct SyntheticBar(double Open, double High, double Low, double Close, double Volume);

    internal sealed class SyntheticMarketSeries
    {
        public SyntheticMarketSeries(
            SyntheticBar[] bars,
            double[] maFast,
            double[] maMid,
            double[] maSlow,
            double[] oscillator,
            double[] volumeAverage)
        {
            Bars = bars;
            MaFast = maFast;
            MaMid = maMid;
            MaSlow = maSlow;
            Oscillator = oscillator;
            VolumeAverage = volumeAverage;
        }

        public SyntheticBar[] Bars { get; }

        public double[] MaFast { get; }

        public double[] MaMid { get; }

        public double[] MaSlow { get; }

        public double[] Oscillator { get; }

        public double[] VolumeAverage { get; }
    }
}
