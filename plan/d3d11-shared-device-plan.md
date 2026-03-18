# D3D11 Shared Device Implementation Plan

Date: 2026-03-18
Scope: `FastDrawingVisual.NativeProxy.D3D11` only

## Goal

Introduce class-level shared D3D11 device ownership for the D3D11 native path while preserving current draw ordering semantics first.

This plan does not change the D3D9 primary path yet.

## Intent

Start with the lowest-risk D3D11 design:

- share `ID3D11Device`
- serialize `ID3D11DeviceContext` use
- split submission into `compile` and `execute`
- preserve current batch order through an explicit operation list

If measured performance loss is too high, the fallback path is to relax semantics later into fixed render passes.

## Current Findings

- `D3D11ShareD3D9Renderer` creates its own `ID3D11Device`, `ID3D11DeviceContext`, `IDirect3D9Ex`, and `IDirect3DDevice9Ex` per renderer instance.
- `D3D11SwapChainRenderer` creates its own `ID3D11Device` and `ID3D11DeviceContext` per renderer instance.
- The shared batch path already groups consecutive commands into `ShapeInstances`, `Text`, and `Image` batches, but execution still happens immediately after parsing.
- D3D11 can safely share `ID3D11Device`, but `ID3D11DeviceContext` work must remain serialized.
- Preserving visual results requires preserving current batch order. Global reordering into `Shape -> Image -> Text` would change semantics.

## Non-Goals

- No D3D9 renderer refactor in this task.
- No command protocol redesign.
- No global pass-based semantic rewrite in the first implementation.
- No deferred-context optimization in the first implementation.

## Target Architecture

### 1. Shared device ownership

Add shared manager types under native D3D11 code:

- `SharedD3D11DeviceManager`
- `SharedD3D11D3D9InteropManager`

Responsibilities:

- one shared `ID3D11Device`
- one shared immediate `ID3D11DeviceContext`
- initialization and rebuild lock
- execution lock for context use
- generation tracking after rebuild

`SharedD3D11D3D9InteropManager` additionally owns:

- one shared `IDirect3D9Ex`
- one shared `IDirect3DDevice9Ex`
- one stable hidden device `HWND`

Rationale:

- `D3D11SwapChainRenderer` only needs shared D3D11 device/context.
- `D3D11ShareD3D9Renderer` depends on a matched D3D11 + D3D9 interop pair and should not stitch two unrelated singleton managers together.

### 2. Compile/execute split

Refactor renderer submission into two phases:

- `CompileFrame(...)`
- `ExecuteCompiledFrame(...)`

`CompileFrame` runs without touching shared device state.
`ExecuteCompiledFrame` runs under the manager execution gate.

### 3. Immutable compiled frame

Introduce a frame-local immutable payload, for example:

- `CompiledFrame`
- `CompiledLayer`
- `CompiledOp`

`CompiledFrame` contains:

- frame size
- ordered op list preserving current batch order
- packed shape instance data
- copied text items
- copied image items and owned pixel/blob memory

`CompiledOp` should stay small and reference payload slices by index/offset instead of copying per-op data.

This keeps the submission side clean without changing draw semantics.

## Locking Model

### Initialization/rebuild lock

Used inside shared managers for:

- first create
- device lost rebuild
- hidden `HWND` create for interop path

Fast path:

- check `Ready` state and generation without heavy work
- return immediately when already initialized

Slow path:

- lock
- re-check state
- create or rebuild
- increment generation

### Execution lock

Used around all immediate-context work:

- render target binding
- viewport setup
- shader/state binding
- draw calls
- copy operations
- query end/wait
- present-surface copy for D3D11 share path

First implementation may use one coarse execution lock per shared manager.

## Ordered Work

### Phase 1: Shared compile model

Tasks:

- Add native shared frame types for D3D11 submission.
- Refactor current batch iteration so it can fully compile a single frame into owned memory.
- Preserve current batch order with an explicit op list.
- Keep `BatchCompiler` as an internal parser, not as a queued object.

Exit criteria:

- A single frame can be compiled without touching D3D11 device state.
- The compiled payload owns all text/image/blob data needed after submit returns.

### Phase 2: Shared D3D11 device managers

Tasks:

- Add `SharedD3D11DeviceManager` for swap-chain path.
- Add `SharedD3D11D3D9InteropManager` for share-to-D3D9 path.
- Move D3D11 device/context creation out of renderer instances.
- Move D3D9 interop device creation out of `D3D11ShareD3D9Renderer`.
- Add generation tracking and rebuild entry points.

Exit criteria:

- Multiple renderer instances in each D3D11 family use shared manager-owned device objects.
- Renderer destructors no longer release shared device objects.

### Phase 3: Renderer-local resource split

Tasks:

- Separate renderer-local resources from shared resources.
- Keep instance-local:
  - swap chain
  - RTV/back-buffer bindings
  - shared textures and surfaces
  - frame slot state
  - text target binding state
- Keep shared-manager-owned:
  - D3D11 device
  - D3D11 immediate context
  - interop D3D9 device for share path

Exit criteria:

- Renderer instances can be created and destroyed without affecting shared device lifetime.

### Phase 4: Execute path serialization

Tasks:

- Replace direct submit-and-draw flow with `CompileFrame` followed by `ExecuteCompiledFrame`.
- Run execute under the shared manager execution gate.
- Rebuild renderer-local resources on generation mismatch.
- Keep current visual order by replaying `CompiledOp` sequence exactly.

Exit criteria:

- Existing rendering order is preserved.
- Shared device/context usage is serialized through the manager.

### Phase 5: Validation and instrumentation

Tasks:

- Add timing around compile and execute separately.
- Measure:
  - compile time
  - execute lock wait time
  - execute duration
  - frame latency under two or more active controls
- Verify both:
  - `D3D11SwapChainRenderer`
  - `D3D11ShareD3D9Renderer`

Exit criteria:

- We have a before/after performance read on contention cost.
- No obvious visual ordering regressions.

## Performance Fallback Path

If coarse execution locking causes unacceptable loss, take the next step without changing semantics first:

### Fallback A: mailbox + executor thread

Per renderer:

- `pendingCompiledFrame`
- `executingCompiledFrame`

Behavior:

- submit side compiles a full frame
- latest frame overwrites pending frame
- executor thread drains the latest frame per renderer
- execution stays ordered within each frame

This keeps `last-wins` behavior and reduces submit-thread blocking.

## Semantic Relaxation Option

Only consider this if the preserved-order model is still too expensive after measurement.

Potential rewrite:

- fixed passes such as `Shapes -> Images -> Text`
- or explicit semantic layers exposed by the command writer

Tradeoff:

- much simpler execution
- lower state-switch churn
- but draw order changes and current painter-style behavior is no longer guaranteed

This must be treated as a rendering contract change, not an internal optimization.

## Validation Strategy

- Keep authoritative mixed validation in Visual Studio solution builds.
- Use existing demo and benchmark scenes that exercise text, image, and shape mixing.
- Add a targeted multi-control stress case for:
  - two or more D3D11 controls updating concurrently
  - overlapping shape/text/image content

## Immediate Execution Target

Implement Phases 1 through 4 for D3D11 only, preserving current batch order and using coarse execution serialization first.
