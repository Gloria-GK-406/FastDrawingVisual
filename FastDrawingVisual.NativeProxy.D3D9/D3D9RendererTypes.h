#pragma once

#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d9.h>
#include <d3dx9.h>
#include <windows.h>

#include "BatchComplier.h"

namespace fdv::d3d9 {

constexpr int kFrameCount = 2;

enum class SurfaceState : uint8_t {
  Ready = 0,
  Drawing = 1,
  ReadyForPresent = 2,
};

struct SurfaceSlot {
  IDirect3DSurface9* renderTarget = nullptr;
  IDirect3DQuery9* renderDoneQuery = nullptr;
  SurfaceState state = SurfaceState::Ready;
};

struct D3D9RendererState {
  IDirect3D9Ex* d3d9 = nullptr;
  IDirect3DDevice9Ex* device = nullptr;
  IDirect3DVertexDeclaration9* instanceVertexDeclaration = nullptr;
  IDirect3DVertexShader9* instanceVertexShader = nullptr;
  IDirect3DPixelShader9* instancePixelShader = nullptr;
  IDirect3DVertexBuffer9* unitQuadVertexBuffer = nullptr;
  IDirect3DIndexBuffer9* unitQuadIndexBuffer = nullptr;
  IDirect3DVertexBuffer9* dynamicInstanceVertexBuffer = nullptr;
  UINT dynamicInstanceVertexCapacityBytes = 0;
  batch::BatchCompiler batchCompiler{};
  SurfaceSlot slots[kFrameCount];
  IDirect3DSurface9* presentingSurface = nullptr;

  HWND hwnd = nullptr;
  int width = 0;
  int height = 0;
  bool frontBufferAvailable = true;
  bool csInitialized = false;
  CRITICAL_SECTION cs{};
};

}  // namespace fdv::d3d9
