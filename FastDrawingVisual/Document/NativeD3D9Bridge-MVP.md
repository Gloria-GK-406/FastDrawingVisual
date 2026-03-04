# NativeD3D9 Bridge MVP Contract

This document defines the minimum native bridge contract used by `NativeD3D9Renderer`.

## Native DLL

- File name: `FastDrawingVisual.NativeProxy.dll`
- Calling convention: `cdecl`
- Export names:
  - `FDV_IsBridgeReady`
  - `FDV_GetBridgeCapabilities`
  - `FDV_CreateRenderer`
  - `FDV_DestroyRenderer`
  - `FDV_Resize`
  - `FDV_SubmitCommands`
  - `FDV_TryAcquirePresentSurface`
  - `FDV_CopyReadyToPresentSurface`
  - `FDV_OnFrontBufferAvailable`

## Export Signatures

```cpp
extern "C" {
    bool  FDV_IsBridgeReady();
    int   FDV_GetBridgeCapabilities();
    void* FDV_CreateRenderer(void* hwnd, int width, int height);
    void  FDV_DestroyRenderer(void* renderer);
    bool  FDV_Resize(void* renderer, int width, int height);
    bool  FDV_SubmitCommands(void* renderer, const void* commands, int commandBytes);
    bool  FDV_TryAcquirePresentSurface(void* renderer, void** outSurface9);
    bool  FDV_CopyReadyToPresentSurface(void* renderer);
    void  FDV_OnFrontBufferAvailable(void* renderer, bool available);
}
```

`FDV_IsBridgeReady` is a lightweight capability probe and should return `true` only when the bridge is actually usable.

`FDV_GetBridgeCapabilities` returns a bitmask. Current D3D9 bridge sets:
- bit0: command stream submission
- bit1: D3DImage-compatible present surface
- bit2: front-buffer availability notifications

`FDV_TryAcquirePresentSurface` must return `IDirect3DSurface9*` suitable for `D3DImage.SetBackBuffer`.

## Command Stream (Little Endian)

Color layout is `A,R,G,B` bytes.

Command schema single source of truth:
- `FastDrawingVisual.CommandProtocol/command_protocol.schema.json`
- Build generates:
  - `artifacts/generated/protocol/BridgeCommandProtocol.g.cs`
  - `artifacts/generated/protocol/BridgeCommandProtocol.g.h`

- `1` Clear: `[u8 cmd][u8 A][u8 R][u8 G][u8 B]`
- `2` FillRect: `[u8 cmd][f32 x][f32 y][f32 w][f32 h][A][R][G][B]`
- `3` StrokeRect: `[u8 cmd][f32 x][f32 y][f32 w][f32 h][f32 thickness][A][R][G][B]`
- `4` FillEllipse: `[u8 cmd][f32 cx][f32 cy][f32 rx][f32 ry][A][R][G][B]`
- `5` StrokeEllipse: `[u8 cmd][f32 cx][f32 cy][f32 rx][f32 ry][f32 thickness][A][R][G][B]`
- `6` Line: `[u8 cmd][f32 x0][f32 y0][f32 x1][f32 y1][f32 thickness][A][R][G][B]`

## MVP Scope

- Implemented in managed side:
  - `DrawRectangle`, `DrawRoundedRectangle` (degrades to rectangle)
  - `DrawEllipse`
  - `DrawLine`
- Not yet implemented in managed side:
  - `DrawGeometry`, `DrawImage`, `DrawText`, `DrawGlyphRun`, `DrawDrawing`
  - Clip/Opacity/Transform stack
