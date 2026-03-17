using FastDrawingVisual.Rendering;
using System;
using System.Collections.Generic;
using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace FastDrawingVisualApp.Benchmark.Scenarios
{
    internal sealed class DrawingContextCoverageScenario : IBenchmarkScenario
    {
        public string Key => "drawing-context-coverage";

        public string DisplayName => "DrawingContext Coverage";

        public string Description =>
            "Static validation scene that exercises every IDrawingContext draw verb and state-stack API in one frame so renderer completeness can be checked visually.";

        public IBenchmarkScenarioSession CreateSession(BenchmarkScalePreset scale, int seed)
            => new Session(scale);

        private sealed class Session : IPreparedBenchmarkScenarioSession
        {
            private readonly BenchmarkScalePreset _scale;
            private PreparedFrame? _cachedFrame;
            private int _cachedWidth;
            private int _cachedHeight;

            public Session(BenchmarkScalePreset scale)
            {
                _scale = scale;
            }

            public string Summary =>
                $"{_scale.DisplayName}, all draw verbs, static replay";

            public void Dispose()
            {
                _cachedFrame = null;
            }

            public void RenderFrame(IDrawingContext ctx, BenchmarkFrameContext frame)
            {
                var prepared = PrepareFrame(new BenchmarkRenderSurface(ctx.Width, ctx.Height), frame);
                RenderPreparedFrame(ctx, prepared);
            }

            public IPreparedBenchmarkFrame PrepareFrame(BenchmarkRenderSurface surface, BenchmarkFrameContext frame)
            {
                if (_cachedFrame != null &&
                    _cachedWidth == surface.Width &&
                    _cachedHeight == surface.Height)
                {
                    return _cachedFrame;
                }

                _cachedWidth = surface.Width;
                _cachedHeight = surface.Height;
                _cachedFrame = BuildPreparedFrame(surface.Width, surface.Height);
                return _cachedFrame;
            }

            public void RenderPreparedFrame(IDrawingContext ctx, IPreparedBenchmarkFrame preparedFrame)
            {
                var frame = (PreparedFrame)preparedFrame;

                ctx.DrawRectangle(BenchmarkDrawingPalette.CanvasBackground, null!, frame.CanvasRect);

                DrawPanelChrome(ctx, frame.HeaderRect, "Coverage Header");
                ctx.DrawText(
                    "DrawingContext Coverage",
                    new Point(frame.HeaderRect.Left + 18, frame.HeaderRect.Top + 16),
                    BenchmarkDrawingPalette.NeutralBrush,
                    fontSize: frame.HeaderTitleFontSize);
                ctx.DrawText(
                    "DrawRectangle, DrawRoundedRectangle, DrawEllipse, DrawLine, DrawGeometry, DrawImage, DrawText, DrawGlyphRun, DrawDrawing, PushClip, PushGuidelineSet, PushOpacity, PushOpacityMask, PushTransform.",
                    new Point(frame.HeaderRect.Left + 18, frame.HeaderRect.Top + 44),
                    BenchmarkDrawingPalette.MutedBrush,
                    fontSize: frame.BodyFontSize);

                DrawPanelChrome(ctx, frame.PrimitivePanelRect, "Direct Verbs");
                ctx.DrawRectangle(BenchmarkDrawingPalette.AccentBrush, BenchmarkDrawingPalette.NeutralPen, frame.FillRect);
                ctx.DrawRoundedRectangle(BenchmarkDrawingPalette.SecondaryAccentBrush, BenchmarkDrawingPalette.NeutralPen, frame.RoundedRect, frame.CornerRadius, frame.CornerRadius);
                ctx.DrawEllipse(BenchmarkDrawingPalette.TertiaryAccentBrush, BenchmarkDrawingPalette.NeutralPen, frame.EllipseCenter, frame.EllipseRadiusX, frame.EllipseRadiusY);
                ctx.DrawLine(BenchmarkDrawingPalette.AccentPen, frame.LineStart, frame.LineEnd);
                ctx.DrawGeometry(null!, BenchmarkDrawingPalette.SecondaryAccentPen, frame.GeometryGroup);
                ctx.DrawText("DrawText(...)", frame.DrawTextOrigin, BenchmarkDrawingPalette.NeutralBrush, fontSize: frame.SectionTitleFontSize);

                DrawPanelChrome(ctx, frame.MediaPanelRect, "Image + Glyph");
                ctx.DrawImage(frame.DemoImage, frame.ImageRect);
                ctx.DrawGlyphRun(BenchmarkDrawingPalette.NeutralBrush, frame.GlyphRun);
                ctx.DrawText("DrawImage(...) + DrawGlyphRun(...)", frame.MediaCaptionOrigin, BenchmarkDrawingPalette.MutedBrush, fontSize: frame.BodyFontSize);

                DrawPanelChrome(ctx, frame.DrawingPanelRect, "DrawDrawing");
                ctx.DrawDrawing(frame.CompositeDrawing);
                ctx.DrawText("DrawDrawing(DrawingGroup)", frame.DrawingCaptionOrigin, BenchmarkDrawingPalette.MutedBrush, fontSize: frame.BodyFontSize);

                DrawPanelChrome(ctx, frame.StatePanelRect, "State Stack");
                ctx.PushTransform(frame.StackTransform);
                ctx.PushClip(frame.StackClip);
                ctx.PushOpacity(0.88);
                ctx.PushGuidelineSet(frame.GuidelineSet);
                ctx.DrawRoundedRectangle(BenchmarkDrawingPalette.PanelBackground, BenchmarkDrawingPalette.AccentPen, frame.StackRect, frame.CornerRadius, frame.CornerRadius);
                ctx.DrawLine(BenchmarkDrawingPalette.SecondaryAccentPen, frame.StackLineStart, frame.StackLineEnd);
                ctx.DrawText("PushTransform + PushClip + PushOpacity + PushGuidelineSet", frame.StackLabelOrigin, BenchmarkDrawingPalette.NeutralBrush, fontSize: frame.BodyFontSize);
                ctx.Pop();
                ctx.Pop();
                ctx.Pop();
                ctx.Pop();

                ctx.PushOpacityMask(frame.OpacityMask);
                ctx.DrawRectangle(BenchmarkDrawingPalette.TertiaryAccentBrush, null!, frame.MaskRect);
                ctx.DrawEllipse(BenchmarkDrawingPalette.AccentBrush, null!, frame.MaskEllipseCenter, frame.MaskEllipseRadiusX, frame.MaskEllipseRadiusY);
                ctx.Pop();

                ctx.DrawText("PushOpacityMask(...)", frame.MaskCaptionOrigin, BenchmarkDrawingPalette.MutedBrush, fontSize: frame.BodyFontSize);
            }

            private PreparedFrame BuildPreparedFrame(int width, int height)
            {
                double margin = Math.Max(16, Math.Min(width, height) * 0.03);
                double gutter = Math.Max(12, margin * 0.55);
                double headerHeight = Math.Max(84, height * 0.14);
                double panelTop = margin + headerHeight + gutter;
                double panelHeight = Math.Max(140, (height - panelTop - margin - gutter) * 0.5);
                double columnWidth = Math.Max(180, (width - (margin * 2) - gutter) * 0.5);

                Rect canvasRect = new(0, 0, width, height);
                Rect headerRect = new(margin, margin, Math.Max(120, width - (margin * 2)), headerHeight);
                Rect primitivePanel = new(margin, panelTop, columnWidth, panelHeight);
                Rect mediaPanel = new(primitivePanel.Right + gutter, panelTop, columnWidth, panelHeight);
                Rect statePanel = new(margin, primitivePanel.Bottom + gutter, columnWidth, panelHeight);
                Rect drawingPanel = new(mediaPanel.Left, mediaPanel.Bottom + gutter, columnWidth, panelHeight);

                Rect fillRect = new(
                    primitivePanel.Left + 22,
                    primitivePanel.Top + 46,
                    primitivePanel.Width * 0.22,
                    primitivePanel.Height * 0.22);
                Rect roundedRect = new(
                    fillRect.Right + 18,
                    primitivePanel.Top + 46,
                    primitivePanel.Width * 0.24,
                    primitivePanel.Height * 0.22);
                Point ellipseCenter = new(
                    primitivePanel.Left + primitivePanel.Width * 0.22,
                    primitivePanel.Top + primitivePanel.Height * 0.63);
                double ellipseRadiusX = Math.Max(16, primitivePanel.Width * 0.12);
                double ellipseRadiusY = Math.Max(12, primitivePanel.Height * 0.09);
                Point lineStart = new(primitivePanel.Left + primitivePanel.Width * 0.46, primitivePanel.Top + primitivePanel.Height * 0.52);
                Point lineEnd = new(primitivePanel.Left + primitivePanel.Width * 0.86, primitivePanel.Top + primitivePanel.Height * 0.78);
                Point drawTextOrigin = new(primitivePanel.Left + 22, primitivePanel.Bottom - 34);

                Rect imageRect = new(
                    mediaPanel.Left + 22,
                    mediaPanel.Top + 46,
                    mediaPanel.Width * 0.42,
                    mediaPanel.Height * 0.46);
                Point glyphOrigin = new(
                    mediaPanel.Left + 22,
                    mediaPanel.Top + mediaPanel.Height * 0.78);
                Point mediaCaptionOrigin = new(mediaPanel.Left + 22, mediaPanel.Bottom - 34);

                Point drawingCaptionOrigin = new(drawingPanel.Left + 22, drawingPanel.Bottom - 34);

                Rect stackRect = new(0, 0, statePanel.Width * 0.74, statePanel.Height * 0.34);
                Point stackLineStart = new(stackRect.Left + 18, stackRect.Top + stackRect.Height - 18);
                Point stackLineEnd = new(stackRect.Right - 18, stackRect.Top + 18);
                Point stackLabelOrigin = new(14, stackRect.Top + stackRect.Height + 12);
                var stackTransform = Freeze(new MatrixTransform(
                    0.96,
                    0,
                    0,
                    0.96,
                    statePanel.Left + 22,
                    statePanel.Top + 46));
                var stackClip = Freeze<Geometry>(new RectangleGeometry(new Rect(0, 0, statePanel.Width - 44, statePanel.Height * 0.56)));
                var guidelineSet = Freeze(new GuidelineSet(
                    new[] { 0.5, stackRect.Width - 0.5, stackRect.Width * 0.5 + 0.5 },
                    new[] { 0.5, stackRect.Height - 0.5, stackRect.Height * 0.5 + 0.5 }));

                Rect maskRect = new(
                    statePanel.Left + 22,
                    statePanel.Top + statePanel.Height * 0.66,
                    statePanel.Width * 0.74,
                    statePanel.Height * 0.18);
                Point maskEllipseCenter = new(maskRect.Right - 34, maskRect.Top + maskRect.Height * 0.5);
                double maskEllipseRadiusX = Math.Max(10, maskRect.Height * 0.42);
                double maskEllipseRadiusY = Math.Max(10, maskRect.Height * 0.42);
                Point maskCaptionOrigin = new(statePanel.Left + 22, statePanel.Bottom - 34);

                var geometryGroup = CreateGeometryGroup(primitivePanel);
                var demoImage = CreateScenarioImage();
                var glyphRun = CreateGlyphRun("GlyphRun()", glyphOrigin, Math.Max(18, Math.Min(30, mediaPanel.Height * 0.12)));
                var compositeDrawing = CreateCompositeDrawing(drawingPanel, demoImage);
                var opacityMask = CreateOpacityMaskBrush();

                return new PreparedFrame(
                    canvasRect,
                    headerRect,
                    primitivePanel,
                    mediaPanel,
                    statePanel,
                    drawingPanel,
                    fillRect,
                    roundedRect,
                    ellipseCenter,
                    ellipseRadiusX,
                    ellipseRadiusY,
                    lineStart,
                    lineEnd,
                    drawTextOrigin,
                    imageRect,
                    glyphRun,
                    mediaCaptionOrigin,
                    compositeDrawing,
                    drawingCaptionOrigin,
                    stackRect,
                    stackLineStart,
                    stackLineEnd,
                    stackLabelOrigin,
                    stackTransform,
                    stackClip,
                    guidelineSet,
                    maskRect,
                    maskEllipseCenter,
                    maskEllipseRadiusX,
                    maskEllipseRadiusY,
                    maskCaptionOrigin,
                    geometryGroup,
                    demoImage,
                    opacityMask,
                    Math.Max(14, Math.Min(18, height * 0.022)),
                    Math.Max(18, Math.Min(32, height * 0.032)),
                    Math.Max(11, Math.Min(14, height * 0.017)),
                    Math.Max(10, Math.Min(12, height * 0.015)),
                    Math.Max(10, Math.Min(18, Math.Min(width, height) * 0.018)));
            }

            private static void DrawPanelChrome(IDrawingContext ctx, Rect rect, string title)
            {
                ctx.DrawRoundedRectangle(BenchmarkDrawingPalette.PanelBackground, BenchmarkDrawingPalette.MutedPen, rect, 16, 16);
                ctx.DrawText(title, new Point(rect.Left + 14, rect.Top + 12), BenchmarkDrawingPalette.AccentBrush, fontSize: 13);
            }

            private static Geometry CreateGeometryGroup(Rect panel)
            {
                var group = new GeometryGroup();
                group.Children.Add(new RectangleGeometry(new Rect(
                    panel.Left + panel.Width * 0.58,
                    panel.Top + panel.Height * 0.16,
                    panel.Width * 0.10,
                    panel.Height * 0.14)));
                group.Children.Add(new EllipseGeometry(
                    new Point(panel.Left + panel.Width * 0.76, panel.Top + panel.Height * 0.25),
                    panel.Width * 0.08,
                    panel.Height * 0.10));
                group.Children.Add(new LineGeometry(
                    new Point(panel.Left + panel.Width * 0.56, panel.Top + panel.Height * 0.38),
                    new Point(panel.Left + panel.Width * 0.88, panel.Top + panel.Height * 0.18)));
                group.Freeze();
                return group;
            }

            private static ImageSource CreateScenarioImage()
            {
                string outputImagePath = Path.Combine(AppContext.BaseDirectory, "Image", "img.jpeg");
                if (File.Exists(outputImagePath))
                {
                    var bitmap = new BitmapImage();
                    bitmap.BeginInit();
                    bitmap.CacheOption = BitmapCacheOption.OnLoad;
                    bitmap.CreateOptions = BitmapCreateOptions.IgnoreColorProfile;
                    bitmap.UriSource = new Uri(outputImagePath, UriKind.Absolute);
                    bitmap.EndInit();
                    bitmap.Freeze();
                    return bitmap;
                }

                return CreateFallbackImage(96, 96);
            }

            private static ImageSource CreateFallbackImage(int width, int height)
            {
                var pixels = new byte[width * height * 4];

                for (int y = 0; y < height; y++)
                {
                    for (int x = 0; x < width; x++)
                    {
                        int offset = (y * width + x) * 4;
                        byte r = (byte)(32 + ((x * 190) / Math.Max(1, width - 1)));
                        byte g = (byte)(40 + ((y * 150) / Math.Max(1, height - 1)));
                        byte b = (byte)(70 + (((x + y) * 110) / Math.Max(1, width + height - 2)));

                        if (((x / 12) + (y / 12)) % 2 == 0)
                            b = (byte)Math.Min(255, b + 35);

                        if (Math.Abs(x - y) < 4 || Math.Abs((width - 1 - x) - y) < 4)
                        {
                            r = 255;
                            g = 180;
                            b = 84;
                        }

                        pixels[offset + 0] = b;
                        pixels[offset + 1] = g;
                        pixels[offset + 2] = r;
                        pixels[offset + 3] = 255;
                    }
                }

                var bitmap = BitmapSource.Create(width, height, 96, 96, PixelFormats.Pbgra32, null, pixels, width * 4);
                bitmap.Freeze();
                return bitmap;
            }

            private static Drawing CreateCompositeDrawing(Rect panel, ImageSource image)
            {
                var group = new DrawingGroup();
                group.Children.Add(new GeometryDrawing(
                    BenchmarkDrawingPalette.GridBrush,
                    BenchmarkDrawingPalette.GridPen,
                    Freeze<Geometry>(new RectangleGeometry(new Rect(panel.Left + 20, panel.Top + 44, panel.Width - 40, panel.Height - 84)))));

                var innerGeometry = new GeometryGroup();
                innerGeometry.Children.Add(new RectangleGeometry(new Rect(panel.Left + 38, panel.Top + 66, panel.Width * 0.28, panel.Height * 0.18)));
                innerGeometry.Children.Add(new EllipseGeometry(new Point(panel.Left + panel.Width * 0.62, panel.Top + panel.Height * 0.44), panel.Width * 0.12, panel.Height * 0.14));
                innerGeometry.Freeze();

                group.Children.Add(new GeometryDrawing(
                    BenchmarkDrawingPalette.SecondaryAccentBrush,
                    BenchmarkDrawingPalette.SecondaryAccentPen,
                    innerGeometry));
                group.Children.Add(new ImageDrawing(
                    image,
                    new Rect(panel.Left + panel.Width * 0.50, panel.Top + 62, panel.Width * 0.22, panel.Width * 0.22)));
                group.Freeze();
                return group;
            }

            private static GlyphRun CreateGlyphRun(string text, Point origin, double fontSize)
            {
                var glyphTypeface = ResolveGlyphTypeface();
                var glyphIndices = new List<ushort>(text.Length);
                var advanceWidths = new List<double>(text.Length);
                var characters = new List<char>(text.Length);

                foreach (char ch in text)
                {
                    if (!glyphTypeface.CharacterToGlyphMap.TryGetValue(ch, out ushort glyphIndex) &&
                        !glyphTypeface.CharacterToGlyphMap.TryGetValue('?', out glyphIndex))
                    {
                        glyphIndex = 0;
                    }

                    glyphIndices.Add(glyphIndex);
                    advanceWidths.Add(glyphTypeface.AdvanceWidths[glyphIndex] * fontSize);
                    characters.Add(ch);
                }

                return new GlyphRun(
                    glyphTypeface,
                    0,
                    false,
                    fontSize,
                    glyphIndices,
                    origin,
                    advanceWidths,
                    null,
                    characters,
                    null,
                    null,
                    null,
                    null);
            }

            private static GlyphTypeface ResolveGlyphTypeface()
            {
                foreach (string familyName in new[] { "Segoe UI", "Arial", "Consolas" })
                {
                    var typeface = new Typeface(new FontFamily(familyName), FontStyles.Normal, FontWeights.Normal, FontStretches.Normal);
                    if (typeface.TryGetGlyphTypeface(out GlyphTypeface glyphTypeface))
                        return glyphTypeface;
                }

                throw new InvalidOperationException("No glyph typeface available for coverage scenario.");
            }

            private static Brush CreateOpacityMaskBrush()
            {
                var brush = new LinearGradientBrush();
                brush.StartPoint = new Point(0, 0.5);
                brush.EndPoint = new Point(1, 0.5);
                brush.GradientStops.Add(new GradientStop(Color.FromArgb(0x18, 0xFF, 0xFF, 0xFF), 0.0));
                brush.GradientStops.Add(new GradientStop(Color.FromArgb(0xD0, 0xFF, 0xFF, 0xFF), 0.35));
                brush.GradientStops.Add(new GradientStop(Color.FromArgb(0xFF, 0xFF, 0xFF, 0xFF), 0.65));
                brush.GradientStops.Add(new GradientStop(Color.FromArgb(0x20, 0xFF, 0xFF, 0xFF), 1.0));
                brush.Freeze();
                return brush;
            }

            private static T Freeze<T>(T freezable) where T : Freezable
            {
                freezable.Freeze();
                return freezable;
            }

            private sealed class PreparedFrame : IPreparedBenchmarkFrame
            {
                public PreparedFrame(
                    Rect canvasRect,
                    Rect headerRect,
                    Rect primitivePanelRect,
                    Rect mediaPanelRect,
                    Rect statePanelRect,
                    Rect drawingPanelRect,
                    Rect fillRect,
                    Rect roundedRect,
                    Point ellipseCenter,
                    double ellipseRadiusX,
                    double ellipseRadiusY,
                    Point lineStart,
                    Point lineEnd,
                    Point drawTextOrigin,
                    Rect imageRect,
                    GlyphRun glyphRun,
                    Point mediaCaptionOrigin,
                    Drawing compositeDrawing,
                    Point drawingCaptionOrigin,
                    Rect stackRect,
                    Point stackLineStart,
                    Point stackLineEnd,
                    Point stackLabelOrigin,
                    Transform stackTransform,
                    Geometry stackClip,
                    GuidelineSet guidelineSet,
                    Rect maskRect,
                    Point maskEllipseCenter,
                    double maskEllipseRadiusX,
                    double maskEllipseRadiusY,
                    Point maskCaptionOrigin,
                    Geometry geometryGroup,
                    ImageSource demoImage,
                    Brush opacityMask,
                    double sectionTitleFontSize,
                    double headerTitleFontSize,
                    double bodyFontSize,
                    double captionFontSize,
                    double cornerRadius)
                {
                    CanvasRect = canvasRect;
                    HeaderRect = headerRect;
                    PrimitivePanelRect = primitivePanelRect;
                    MediaPanelRect = mediaPanelRect;
                    StatePanelRect = statePanelRect;
                    DrawingPanelRect = drawingPanelRect;
                    FillRect = fillRect;
                    RoundedRect = roundedRect;
                    EllipseCenter = ellipseCenter;
                    EllipseRadiusX = ellipseRadiusX;
                    EllipseRadiusY = ellipseRadiusY;
                    LineStart = lineStart;
                    LineEnd = lineEnd;
                    DrawTextOrigin = drawTextOrigin;
                    ImageRect = imageRect;
                    GlyphRun = glyphRun;
                    MediaCaptionOrigin = mediaCaptionOrigin;
                    CompositeDrawing = compositeDrawing;
                    DrawingCaptionOrigin = drawingCaptionOrigin;
                    StackRect = stackRect;
                    StackLineStart = stackLineStart;
                    StackLineEnd = stackLineEnd;
                    StackLabelOrigin = stackLabelOrigin;
                    StackTransform = stackTransform;
                    StackClip = stackClip;
                    GuidelineSet = guidelineSet;
                    MaskRect = maskRect;
                    MaskEllipseCenter = maskEllipseCenter;
                    MaskEllipseRadiusX = maskEllipseRadiusX;
                    MaskEllipseRadiusY = maskEllipseRadiusY;
                    MaskCaptionOrigin = maskCaptionOrigin;
                    GeometryGroup = geometryGroup;
                    DemoImage = demoImage;
                    OpacityMask = opacityMask;
                    SectionTitleFontSize = sectionTitleFontSize;
                    HeaderTitleFontSize = headerTitleFontSize;
                    BodyFontSize = bodyFontSize;
                    CaptionFontSize = captionFontSize;
                    CornerRadius = cornerRadius;
                }

                public Rect CanvasRect { get; }
                public Rect HeaderRect { get; }
                public Rect PrimitivePanelRect { get; }
                public Rect MediaPanelRect { get; }
                public Rect StatePanelRect { get; }
                public Rect DrawingPanelRect { get; }
                public Rect FillRect { get; }
                public Rect RoundedRect { get; }
                public Point EllipseCenter { get; }
                public double EllipseRadiusX { get; }
                public double EllipseRadiusY { get; }
                public Point LineStart { get; }
                public Point LineEnd { get; }
                public Point DrawTextOrigin { get; }
                public Rect ImageRect { get; }
                public GlyphRun GlyphRun { get; }
                public Point MediaCaptionOrigin { get; }
                public Drawing CompositeDrawing { get; }
                public Point DrawingCaptionOrigin { get; }
                public Rect StackRect { get; }
                public Point StackLineStart { get; }
                public Point StackLineEnd { get; }
                public Point StackLabelOrigin { get; }
                public Transform StackTransform { get; }
                public Geometry StackClip { get; }
                public GuidelineSet GuidelineSet { get; }
                public Rect MaskRect { get; }
                public Point MaskEllipseCenter { get; }
                public double MaskEllipseRadiusX { get; }
                public double MaskEllipseRadiusY { get; }
                public Point MaskCaptionOrigin { get; }
                public Geometry GeometryGroup { get; }
                public ImageSource DemoImage { get; }
                public Brush OpacityMask { get; }
                public double SectionTitleFontSize { get; }
                public double HeaderTitleFontSize { get; }
                public double BodyFontSize { get; }
                public double CaptionFontSize { get; }
                public double CornerRadius { get; }
            }
        }
    }
}
