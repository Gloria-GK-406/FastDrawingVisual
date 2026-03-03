# FastDrawingVisual Agent Guide

## Project Intent
- This repo is an exploratory WPF-like wrapper library.
- It unifies multiple low-level drawing backends behind a stable host control API.
- Treat it as a mixed state codebase: production path + experimental path + placeholders.

## Module Status

### Primary Path (default maintenance priority)
- `FastDrawingVisual.Contracts`
- `FastDrawingVisual`
- `FastDrawingVisual.SkiaSharp`
- `FastDrawingVisual.NativeD3D9`
- `FastDrawingVisual.NativeProxy.D3D9`
- `FastDrawingVisual.WpfRenderer`
- `FastDrawingVisualApp` (demo host)

Evidence:
- `FastDrawingVisual/FastDrawingVisual.csproj` references these projects (plus DCompD3D11).
- `FastDrawingVisual/Controls/RendererFactory.cs` default runtime order is:
  - `Skia` -> `NativeD3D9` -> `WpfFallbackRenderer`

### Experimental Path (editable, but not default capability)
- `FastDrawingVisual.DCompD3D11`
- `FastDrawingVisual.WINRTProxy`

Evidence:
- DCompD3D11 is only force-selected by env var `FDV_RENDERER=DCompD3D11`.
- `DCompD3D11Renderer.SubmitDrawing(...)` is currently demo-level (no full draw contract yet).

### Placeholder / Likely Dormant
- `FastDrawingVisual.DCompD3D12`

Evidence:
- Not referenced by `FastDrawingVisual/FastDrawingVisual.csproj`.
- `DCompD3D12Renderer` is a placeholder (`AttachToElement` throws `NotImplementedException`).

## Obsolete Detection Rules
- If a project is not referenced by `FastDrawingVisual.csproj` and not reachable from `RendererFactory`, treat it as non-primary.
- If implementation is clearly stubbed (`NotImplementedException`, permanent `false`, empty behavior), classify as experimental/placeholder.
- For likely dormant modules, prefer minimal maintenance only (compile fixes, comments, docs). Avoid large refactors unless explicitly requested.

## Architecture Constraints
- Shared contracts live in `FastDrawingVisual.Contracts/Rendering/*.cs`. Do not drift backend-specific behavior from contract semantics.
- Host control entry point is `FastDrawingVisual.Controls.FastDrawingVisual`.
- Renderer lifecycle is expected to stay:
  - `Initialize(width, height)` -> `AttachToElement(...)` -> `SubmitDrawing(...)` -> `Resize(...)` -> `Dispose()`
- `SubmitDrawing` is latest-wins (replace semantics), not a queued replay model.
- `IDrawingContext` may run off the UI thread in accelerated/native paths. Respect `Freezable` thread rules (`Freeze()` before cross-thread usage).

## NativeD3D9 Protocol Rules
- Command stream is dual-ended. Any change to:
  - `NativeCommandType`
  - `NativeCommandBuffer`
  must be mirrored in native parser:
  - `FastDrawingVisual.NativeProxy.D3D9/BridgeDrawing.cpp`
- Keep `FastDrawingVisual/Document/NativeD3D9Bridge-MVP.md` in sync when protocol bytes/signatures change.
- MVP intentionally allows partial coverage (text/image/complex geometry may be no-op), but behavior must be explicit in comments.

## Build and Validation Notes
- Prerequisites for full build:
  - Visual Studio 2022 with C++ v143 toolset
  - Windows 10 SDK
  - .NET 6 Windows Desktop
  - NuGet access for packages (`SkiaSharp`, `SharpDX`, `Silk.NET`)
- Verified local commands (works offline in this environment):
  - `dotnet build FastDrawingVisual.Contracts/FastDrawingVisual.Contracts.csproj -v minimal`
  - `dotnet build FastDrawingVisual.WpfRenderer/FastDrawingVisual.WpfRenderer.csproj -v minimal`
- Package-based projects fail offline with `NU1301`; do not treat that as code regression.
- Full mixed managed/native solution build should be done via Visual Studio solution build.

## Change Priority Policy
- Fix primary path first (`Skia`, `NativeD3D9`, `WPF fallback`).
- Keep experimental path changes small, local, and reversible.
- Do not remove likely dormant projects unless explicitly asked.
- Do not reorder default renderer selection logic without a clear requirement and validation data.

## Pre-Commit Checklist
- Contract change: all affected renderers updated?
- Native protocol change: managed + native + doc updated together?
- At least one meaningful build command executed?
- PR/task note states whether the change is in primary or experimental path?
