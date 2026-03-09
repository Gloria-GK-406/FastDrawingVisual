# Layered Drawing Context Plan

## Goal

Introduce an optional layered drawing capability on top of the existing `IDrawingContext` contract, so high-pressure renderers can batch more aggressively without giving up the generic drawing API shape.

This plan targets the primary accelerated paths first:

- `FastDrawingVisual.DCompD3D11`
- `FastDrawingVisual.NativeD3D9`

`FastDrawingVisual.WpfRenderer` remains compatibility-first and is not required to implement layered drawing support.

## Motivation

Current hot paths still pay high CPU submission cost because a frame is recorded as a flat stream of small commands:

- many `DrawLine`
- many `DrawRectangle`
- many `StrokeRect`
- many ellipse commands

Even after backend-side batching improvements, a flat command stream still mixes unrelated visual layers together. That limits safe batching because the renderer must preserve FIFO order across the entire frame.

Layered recording gives the renderer a stronger ordering structure:

- layer-to-layer order is explicit and stable
- layer-internal order is preserved
- batching can be more aggressive inside one layer

## Design Principles

1. Preserve the existing `IDrawingContext` contract as the baseline API.
2. Layer support must be an explicit optional capability, not an implicit degraded behavior.
3. A layer is a logical ordering bucket, not a separate render target by default.
4. Final output is still rendered into one final target/RTV in layer order.
5. Root context owns submission lifetime; layer contexts are views into per-layer buckets.
6. Unsupported backends should not pretend to support layered semantics.

## Proposed API Shape

Keep the existing base contract unchanged:

```csharp
public interface IDrawingContext : IDisposable
{
    int Width { get; }
    int Height { get; }

    void DrawEllipse(Brush brush, Pen pen, Point center, double radiusX, double radiusY);
    void DrawRectangle(Brush brush, Pen pen, Rect rectangle);
    void DrawRoundedRectangle(Brush brush, Pen pen, Rect rectangle, double radiusX, double radiusY);
    void DrawLine(Pen pen, Point point0, Point point1);
    void DrawGeometry(Brush brush, Pen pen, Geometry geometry);
    void DrawImage(ImageSource imageSource, Rect rectangle);
    void DrawText(string text, Point origin, Brush foreground, string fontFamily = "Segoe UI", double fontSize = 12);
    void DrawGlyphRun(Brush foregroundBrush, GlyphRun glyphRun);
    void DrawDrawing(Drawing drawing);
    void PushClip(Geometry clipGeometry);
    void PushGuidelineSet(GuidelineSet guidelines);
    void PushOpacity(double opacity);
    void PushOpacityMask(Brush opacityMask);
    void PushTransform(Transform transform);
    void Pop();
    void Close();
}
```

Add an optional capability interface:

```csharp
public interface ILayeredDrawingContextContainer
{
    int LayerCount { get; }
    IDrawingContext GetLayer(int layerIndex);
}
```

Usage pattern:

```csharp
void Render(IDrawingContext ctx)
{
    ctx.DrawRectangle(...);

    if (ctx is ILayeredDrawingContextContainer layered)
    {
        var overlay = layered.GetLayer(6);
        overlay.DrawLine(...);
        overlay.DrawText(...);
    }
}
```

## Layer Semantics

Recommended fixed layer count for MVP:

- `0..7`

Layer rules:

1. Lower layer index draws first.
2. Higher layer index draws later and visually overlays lower layers.
3. Layer order is strict.
4. Within one layer, command order remains FIFO.
5. Renderer may batch within one layer as long as FIFO semantics inside that layer are preserved.
6. By default, the root `IDrawingContext` writes into layer `0`.

This is ordering-only. It does not imply:

- separate RTV per layer
- automatic caching
- automatic alpha compositing passes
- independent present behavior

## Lifetime Model

Root drawing context owns the frame lifetime.

Layer contexts are only valid while the root context is alive.

Rules:

1. `GetLayer()` is valid only before root context `Dispose/Close`.
2. Root context `Dispose/Close` submits all layer buckets in layer order.
3. Layer contexts do not independently submit the frame.
4. Repeated `GetLayer(i)` during one frame returns the same effective bucket target.
5. Access after root disposal throws `ObjectDisposedException`.

Recommended implementation behavior:

- layer context disposal is a lightweight no-op or local closed-state only
- actual backend submission remains root-owned

## Backend Capability Policy

### DCompD3D11

Should implement `ILayeredDrawingContextContainer`.

Reason:

- primary accelerated path
- already benefits from backend batching
- layered recording naturally improves further batching opportunities

### NativeD3D9

Should implement `ILayeredDrawingContextContainer`.

Reason:

- primary accelerated path
- same command-style recording model
- can consume per-layer buckets and apply the same ordering model

### WpfRenderer

Do not implement `ILayeredDrawingContextContainer` in MVP.

Reason:

- compatibility path
- no strong batching upside
- fake support would create ambiguous semantics

