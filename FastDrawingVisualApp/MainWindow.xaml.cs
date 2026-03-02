using FastDrawingVisual.Rendering;
using System;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;

namespace FastDrawingVisualApp
{
    public partial class MainWindow : Window
    {
        private double _animPhase = 0;
        private int _frameCount = 0;
        private DateTime _lastFpsTime = DateTime.Now;
        private double _fps = 0;

        // 预冻结画刷/笔（跨线程安全）
        private static readonly SolidColorBrush _bgBrush = Freeze(new SolidColorBrush(Color.FromRgb(0x18, 0x18, 0x25)));
        private static readonly SolidColorBrush _gridBrush = Freeze(new SolidColorBrush(Color.FromArgb(0x28, 0xCD, 0xD6, 0xF4)));
        private static readonly SolidColorBrush _purpleBrush = Freeze(new SolidColorBrush(Color.FromRgb(0xCB, 0xA6, 0xF7)));
        private static readonly SolidColorBrush _pinkBrush = Freeze(new SolidColorBrush(Color.FromRgb(0xF3, 0x8B, 0xA8)));
        private static readonly SolidColorBrush _blueBrush = Freeze(new SolidColorBrush(Color.FromRgb(0x89, 0xB4, 0xFA)));
        private static readonly SolidColorBrush _greenBrush = Freeze(new SolidColorBrush(Color.FromRgb(0xA6, 0xE3, 0xA1)));
        private static readonly SolidColorBrush _yellowBrush = Freeze(new SolidColorBrush(Color.FromRgb(0xF9, 0xE2, 0xAF)));
        private static readonly SolidColorBrush _tealBrush = Freeze(new SolidColorBrush(Color.FromRgb(0x94, 0xE2, 0xD5)));
        private static readonly SolidColorBrush _whiteBrush = Freeze(new SolidColorBrush(Colors.White));
        private static readonly SolidColorBrush _dimBrush = Freeze(new SolidColorBrush(Color.FromArgb(0xCC, 0x6C, 0x70, 0x86)));

        private static readonly Pen _gridPen = Freeze(new Pen(_gridBrush, 1));
        private static readonly Pen _purplePen = Freeze(new Pen(_purpleBrush, 2));
        private static readonly Pen _pinkPen = Freeze(new Pen(_pinkBrush, 2));
        private static readonly Pen _bluePen = Freeze(new Pen(_blueBrush, 2));
        private static readonly Pen _whitePen = Freeze(new Pen(_whiteBrush, 1.5));

        private readonly PeriodicTimer _timer;

        public MainWindow()
        {
            InitializeComponent();

            FastCanvas.Loaded += (_, _) => UpdateStatus(true);
            _timer = new PeriodicTimer(TimeSpan.FromMilliseconds(8));

            // 动画定时器，约 60fps
            Task.Run(OnAnimTick);
        }

        private async Task OnAnimTick()
        {
            while (await _timer.WaitForNextTickAsync())
            {
                _animPhase += 0.04;

                // 将本帧绘制逻辑提交给内部调度器。
                // DrawingWorker 会在下一个与 WPF VSync 对齐的绘制窗口执行它。
                // 若上一帧委托尚未执行，本帧委托会原子替换它（Replace 语义，后继存在，丢弃安全）。
                // 注意：捕获 _animPhase 的当前值到局部变量，避免委托执行时读到更新后的值。
                double phase = _animPhase;
                FastCanvas.SubmitDrawing(ctx => DrawFrame(ctx, phase));

                UpdateFps();
            }
        }

        /// <summary>
        /// 实际绘制逻辑。由 DrawingWorker 在后台线程回调，不可访问 UI 元素。
        /// <paramref name="phase"/> 是提交时刻捕获的动画相位快照，保证帧间一致性。
        /// </summary>
        private void DrawFrame(IDrawingContext ctx, double phase)
        {
            int w = ctx.Width;
            int h = ctx.Height;

            // ── 背景 ──────────────────────────────────────────────────────
            ctx.DrawRectangle(_bgBrush, (Pen?)null, new Rect(0, 0, w, h));

            // ── 网格 ──────────────────────────────────────────────────────
            DrawGrid(ctx, w, h, 40);

            // ── 动态正弦波（使用捕获的 phase 快照）────────────────────────
            DrawSineWave(ctx, w, h, phase);

            // ── 静态图形展示 ───────────────────────────────────────────────
            DrawShapes(ctx, w, h, phase);

            // ── FPS & 尺寸文字 ─────────────────────────────────────────────
            ctx.DrawText($"FPS: {_fps:F1}  |  {w} × {h} px",
                         new Point(12, 12), _dimBrush, "Segoe UI", 13);

            _frameCount++;
        }

