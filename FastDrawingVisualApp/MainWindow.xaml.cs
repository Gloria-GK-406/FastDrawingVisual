using FastDrawingVisual.Controls;
using FastDrawingVisualApp.Benchmark;
using FastDrawingVisualApp.Benchmark.Scenarios;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace FastDrawingVisualApp
{
    public partial class MainWindow : Window
    {
        private readonly DispatcherTimer _metricsTimer;
        private readonly List<FastDrawingVisual.Controls.FastDrawingVisual> _multiCanvasTiles = new();
        private readonly IReadOnlyList<RendererOption> _rendererOptions;
        private readonly ObservableCollection<BenchmarkRunRecord> _history = new();
        private IBenchmarkRunController? _runner;

        public MainWindow()
        {
            InitializeComponent();
            InitializeMultiCanvasTiles();

            _rendererOptions = new[]
            {
                new RendererOption("Auto", RendererPreference.Auto, "Try Skia, then D3D9, then WPF fallback."),
                new RendererOption("Skia", RendererPreference.Skia, "Primary accelerated path."),
                new RendererOption("D3D9", RendererPreference.D3D9, "Native D3D9 command bridge."),
                new RendererOption("WPF", RendererPreference.Wpf, "Compatibility fallback."),
                new RendererOption("D3D11 AirSpace", RendererPreference.D3D11AirSpace, "Experimental explicit path."),
                new RendererOption("D3D11 ShareD3D9", RendererPreference.D3D11ShareD3D9, "Experimental D3D11-led render path presented through D3D9 shared surfaces.")
            };

            RendererCombo.ItemsSource = _rendererOptions;
            ScenarioCombo.ItemsSource = BenchmarkScenarioCatalog.All;
            ScaleCombo.ItemsSource = BenchmarkScalePreset.All;
            PacingCombo.ItemsSource = SubmitPacingPreset.All;
            DurationCombo.ItemsSource = RunDurationPreset.All;
            HistoryList.ItemsSource = _history;

            RendererCombo.SelectedIndex = 0;
            ScenarioCombo.SelectedIndex = 1;
            ScaleCombo.SelectedItem = BenchmarkScalePreset.Medium;
            PacingCombo.SelectedItem = SubmitPacingPreset.P240;
            DurationCombo.SelectedItem = RunDurationPreset.Seconds30;

            _metricsTimer = new DispatcherTimer(DispatcherPriority.DataBind)
            {
                Interval = TimeSpan.FromMilliseconds(500)
            };
            _metricsTimer.Tick += OnMetricsTick;
            _metricsTimer.Start();

            UpdateConfigDescription();
            UpdateCanvasMode();
            UpdateRunningState(false);
        }

        private void OnStartClicked(object sender, RoutedEventArgs e)
        {
            StopRunner();

            var config = BuildConfig();
            if (IsGrid16Scenario(config.Scenario))
            {
                for (int i = 0; i < _multiCanvasTiles.Count; i++)
                    _multiCanvasTiles[i].PreferredRenderer = config.Renderer;

                _runner = new MultiCanvasBenchmarkRunner(_multiCanvasTiles, config);
            }
            else
            {
                FastCanvas.PreferredRenderer = config.Renderer;
                _runner = new BenchmarkRunner(FastCanvas, config);
            }

            StatusText.Text = "Running";
            ConfigSummaryText.Text = config.Summary;
            CanvasOverlayText.Text = BuildCanvasOverlayText(config.Scenario, ((RendererOption?)RendererCombo.SelectedItem)?.DisplayName ?? config.Renderer.ToString());
            UpdateRunningState(true);
            UpdateMetrics(_runner.CreateSnapshot());
        }

        private void OnStopClicked(object sender, RoutedEventArgs e)
        {
            StopRunner();
        }

        private void OnMetricsTick(object? sender, EventArgs e)
        {
            if (_runner == null)
            {
                UpdateMetrics(new BenchmarkMetricsSnapshot(
                    isRunning: false,
                    stopReason: BenchmarkStopReason.Manual,
                    elapsed: TimeSpan.Zero,
                    submittedTotal: 0,
                    executedTotal: 0,
                    droppedTotal: 0,
                    pendingTotal: 0,
                    submitHz: 0,
                    executeHz: 0,
                    recentDropRatePercent: 0,
                    prepareDuration: new RollingValueStatistics(0, 0, 0, 0),
                    queueDelay: new RollingValueStatistics(0, 0, 0, 0),
                    drawDuration: new RollingValueStatistics(0, 0, 0, 0),
                    endToEndLatency: new RollingValueStatistics(0, 0, 0, 0)));
                return;
            }

            var snapshot = _runner.CreateSnapshot();
            UpdateMetrics(snapshot);

            if (!snapshot.IsRunning)
                CompleteRunner(snapshot);
        }

        private void OnConfigSelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            UpdateConfigDescription();
            UpdateCanvasMode();
        }

        private BenchmarkConfig BuildConfig()
        {
            var renderer = ((RendererOption?)RendererCombo.SelectedItem)?.Value ?? RendererPreference.Auto;
            var scenario = (IBenchmarkScenario?)ScenarioCombo.SelectedItem ?? BenchmarkScenarioCatalog.All[0];
            var scale = (BenchmarkScalePreset?)ScaleCombo.SelectedItem ?? BenchmarkScalePreset.Medium;
            var pacing = (SubmitPacingPreset?)PacingCombo.SelectedItem ?? SubmitPacingPreset.P240;
            var duration = (RunDurationPreset?)DurationCombo.SelectedItem ?? RunDurationPreset.Seconds30;
            return new BenchmarkConfig(renderer, scenario, scale, pacing, duration, seed: 20260309);
        }

        private void UpdateConfigDescription()
        {
            var renderer = (RendererOption?)RendererCombo.SelectedItem;
            var scenario = (IBenchmarkScenario?)ScenarioCombo.SelectedItem;
            var scale = (BenchmarkScalePreset?)ScaleCombo.SelectedItem;
            var pacing = (SubmitPacingPreset?)PacingCombo.SelectedItem;
            var duration = (RunDurationPreset?)DurationCombo.SelectedItem;

            ScenarioTitleText.Text = scenario?.DisplayName ?? "No scenario";
            ScenarioDescriptionText.Text = scenario?.Description ?? string.Empty;
            ScaleSummaryText.Text = scale?.Summary ?? string.Empty;
            PacingDescriptionText.Text = pacing?.Description ?? string.Empty;
            DurationDescriptionText.Text = duration?.Description ?? string.Empty;
            RendererDescriptionText.Text = renderer?.Description ?? string.Empty;
            CanvasOverlayText.Text = renderer == null
                ? "Idle"
                : BuildCanvasOverlayText(scenario, renderer.DisplayName);
        }

        private void UpdateRunningState(bool isRunning)
        {
            RendererCombo.IsEnabled = !isRunning;
            ScenarioCombo.IsEnabled = !isRunning;
            ScaleCombo.IsEnabled = !isRunning;
            PacingCombo.IsEnabled = !isRunning;
            DurationCombo.IsEnabled = !isRunning;
            StartButton.IsEnabled = !isRunning;
            StopButton.IsEnabled = isRunning;

            if (!isRunning)
            {
                StatusText.Text = "Idle";
                ConfigSummaryText.Text = "No benchmark running.";
            }
        }

        private void UpdateMetrics(BenchmarkMetricsSnapshot snapshot)
        {
            SubmitHzText.Text = snapshot.SubmitHz.ToString("F1");
            ExecuteHzText.Text = snapshot.ExecuteHz.ToString("F1");
            DropRateText.Text = snapshot.RecentDropRatePercent.ToString("F1");
            PendingText.Text = snapshot.PendingTotal.ToString("n0");

            PrepareAvgText.Text = snapshot.PrepareDuration.Average.ToString("F2");
            PrepareP95Text.Text = snapshot.PrepareDuration.P95.ToString("F2");
            PrepareMaxText.Text = snapshot.PrepareDuration.Max.ToString("F2");

            DrawAvgText.Text = snapshot.DrawDuration.Average.ToString("F2");
            DrawP95Text.Text = snapshot.DrawDuration.P95.ToString("F2");
            DrawMaxText.Text = snapshot.DrawDuration.Max.ToString("F2");

            QueueAvgText.Text = snapshot.QueueDelay.Average.ToString("F2");
            QueueP95Text.Text = snapshot.QueueDelay.P95.ToString("F2");
            QueueMaxText.Text = snapshot.QueueDelay.Max.ToString("F2");

            LatencyAvgText.Text = snapshot.EndToEndLatency.Average.ToString("F2");
            LatencyP95Text.Text = snapshot.EndToEndLatency.P95.ToString("F2");
            LatencyMaxText.Text = snapshot.EndToEndLatency.Max.ToString("F2");

            TotalsText.Text = $"submitted={snapshot.SubmittedTotal:n0} executed={snapshot.ExecutedTotal:n0} dropped={snapshot.DroppedTotal:n0}";

            if (_runner != null)
            {
                ConfigSummaryText.Text = $"{_runner.Config.Summary} | {_runner.ScenarioSummary} | elapsed {snapshot.Elapsed:mm\\:ss}";
                StatusText.Text = snapshot.IsRunning ? "Running" : $"Completed ({snapshot.StopReason})";
            }
        }

        private void StopRunner()
        {
            if (_runner != null)
            {
                _runner.RequestManualStop();
                _runner.Dispose();
                var snapshot = _runner.CreateSnapshot();
                if (snapshot.SubmittedTotal > 0 || snapshot.ExecutedTotal > 0)
                    AddHistoryRecord(_runner, snapshot);
                _runner = null;
            }

            UpdateRunningState(false);
            UpdateConfigDescription();
        }

        private void CompleteRunner(BenchmarkMetricsSnapshot snapshot)
        {
            if (_runner == null)
                return;

            AddHistoryRecord(_runner, snapshot);
            _runner.Dispose();
            _runner = null;
            UpdateRunningState(false);
            UpdateConfigDescription();
        }

        private void AddHistoryRecord(IBenchmarkRunController runner, BenchmarkMetricsSnapshot snapshot)
        {
            if (_history.Count >= 10)
                _history.RemoveAt(_history.Count - 1);

            _history.Insert(0, new BenchmarkRunRecord(DateTime.Now, runner.Config, runner.ScenarioSummary, snapshot));
        }

        protected override void OnClosed(EventArgs e)
        {
            _metricsTimer.Stop();
            StopRunner();
            for (int i = 0; i < _multiCanvasTiles.Count; i++)
                _multiCanvasTiles[i].Dispose();
            FastCanvas.Dispose();
            base.OnClosed(e);
        }

        private void InitializeMultiCanvasTiles()
        {
            for (int i = 0; i < 16; i++)
            {
                var tileCanvas = new FastDrawingVisual.Controls.FastDrawingVisual
                {
                    Margin = new Thickness(1)
                };
                var tileLabel = new TextBlock
                {
                    Text = $"#{i + 1:00}",
                    Foreground = (Brush)FindResource("TextMain"),
                    FontSize = 11,
                    FontFamily = new FontFamily("Consolas")
                };
                var tileOverlay = new Border
                {
                    Background = new SolidColorBrush(Color.FromArgb(0xAA, 0x11, 0x18, 0x21)),
                    CornerRadius = new CornerRadius(6),
                    Padding = new Thickness(8, 4, 8, 4),
                    Margin = new Thickness(10),
                    VerticalAlignment = VerticalAlignment.Top,
                    HorizontalAlignment = HorizontalAlignment.Left,
                    Child = tileLabel
                };
                var tileHost = new Grid();
                tileHost.Children.Add(tileCanvas);
                tileHost.Children.Add(tileOverlay);

                var tileBorder = new Border
                {
                    Margin = new Thickness(6),
                    Background = (Brush)FindResource("PanelBgAlt"),
                    BorderBrush = (Brush)FindResource("Stroke"),
                    BorderThickness = new Thickness(1),
                    CornerRadius = new CornerRadius(8),
                    Child = tileHost
                };

                MultiCanvasGrid.Children.Add(tileBorder);
                _multiCanvasTiles.Add(tileCanvas);
            }
        }

        private void UpdateCanvasMode()
        {
            bool useGrid16 = IsGrid16Scenario(ScenarioCombo.SelectedItem as IBenchmarkScenario);
            FastCanvas.Visibility = useGrid16 ? Visibility.Collapsed : Visibility.Visible;
            SingleCanvasOverlay.Visibility = useGrid16 ? Visibility.Collapsed : Visibility.Visible;
            MultiCanvasGrid.Visibility = useGrid16 ? Visibility.Visible : Visibility.Collapsed;
        }

        private static bool IsGrid16Scenario(IBenchmarkScenario? scenario)
            => string.Equals(scenario?.Key, KLineGrid16Scenario.ScenarioId, StringComparison.Ordinal);

        private static string BuildCanvasOverlayText(IBenchmarkScenario? scenario, string rendererDisplayName)
        {
            if (scenario == null)
                return "Idle";

            return IsGrid16Scenario(scenario)
                ? $"{rendererDisplayName} | 16x {scenario.DisplayName}"
                : $"{rendererDisplayName} | {scenario.DisplayName}";
        }

        private sealed class RendererOption
        {
            public RendererOption(string displayName, RendererPreference value, string description)
            {
                DisplayName = displayName;
                Value = value;
                Description = description;
            }

            public string DisplayName { get; }

            public RendererPreference Value { get; }

            public string Description { get; }
        }
    }
}
