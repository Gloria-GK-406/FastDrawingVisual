# FastDrawingVisual Agent Guide

## Project Intent
- This repo is an exploratory WPF-like wrapper library.
- It unifies multiple low-level drawing backends behind a stable host control API.
- Treat it as a mixed state codebase: production path + experimental path + placeholders.

## Current Topology

### Runtime Entry and Selection
- Host control entry point: `FastDrawingVisual.Controls.FastDrawingVisual`.
- Renderer factory: `FastDrawingVisual.Controls.RendererFactory`.
- `FastDrawingVisual` exposes a `PreferredRenderer` dependency property.
- `RendererFactory.Create(RendererPreference)` behaves as:
  - `Auto` -> try `Skia`, then `NativeD3D9`, then `WpfFallbackRenderer`
  - explicit preference (`Skia` / `NativeD3D9` / `DCompD3D11`) -> try only that renderer family
  - if the preferred renderer is unsupported or initialization/attach fails -> fallback to `WpfFallbackRenderer`
- No environment-variable renderer override is used.

### Primary Path (default maintenance priority)
- `FastDrawingVisual.Contracts`
- `FastDrawingVisual`
- `FastDrawingVisual.SkiaSharp`
- `FastDrawingVisual.NativeD3D9`
- `FastDrawingVisual.NativeProxy.D3D9`
- `FastDrawingVisual.WpfRenderer`
- `FastDrawingVisual.CommandProtocol` (shared command schema/generator)
- `FastDrawingVisualApp` (demo host)

Evidence:
- `FastDrawingVisual/FastDrawingVisual.csproj` references Contracts + DCompD3D11 + WpfRenderer + NativeD3D9 + SkiaSharp.
- Default runtime path still prefers `Skia` / `NativeD3D9` / `WpfFallbackRenderer`.
- `NativeD3D9` + `NativeProxy.D3D9` consume generated protocol outputs from `FastDrawingVisual.CommandProtocol`.

### Experimental Path (editable, but not default capability)
- `FastDrawingVisual.DCompD3D11`
- `FastDrawingVisual.NativeProxy.D3D11`
- `FastDrawingVisual.WINRTProxy`
- `FastDrawingVisual.LogCore`
- `FastDrawingVisual.LogClrProxy`

Evidence:
- DCompD3D11 is only reachable via explicit `PreferredRenderer=DCompD3D11`.
- `DCompD3D11` depends on `NativeProxy.D3D11` + `LogClrProxy` and runtime-loads `WINRTProxy`.
- `DCompDrawingContext` intentionally has partial `IDrawingContext` coverage (several APIs are explicit no-op in MVP scope).

### Placeholder / Likely Dormant
- `FastDrawingVisual.DCompD3D12`

Evidence:
- Not referenced by `FastDrawingVisual/FastDrawingVisual.csproj`.
- `DCompD3D12Renderer` is a placeholder (`AttachToElement` throws `NotImplementedException`).

## Obsolete Detection Rules
- Primary status is based on default runtime reachability from `RendererFactory` plus required build-time infrastructure.
- If a project is only reachable via explicit opt-in (for example `PreferredRenderer=DCompD3D11`), treat it as experimental.
- If implementation is clearly stubbed (`NotImplementedException`, permanent `false`, empty behavior), classify as experimental/placeholder.
- For likely dormant modules, prefer minimal maintenance only (compile fixes, comments, docs). Avoid large refactors unless explicitly requested.

## Architecture Constraints
- Shared contracts live in `FastDrawingVisual.Contracts/Rendering/*.cs`. Do not drift backend-specific behavior from contract semantics.
- Host control entry point is `FastDrawingVisual.Controls.FastDrawingVisual`.
- Runtime capability probing lives in the main `FastDrawingVisual` assembly, not in `Contracts`.
- Renderer lifecycle is expected to stay:
  - `Initialize(width, height)` -> `AttachToElement(...)` -> `SubmitDrawing(...)` -> `Resize(...)` -> `Dispose()`
- `SubmitDrawing` is latest-wins (replace semantics), not a queued replay model.
- `IDrawingContext` may run off the UI thread in accelerated/native paths. Respect `Freezable` thread rules (`Freeze()` before cross-thread usage).

## Command Protocol Rules
- `FastDrawingVisual.CommandProtocol/command_protocol.schema.json` is the source of truth for shared command IDs/layout.
- Any shared command protocol change must be updated together across:
  - `FastDrawingVisual.CommandProtocol` generator outputs (`BridgeCommandProtocol.g.cs/.g.h`)
  - managed writers (`BridgeCommandBufferWriter`, related drawing contexts)
  - native parsers (`FastDrawingVisual.NativeProxy.D3D9/BridgeDrawing.cpp`, and D3D11 paths consuming `fdv::protocol` constants)
- Shared protocol is v2 fixed-slot plus external blob buffer; native exports consume command buffer and blob buffer together.
- Keep `FastDrawingVisual/Document/NativeD3D9Bridge-MVP.md` in sync when NativeD3D9 protocol semantics change.

## Build and Validation Notes
- Prerequisites for full build:
  - Visual Studio 2022 with C++ v143 toolset
  - C++/CLI support (for `FastDrawingVisual.LogClrProxy`)
  - C++/WinRT support (for `FastDrawingVisual.WINRTProxy`)
  - Windows 10 SDK
  - .NET 6 Windows Desktop
  - NuGet access for package-based projects (`SkiaSharp`, `SharpDX.*`)
- Project status baseline: builds are considered healthy unless there is a reproduced failure from Visual Studio solution build.
- CLI in this sandbox is not authoritative for C++/mixed projects. Do not infer regression from CLI-only build failures.
- Full mixed managed/native validation should be done via Visual Studio solution build output.
- Use local CLI build only when explicitly requested and scoped to managed-only projects.

## Change Priority Policy
- Fix primary path first (`Skia`, `NativeD3D9`, `WPF fallback`).
- Keep experimental path changes small, local, and reversible.
- Do not remove likely dormant projects unless explicitly asked.
- Do not reorder default renderer selection logic without a clear requirement and validation data.

## Pre-Commit Checklist
- Contract change: all affected renderers updated?
- Shared command protocol change: schema + generated outputs + managed + native parser updated together?
- DComp text command extension change: managed writer + native parser both updated?
- Validation evidence captured (local managed build or user-provided VS solution build output)?
- PR/task note states whether the change is in primary or experimental path?