        // ── 网格 ──────────────────────────────────────────────────────────────
        private static void DrawGrid(IDrawingContext ctx, int w, int h, int step)
        {
            for (int x = 0; x <= w; x += step)
                ctx.DrawLine(_gridPen, new Point(x, 0), new Point(x, h));
            for (int y = 0; y <= h; y += step)
                ctx.DrawLine(_gridPen, new Point(0, y), new Point(w, y));
        }

        // ── 动态正弦波 ────────────────────────────────────────────────────────
        private static void DrawSineWave(IDrawingContext ctx, int w, int h, double phase)
        {
            double cy = h / 2.0;
            double amp = h * 0.25;
            const int segments = 200;

            double prevX = 0;
            double prevY = cy + Math.Sin(phase) * amp;

            for (int i = 1; i <= segments; i++)
            {
                double t = (double)i / segments;
                double x = t * w;
                double y = cy + Math.Sin(t * Math.PI * 4 + phase) * amp;

                ctx.DrawLine(_purplePen, new Point(prevX, prevY), new Point(x, y));
                prevX = x;
                prevY = y;
            }

            // 第二条波（相位偏移）
            prevX = 0;
            prevY = cy + Math.Sin(phase + Math.PI) * amp * 0.6;
            for (int i = 1; i <= segments; i++)
            {
                double t = (double)i / segments;
                double x = t * w;
                double y = cy + Math.Sin(t * Math.PI * 4 + phase + Math.PI) * amp * 0.6;

                ctx.DrawLine(_pinkPen, new Point(prevX, prevY), new Point(x, y));
                prevX = x;
                prevY = y;
            }
        }

        // ── 静态图形展示 ──────────────────────────────────────────────────────
        private static void DrawShapes(IDrawingContext ctx, int w, int h, double phase)
        {
            // 左下角：旋转矩形
            double cx1 = 80, cy1 = h - 80;
            double size = 40 + 10 * Math.Sin(phase * 1.5);
            ctx.DrawRoundedRectangle(
                _blueBrush, _whitePen,
                new Rect(cx1 - size / 2, cy1 - size / 2, size, size),
                6, 6);

            // 右下角：脉动椭圆
            double cx2 = w - 80, cy2 = h - 80;
            double rx = 30 + 15 * Math.Sin(phase * 2);
            double ry = 20 + 10 * Math.Cos(phase * 2);
            ctx.DrawEllipse(_greenBrush, (Pen?)null, new Point(cx2, cy2), rx, ry);

            // 右上角：弹跳小球
            double bx = w - 80;
            double by = 60 + 30 * Math.Abs(Math.Sin(phase * 1.2));
            ctx.DrawEllipse(_yellowBrush, (Pen?)null, new Point(bx, by), 12, 12);

            // 左上角：渐变圆环（多环模拟）
            double lx = 80, ly = 80;
            for (int r = 35; r >= 5; r -= 10)
            {
                byte alpha = (byte)(60 + r * 3);
                var brush = new SolidColorBrush(Color.FromArgb(alpha, 0x94, 0xE2, 0xD5));
                brush.Freeze();
                ctx.DrawEllipse((Brush?)null, new Pen(brush, 2), new Point(lx, ly), r, r);
            }

            // 中央：标题文字
            ctx.DrawText("FastDrawingVisual",
                         new Point(w / 2.0 - 90, h / 2.0 + 60),
                         _tealBrush, "Segoe UI", 18);
        }

        // ── FPS 计算 ──────────────────────────────────────────────────────────
        private void UpdateFps()
        {
            Dispatcher.InvokeAsync(() =>
            {
                var now = DateTime.Now;
                double elapsed = (now - _lastFpsTime).TotalSeconds;
                if (elapsed >= 0.5)
                {
                    _fps = _frameCount / elapsed;
                    _frameCount = 0;
                    _lastFpsTime = now;
                    FrameInfoText.Text = $"FPS: {_fps:F1}";
                }
            });
        }

        private void UpdateStatus(bool ready)
        {
            Dispatcher.InvokeAsync(() =>
            {
                StatusIndicator.Fill = ready
    ? new SolidColorBrush(Color.FromRgb(0xA6, 0xE3, 0xA1))
    : new SolidColorBrush(Color.FromRgb(0xF3, 0x8B, 0xA8));
                StatusText.Text = ready ? "FastDrawingVisual 已就绪" : "正在初始化…";
            });
        }

        // ── 工具方法 ──────────────────────────────────────────────────────────
        private static T Freeze<T>(T freezable) where T : Freezable
        {
            freezable.Freeze();
            return freezable;
        }

        protected override void OnClosed(EventArgs e)
        {
            _timer.Dispose();
            FastCanvas.Dispose();
            base.OnClosed(e);
        }
    }
}