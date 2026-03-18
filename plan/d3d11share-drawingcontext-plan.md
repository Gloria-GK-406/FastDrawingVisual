# D3D11Share DrawingContext Completion Plan

Date: 2026-03-18
Scope: `FastDrawingVisual.NativeProxy.D3D11` + shared command path used by `RendererPreference.D3D11ShareD3D9`

## Goal

Complete the highest-value missing `IDrawingContext` features for the D3D11Share path in dependency order, starting with rounded rectangles and then closing the most visible coverage gaps.

## Current Findings

- The D3D11Share backend records drawing through `LayeredCommandRecordingContext`.
- `DrawRoundedRectangle` currently degrades to `DrawRectangle`.
- `DrawImage`, `DrawGlyphRun`, `PushClip`, `PushOpacity`, `PushOpacityMask`, and `PushGuidelineSet` are incomplete or explicit no-op in the layered command writer.
- The D3D11 pixel shader already supports rounded-box signed distance rendering using the `radius` field in shape instances.
- The shared command protocol currently has no rounded rectangle command, so managed and native sides cannot preserve corner radius.

## Ordered Work

### Phase 1: Rounded Rectangle End-to-End

Reason: shortest complete path, highest visual value, already partially supported by native shader design.

Tasks:
- Add `FillRoundedRect` and `StrokeRoundedRect` to `FastDrawingVisual.CommandProtocol/command_protocol.schema.json`.
- Regenerate `artifacts/generated/protocol/CommandProtocol.g.cs` and `CommandProtocol.g.h`.
- Extend `CommandWriter` with `WriteFillRoundedRect` and `WriteStrokeRoundedRect`.
- Update `LayeredCommandRecordingContext` so `DrawRoundedRectangle` emits rounded-rect commands instead of degrading to rectangle.
- Update shared batch compiler to parse rounded-rect commands and carry radius data into `ShapeInstance`.
- Update D3D11 and D3D9 shape type enums and shaders to distinguish plain rect from rounded rect without breaking existing rect behavior.
- Validate against `DrawingContextCoverageScenario`.

Exit criteria:
- Rounded rectangles render with visible corner radius in D3D11Share.
- Plain rectangles remain visually unchanged.

### Phase 2: Opacity Stack

Reason: cheap, isolated, improves state-stack coverage without new native presentation primitives.

Tasks:
- Maintain an opacity stack in `LayeredCommandRecordingContext`.
- Multiply outgoing fill/stroke/text alpha by current opacity.
- Keep implementation local to command recording first; avoid protocol expansion unless later required.

Exit criteria:
- `PushOpacity` affects shapes and text in coverage scene.

### Phase 3: GlyphRun Fallback Support

Reason: D3D11Share already has a text batch path through D2D/DirectWrite; a fallback implementation can be useful before exact glyph support.

Tasks:
- Extract a reasonable text fallback from `GlyphRun` when recoverable.
- Emit `DrawTextRun` as the first usable implementation.
- Document unsupported glyph-only cases that cannot map back to text.

Exit criteria:
- Coverage scenario no longer silently drops `DrawGlyphRun`.

### Phase 4: Image Commands

Reason: visible coverage gap, but needs new protocol + blob/native texture upload path.

Tasks:
- Add image draw command plus blob/image payload strategy.
- Implement managed `ImageSource` capture for frozen `BitmapSource`.
- Implement native texture upload/cache and draw path.

Exit criteria:
- `DrawImage` works for frozen bitmap sources used by the benchmark scenario.

### Phase 5: Clip Support, Rect First

Reason: `PushClip` is used in coverage scene with rectangle geometry; full arbitrary geometry clip is not needed first.

Tasks:
- Support axis-aligned rectangle clip first.
- Encode clip state in command stream or batch-local state.
- Use scissor or equivalent in native renderer.

Exit criteria:
- Coverage scene clip panel visibly clips content.

### Phase 6: Opacity Mask

Reason: requires offscreen layer/mask composition and is the most invasive state feature.

Tasks:
- Design temporary offscreen surface workflow.
- Apply mask during composite.
- Restrict first implementation to bitmap/gradient mask cases used by coverage scenario if needed.

Exit criteria:
- `PushOpacityMask` no longer behaves as a no-op in the benchmark scene.

### Phase 7: Geometry and Guideline Completeness

Reason: lowest ROI after the above.

Tasks:
- Expand `DrawGeometry` beyond the current simple geometry cases.
- Decide whether `PushGuidelineSet` stays no-op or gets raster-alignment behavior.

Exit criteria:
- Remaining unsupported geometry/state behavior is explicit and documented.

## Validation Strategy

- Primary visual check: `FastDrawingVisualApp/Benchmark/Scenarios/DrawingContextCoverageScenario.cs`
- Focus first on D3D11Share path, then keep shared/native changes compatible with D3D9 where shape protocol is shared.
- Use CLI only for managed/protocol generation validation in this environment.
- Treat Visual Studio solution build as the authoritative mixed native validation step.

## Immediate Execution Target

Start with Phase 1 and do not begin image/clip/mask work until rounded rectangle is fully encoded through protocol, managed recording, native compile, and shader dispatch.