Callers should use capability checks:

```csharp
if (ctx is ILayeredDrawingContextContainer layered)
{
    ...
}
```

Avoid a silent fallback where unsupported backends accept layered calls but flatten everything into one FIFO stream without telling the caller.

## Internal Recording Model

For supporting backends, root context owns per-layer buckets:

```text
root context
  layer 0 -> command bucket
  layer 1 -> command bucket
  ...
  layer 7 -> command bucket
```

Each layer context writes into one bucket only.

At submission time:

1. consume layer 0
2. consume layer 1
3. ...
4. consume layer 7

Within each layer:

- preserve command order
- apply backend-side batching
- treat text / clip / transform / special commands as batch barriers where needed

## Rendering Model

Layering is pass ordering on the same final target.

Default execution model:

1. begin frame on one final target
2. replay layer 0 commands
3. replay layer 1 commands
4. ...
5. replay layer 7 commands
6. present

This is not a multi-RTV composition system in MVP.

Multi-RTV or offscreen layer rendering should only be considered later if needed for:

- caching static layers
- post-processing
- blur/glow/shadow
- partial redraw reuse

## Batching Policy

Layering does not replace batching. It improves batching safety.

Recommended batching strategy:

1. Treat each layer as a hard ordering barrier.
2. Within one layer, batch compatible triangle primitives.
3. Flush on state barriers such as:
   - text draw
   - image draw
   - clip change
   - transform change
   - opacity change
   - future texture/material changes

MVP command classes likely to batch well:

- fill rect
- stroke rect
- line
- fill ellipse
- stroke ellipse

## Proposed Implementation Phases

### Phase 1: API and Recording Scaffolding

Scope:

- add `ILayeredDrawingContextContainer`
- add root/layer context ownership model
- implement per-layer bucket recording in DCompD3D11
- keep root context behavior unchanged for callers not using layers

Expected result:

- no behavior change for old callers
- new callers can explicitly record into layers

### Phase 2: DCompD3D11 Consumption

Scope:

- consume per-layer buckets in strict layer order
- keep same final RTV
- extend current batching to flush by layer boundary

Expected result:

- lower draw-call count for structured scenes
- clearer behavior for overlays and chart subparts

### Phase 3: NativeD3D9 Consumption

Scope:

- mirror the same per-layer bucket model
- consume buckets in layer order
- reuse or extend D3D9-side batching strategy where possible

Expected result:

- consistent API across both accelerated paths
- explicit structure for latest-wins chart workloads

### Phase 4: Scenario-Level Adoption

Scope:

- update benchmark or demo scenarios to use layers intentionally
- example mapping:
  - layer 0: canvas/panel backgrounds
  - layer 1: grids
  - layer 2: candle bodies and wicks
  - layer 3: volume
  - layer 4: overlays
  - layer 5: indicators
  - layer 6: text/labels
  - layer 7: interaction/crosshair

Expected result:

- real-world validation of the feature
- clearer measurement of batching gains

## Risks

### 1. Ambiguous API Semantics

If unsupported renderers expose the API but flatten behavior silently, callers will assume guarantees that do not exist.

Mitigation:

- capability interface only
- no fake layered support in unsupported backends

### 2. Lifetime Confusion

If layer contexts can outlive the root context, ownership becomes error-prone.

Mitigation:

- root-owned lifetime
- disposed root invalidates all layer contexts

### 3. Batch Fragmentation

If each `GetLayer()` call is treated as a hard batch split, benefits will be limited.

Mitigation:

- layer is a bucket target, not a batch instance
- renderer keeps control of actual flush timing inside the layer

### 4. Future State Stack Semantics

Current accelerated backends already have partial `Push/Pop` semantics. Layering should not make that ambiguity worse.

Mitigation:

- document current scope clearly
- treat unsupported stack-affecting commands as conservative flush barriers where needed

## Validation Plan

Validation should focus on the primary path first.

Measure before/after on:

- DCompD3D11
- NativeD3D9

Metrics:

- execute Hz
- drop rate
- end-to-end latency
- root-recorded draw duration
- native submit duration when available
- draw-call count if instrumented later

Benchmark scenarios:

- K-Line Stress
- K-Line Live Append
- Primitive Stress

## Open Questions

1. Should layer count be fixed at 8 in contract, or configurable with a fixed minimum?
2. Should `GetLayer(0)` return the same effective destination as the root context, or should root be separate from indexed layers?
3. Do we want a small enum of recommended semantic layers for app authors, or keep layers purely numeric?
4. Should text remain a hard layer-internal batch barrier in both D3D11 and D3D9 long term?

## Recommendation

Proceed with the optional capability design:

- keep `IDrawingContext` stable
- add `ILayeredDrawingContextContainer`
- implement it only on accelerated backends
- keep one final render target
- use layers as ordering buckets, not offscreen surfaces

This gives a clean path to better batching without abandoning the generic drawing API.
