# D3D11 Fixed-Pass Shape Image Text Plan

Date: 2026-03-18
Scope: `FastDrawingVisual.NativeProxy.D3D11` only

## Goal

Replace the order-preserving D3D11 submission model with a simpler fixed-pass model:

- draw all shapes first
- draw all images second
- draw all text last

This plan explicitly accepts a rendering semantic change in exchange for a much simpler shared-device execution model.

## New Rendering Contract

For the D3D11 path only, frame content will be composed in three passes:

1. `Shape`
2. `Image`
3. `Text`

Implications:

- original command order is no longer preserved across categories
- text is always top-most within a layer/frame
- images always render above shapes
- shapes cannot cover images or text unless a future semantic extension is added

This must be treated as an intentional D3D11 rendering contract, not as an implementation detail.

## Rationale

The previous order-preserving shared-device design is correct but significantly more complex:

- compile/execute split with ordered ops
- executor or coarse context serialization
- more frame bookkeeping
- more sensitivity to cross-category interleaving

The fixed-pass model keeps the D3D11 device layer simple:

- one render target/view context
- one shape buffer payload
- one image payload list
- one text payload list
- one straightforward execute sequence

This is a better fit if the main objective is reducing renderer complexity first.

## Current Findings

- The shared command pipeline already separates output into shape instances, image items, and text items during batch compilation.
- Current execution still preserves category boundaries in original order.
- D3D11 share and swap-chain renderers both already have native paths for:
  - shape drawing
  - image drawing
  - text drawing
- The major complexity comes from preserving interleaved category ordering, not from the category renderers themselves.

## Non-Goals

- No D3D9 renderer changes.
- No attempt to preserve original painter-order semantics across categories.
- No full clip/layer system in the first implementation.
- No protocol redesign in the first implementation unless needed to support the simplified frame payload.

## Target Architecture

### 1. Simplified frame payload

Create a D3D11 frame payload type with only three content groups:

- `ShapePassData`
- `ImagePassData`
- `TextPassData`

The frame payload also includes:

- target size
- clear color or clear commands
- frame-local owned memory for text/image payload data

The submission side should be free to recycle buffers and vectors aggressively.

### 2. Shared device manager

Keep D3D11 shared-device ownership, but simplify execution assumptions:

- shared `ID3D11Device`
- shared immediate `ID3D11DeviceContext`
- coarse execution lock
- generation tracking on rebuild

For `D3D11ShareD3D9Renderer`, keep a dedicated interop manager that owns:

- shared D3D11 device/context
- shared D3D9 interop device
- hidden stable device `HWND`

### 3. Fixed execute sequence

Each compiled frame executes as:

1. clear target if needed
2. bind shape pipeline and draw all shapes
3. bind image/text path and draw all images
4. bind text path and draw all text

No cross-category order replay is needed.

## Ordered Work

### Phase 1: Define the fixed-pass frame model

Tasks:

- Add D3D11-specific frame payload types for shapes, images, and text.
- Make ownership explicit so frame payloads survive after submit returns.
- Keep shape payload packed and reusable for buffer upload.

Exit criteria:

- A single submitted frame can be represented without any ordered op list.

### Phase 2: Refactor compilation into pass buckets

Tasks:

- Change the D3D11 submission compiler path so it accumulates:
  - all shapes into one shape bucket
  - all images into one image bucket
  - all text into one text bucket
- Preserve layer/frame-local data needed by the native renderers.
- Deep-copy image/text/blob data into owned frame memory.

Exit criteria:

- D3D11 submission produces a compact frame object with three pass buckets.

### Phase 3: Shared D3D11 device ownership

Tasks:

- Add shared manager for swap-chain D3D11 path.
- Add shared interop manager for D3D11 share-to-D3D9 path.
- Move instance-local device creation out of renderers.
- Keep renderer-local resources limited to:
  - swap chain or shared textures/surfaces
  - render target views
  - per-frame slot state
  - text target state

Exit criteria:

- Multiple D3D11 renderer instances use shared manager-owned devices.

### Phase 4: Simplify execute path

Tasks:

- Replace current ordered batch replay with direct pass execution.
- Execute under one coarse lock on the shared immediate context.
- Upload and draw shape buffer first.
- Draw images next.
- Draw text last.

Exit criteria:

- D3D11 frame execution becomes a simple fixed sequence.

### Phase 5: Validation and measurement

Tasks:

- Verify expected semantic change with mixed shape/image/text scenes.
- Measure lock wait and total execute time with multiple controls.
- Compare implementation complexity and runtime cost against the previous design.

Exit criteria:

- The simplified path is functionally stable and easier to maintain.

## Semantic Risks

The following cases will render differently after this change:

- shapes intended to overlay text
- shapes intended to overlay images
- images intended to overlay text
- interleaved commands relying on painter order between categories

These are accepted risks for this plan.

## Future Extension Path

If later you need text to be partially covered or constrained, prefer adding an explicit D2D-side clip or overlay semantic instead of reintroducing full painter-order replay.

Possible follow-up features:

- explicit clip region around text/image draws
- explicit overlay/background command groups
- opt-in semantic layers from the command writer

This keeps the runtime model simple while enabling targeted exceptions.

## Validation Strategy

- Use D3D11-only scenes with mixed shapes, images, and text.
- Add at least one case that intentionally demonstrates the new stacking rule.
- Treat Visual Studio solution build as the authoritative mixed native validation step.

## Immediate Execution Target

Implement the D3D11 fixed-pass frame model first, then move device ownership into shared managers, and only after that wire both D3D11 renderers to the new pass execution path.
