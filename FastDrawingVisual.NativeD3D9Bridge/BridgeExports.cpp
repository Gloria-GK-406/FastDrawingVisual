// BridgeExports.cpp
// Native D3D9 implementation + exported C ABI entry points.
// Compiled as unmanaged code for the hot rendering path.

#include <math.h> // ceilf
#include <new>
#include <stdint.h>
#include <string.h> // memcpy
#include "BridgeNativeExports.h"

// ---- Windows / Direct3D 9 headers ----------------------------------------
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d9.h>
#include <d3dx9.h>
#include <windows.h>

#if !defined(YieldProcessor)
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64) || defined(_M_AMD64))
#include <intrin.h>
#define YieldProcessor() _mm_pause()
#elif defined(_WIN32)
#include <windows.h>
#define YieldProcessor() SwitchToThread()
#else
#define YieldProcessor() ((void)0)
#endif
#endif

// The DXSDK NuGet package injects the correct lib via its .targets file,
// but list them explicitly so an IDE-only build also links correctly.
#pragma comment(lib, "d3d9.lib")

FDV_NATIVE_REGION_BEGIN

// ==========================================================================
//  Command stream constants (must match NativeCommandType.cs)
// ==========================================================================
static constexpr uint8_t CMD_CLEAR = 1;
static constexpr uint8_t CMD_FILL_RECT = 2;
static constexpr uint8_t CMD_STROKE_RECT = 3;
static constexpr uint8_t CMD_FILL_ELLIPSE = 4;
static constexpr uint8_t CMD_STROKE_ELLIPSE = 5;
static constexpr uint8_t CMD_LINE = 6;

// ==========================================================================
//  Small inline helpers for reading the packed binary command stream
// ==========================================================================
static inline float ReadF32(const uint8_t *p) {
  float v;
  memcpy(&v, p, 4);
  return v;
}
static inline D3DCOLOR ReadColor(const uint8_t *p) {
  // Wire format: A, R, G, B  (matches NativeCommandBuffer.WriteColor)
  return D3DCOLOR_ARGB(p[0], p[1], p[2], p[3]);
}

// ==========================================================================
//  D3D9 device + surface state
// ==========================================================================
static constexpr int kFrameCount = 3;

enum class SurfaceState : uint8_t {
  Ready = 0,
  Drawing = 1,
  ReadyForPresent = 2,
  Presenting = 3,
};

struct SurfaceSlot {
  IDirect3DSurface9 *renderTarget = nullptr;
  IDirect3DQuery9 *renderDoneQuery = nullptr;
  SurfaceState state = SurfaceState::Ready;
};

struct BridgeRenderer {
  IDirect3D9 *d3d9 = nullptr;
  IDirect3DDevice9 *device = nullptr;
  SurfaceSlot slots[kFrameCount];

  HWND hwnd = nullptr;
  int width = 0;
  int height = 0;
  bool frontBufferAvailable = true;
  int currentPresentingSlot = -1;
  bool csInitialized = false;
  // Guards D3D9 device access between the worker render thread
  // (FDV_SubmitCommands) and the WPF UI thread (FDV_TryAcquirePresentSurface).
  CRITICAL_SECTION cs;
};

// --------------------------------------------------------------------------
//  Forward declarations for private helpers
// --------------------------------------------------------------------------
static bool CreateDeviceAndSurface(BridgeRenderer *s);
static void ReleaseDeviceResources(BridgeRenderer *s);
static bool ResetDeviceAndSurface(BridgeRenderer *s);
static bool CreateFrameResources(BridgeRenderer *s);
static void ReleaseFrameResources(BridgeRenderer *s);
static int FindSlotByState(const BridgeRenderer *s, SurfaceState state);
static void DemoteReadyForPresentSlots(BridgeRenderer *s, int keepIndex);
static void RecycleStalePresentingSlots(BridgeRenderer *s);
static void SetupRenderState(IDirect3DDevice9 *dev);
static bool ExecuteCommands(BridgeRenderer *s, SurfaceSlot *slot,
                            const uint8_t *data, int bytes);

static void ReleaseFrameResources(BridgeRenderer *s) {
  if (!s)
    return;

  for (int i = 0; i < kFrameCount; i++) {
    if (s->slots[i].renderDoneQuery) {
      s->slots[i].renderDoneQuery->Release();
      s->slots[i].renderDoneQuery = nullptr;
    }
    if (s->slots[i].renderTarget) {
      s->slots[i].renderTarget->Release();
      s->slots[i].renderTarget = nullptr;
    }
    s->slots[i].state = SurfaceState::Ready;
  }
  s->currentPresentingSlot = -1;
}

static bool CreateFrameResources(BridgeRenderer *s) {
  if (!s || !s->device || s->width <= 0 || s->height <= 0)
    return false;

  ReleaseFrameResources(s);

  for (int i = 0; i < kFrameCount; i++) {
    HRESULT hr = s->device->CreateRenderTarget(
        (UINT)s->width, (UINT)s->height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
        FALSE, // lockable=FALSE -- D3DImage does not need CPU lock
        &s->slots[i].renderTarget, nullptr);
    if (FAILED(hr)) {
      ReleaseFrameResources(s);
      return false;
    }

    // If query creation fails, rendering still works but loses explicit GPU wait.
    s->device->CreateQuery(D3DQUERYTYPE_EVENT, &s->slots[i].renderDoneQuery);
    s->slots[i].state = SurfaceState::Ready;
  }

  return true;
}

static int FindSlotByState(const BridgeRenderer *s, SurfaceState state) {
  if (!s)
    return -1;

  for (int i = 0; i < kFrameCount; i++) {
    if (s->slots[i].state == state)
      return i;
  }
  return -1;
}

static void DemoteReadyForPresentSlots(BridgeRenderer *s, int keepIndex) {
  if (!s)
    return;

  for (int i = 0; i < kFrameCount; i++) {
    if (i == keepIndex)
      continue;
    if (s->slots[i].state == SurfaceState::ReadyForPresent)
      s->slots[i].state = SurfaceState::Ready;
  }
}

static void RecycleStalePresentingSlots(BridgeRenderer *s) {
  if (!s)
    return;

  int current = s->currentPresentingSlot;
  if (current < 0 || current >= kFrameCount ||
      s->slots[current].state != SurfaceState::Presenting) {
    current = -1;
  }
  s->currentPresentingSlot = current;

  for (int i = 0; i < kFrameCount; i++) {
    if (i == current)
      continue;
    if (s->slots[i].state == SurfaceState::Presenting)
      s->slots[i].state = SurfaceState::Ready;
  }
}

// ==========================================================================
//  Internal: device / surface creation
// ==========================================================================
static bool CreateDeviceAndSurface(BridgeRenderer *s) {
  if (s->d3d9 == nullptr) {
    s->d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (s->d3d9 == nullptr)
      return false;
  }

  // ---- Present parameters ------------------------------------------------
  // We create a render-to-texture (offscreen) setup.
  // SwapEffect_Discard with a 1x1 back-buffer satisfies WPF's D3DImage
  // requirement that the device owns a swap chain, while the real rendering
  // goes to an ARGB offscreen surface.
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.BackBufferWidth = 1;
  pp.BackBufferHeight = 1;
  pp.BackBufferCount = 1;
  pp.hDeviceWindow = s->hwnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  pp.EnableAutoDepthStencil = FALSE;

  HRESULT hr = s->d3d9->CreateDevice(
      D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, s->hwnd,
      D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED |
          D3DCREATE_FPU_PRESERVE,
      &pp, &s->device);

  if (FAILED(hr)) {
    // Fall back to software vertex processing (e.g. WARP / SWVP machines)
    pp.BackBufferWidth = 1;
    pp.BackBufferHeight = 1;
    hr = s->d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, s->hwnd,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                                   D3DCREATE_MULTITHREADED |
                                   D3DCREATE_FPU_PRESERVE,
                               &pp, &s->device);
  }

  if (FAILED(hr))
    return false;

  if (!CreateFrameResources(s)) {
    s->device->Release();
    s->device = nullptr;
    return false;
  }
  return true;
}

static void ReleaseDeviceResources(BridgeRenderer *s) {
  ReleaseFrameResources(s);
  if (s->device) {
    s->device->Release();
    s->device = nullptr;
  }
  // NOTE: cs lifetime is managed by FDV_CreateRenderer/FDV_DestroyRenderer.
}

static bool ResetDeviceAndSurface(BridgeRenderer *s) {
  ReleaseFrameResources(s);

  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.BackBufferWidth = 1;
  pp.BackBufferHeight = 1;
  pp.BackBufferCount = 1;
  pp.hDeviceWindow = s->hwnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  pp.EnableAutoDepthStencil = FALSE;

  HRESULT hr = s->device->Reset(&pp);
  if (FAILED(hr))
    return false;

  return CreateFrameResources(s);
}

// ==========================================================================
//  Render-state setup for 2-D drawing
// ==========================================================================
static void SetupRenderState(IDirect3DDevice9 *dev) {
  // Vertices use D3DFVF_XYZRHW (pre-transformed screen-space coordinates).
  // The T&L pipeline is completely bypassed, so SetTransform calls would be
  // ignored -- we skip them entirely.

  dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
  dev->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, TRUE);

  dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
}

// ==========================================================================
//  Vertex helpers
// ==========================================================================
struct Vertex2D {
  float x, y, z, rhw;
  D3DCOLOR color;
};

// Draw a filled ellipse using D3DPT_TRIANGLELIST.
// D3DPT_TRIANGLEFAN is deprecated/broken on some D3D9-on-D3D11 wrappers.
static void DrawFilledEllipse(IDirect3DDevice9 *dev, float cx, float cy,
                              float rx, float ry, D3DCOLOR color) {
  const int SEGS = 64;
  Vertex2D perim[SEGS + 1];
  for (int i = 0; i <= SEGS; ++i) {
    float a = (float)(i * 2.0f * D3DX_PI / SEGS);
    perim[i] = {cx + cosf(a) * rx, cy + sinf(a) * ry, 0.5f, 1.0f, color};
  }
  // 64 triangles * 3 verts = 192 verts
  Vertex2D verts[SEGS * 3];
  Vertex2D center = {cx, cy, 0.5f, 1.0f, color};
  for (int i = 0; i < SEGS; ++i) {
    verts[i * 3 + 0] = center;
    verts[i * 3 + 1] = perim[i];
    verts[i * 3 + 2] = perim[i + 1];
  }
  dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, SEGS, verts, sizeof(Vertex2D));
}

// Draw a stroked ellipse outline via closed LINESTRIP.
static void DrawStrokeEllipse(IDirect3DDevice9 *dev, float cx, float cy,
                              float rx, float ry, D3DCOLOR color) {
  const int SEGS = 64;
  Vertex2D verts[SEGS + 1];
  for (int i = 0; i <= SEGS; ++i) {
    float a = (float)(i * 2.0f * D3DX_PI / SEGS);
    verts[i] = {cx + cosf(a) * rx, cy + sinf(a) * ry, 0.5f, 1.0f, color};
  }
  dev->DrawPrimitiveUP(D3DPT_LINESTRIP, SEGS, verts, sizeof(Vertex2D));
}

// Helper: draw a solid filled quad (two triangles)
static void DrawFilledRect(IDirect3DDevice9 *dev, float x, float y, float w,
                           float h, D3DCOLOR color) {
  Vertex2D verts[6] = {
      {x, y, 0.5f, 1.0f, color},         {x + w, y, 0.5f, 1.0f, color},
      {x, y + h, 0.5f, 1.0f, color},     {x + w, y, 0.5f, 1.0f, color},
      {x + w, y + h, 0.5f, 1.0f, color}, {x, y + h, 0.5f, 1.0f, color},
  };
  dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, verts, sizeof(Vertex2D));
}

// Helper: draw a hollow rectangle outline with a given pixel thickness
static void DrawStrokeRect(IDirect3DDevice9 *dev, float x, float y, float w,
                           float h, float t, D3DCOLOR color) {
  // Top
  DrawFilledRect(dev, x, y, w, t, color);
  // Bottom
  DrawFilledRect(dev, x, y + h - t, w, t, color);
  // Left
  DrawFilledRect(dev, x, y + t, t, h - 2 * t, color);
  // Right
  DrawFilledRect(dev, x + w - t, y + t, t, h - 2 * t, color);
}

// ==========================================================================
//  Command decoder
// ==========================================================================
static bool ExecuteCommands(BridgeRenderer *s, SurfaceSlot *slot,
                            const uint8_t *data, int bytes) {
  IDirect3DDevice9 *dev = s->device;
  if (!dev || !slot || !slot->renderTarget)
    return false;

  // Set the offscreen surface as the current render target
  HRESULT hr = dev->SetRenderTarget(0, slot->renderTarget);
  if (FAILED(hr))
    return false;

  hr = dev->BeginScene();
  if (FAILED(hr))
    return false;

  SetupRenderState(dev);

  const uint8_t *p = data;
  const uint8_t *end = data + bytes;

  while (p < end) {
    uint8_t cmd = *p++;

    switch (cmd) {
    // ------------------------------------------------------------------
    case CMD_CLEAR: // [A,R,G,B]
    {
      if (p + 4 > end)
        goto done;
      D3DCOLOR color = ReadColor(p);
      p += 4;
      DWORD argb = color;
      dev->Clear(0, nullptr, D3DCLEAR_TARGET,
                 D3DCOLOR_ARGB((argb >> 24) & 0xFF, (argb >> 16) & 0xFF,
                               (argb >> 8) & 0xFF, argb & 0xFF),
                 1.0f, 0);
      break;
    }

    // ------------------------------------------------------------------
    case CMD_FILL_RECT: // [x,y,w,h : 4xf32][A,R,G,B]
    {
      if (p + 20 > end)
        goto done;
      float x = ReadF32(p);
      float y = ReadF32(p + 4);
      float w = ReadF32(p + 8);
      float h = ReadF32(p + 12);
      D3DCOLOR color = ReadColor(p + 16);
      p += 20;
      DrawFilledRect(dev, x, y, w, h, color);
      break;
    }

    // ------------------------------------------------------------------
    case CMD_STROKE_RECT: // [x,y,w,h : 4xf32][thickness:f32][A,R,G,B]
    {
      if (p + 24 > end)
        goto done;
      float x = ReadF32(p);
      float y = ReadF32(p + 4);
      float w = ReadF32(p + 8);
      float h = ReadF32(p + 12);
      float t = ReadF32(p + 16);
      D3DCOLOR color = ReadColor(p + 20);
      p += 24;
      if (t < 1.0f)
        t = 1.0f;
      DrawStrokeRect(dev, x, y, w, h, t, color);
      break;
    }

    // ------------------------------------------------------------------
    case CMD_FILL_ELLIPSE: // [cx,cy:2xf32][rx,ry:2xf32][A,R,G,B]
    {
      if (p + 20 > end)
        goto done;
      float cx = ReadF32(p);
      float cy = ReadF32(p + 4);
      float rx = ReadF32(p + 8);
      float ry = ReadF32(p + 12);
      D3DCOLOR color = ReadColor(p + 16);
      p += 20;
      DrawFilledEllipse(dev, cx, cy, rx, ry, color);
      break;
    }

    // ------------------------------------------------------------------
    case CMD_STROKE_ELLIPSE: // [cx,cy][rx,ry][thickness:f32][A,R,G,B]
    {
      if (p + 24 > end)
        goto done;
      float cx = ReadF32(p);
      float cy = ReadF32(p + 4);
      float rx = ReadF32(p + 8);
      float ry = ReadF32(p + 12);
      float t = ReadF32(p + 16);
      D3DCOLOR color = ReadColor(p + 20);
      p += 24;
      if (t < 1.0f)
        t = 1.0f;
      DrawStrokeEllipse(dev, cx, cy, rx - t * 0.5f, ry - t * 0.5f, color);
      break;
    }

    // ------------------------------------------------------------------
    case CMD_LINE: // [x0,y0][x1,y1][thickness:f32][A,R,G,B]
    {
      if (p + 24 > end)
        goto done;
      float x0 = ReadF32(p);
      float y0 = ReadF32(p + 4);
      float x1 = ReadF32(p + 8);
      float y1 = ReadF32(p + 12);
      float t = ReadF32(p + 16);
      D3DCOLOR color = ReadColor(p + 20);
      p += 24;

      if (t <= 1.5f) {
        // Thin line: use D3DPT_LINELIST
        Vertex2D verts[2] = {
            {x0, y0, 0.5f, 1.0f, color},
            {x1, y1, 0.5f, 1.0f, color},
        };
        dev->DrawPrimitiveUP(D3DPT_LINELIST, 1, verts, sizeof(Vertex2D));
      } else {
        // Thick line: expand into a quad
        float dx = x1 - x0, dy = y1 - y0;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f)
          break;
        float nx = -dy / len * (t * 0.5f);
        float ny = dx / len * (t * 0.5f);
        Vertex2D verts[6] = {
            {x0 + nx, y0 + ny, 0.5f, 1.0f, color},
            {x1 + nx, y1 + ny, 0.5f, 1.0f, color},
            {x0 - nx, y0 - ny, 0.5f, 1.0f, color},
            {x1 + nx, y1 + ny, 0.5f, 1.0f, color},
            {x1 - nx, y1 - ny, 0.5f, 1.0f, color},
            {x0 - nx, y0 - ny, 0.5f, 1.0f, color},
        };
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, verts, sizeof(Vertex2D));
      }
      break;
    }

    default:
      // Unknown command �� abort decoding to avoid misalignment
      goto done;
    }
  }

done:
  hr = dev->EndScene();
  if (FAILED(hr))
    return false;

  // ---- CPU-GPU synchronization ----------------------------------------
  // EndScene() only flushes CPU-side command recording; the GPU executes
  // asynchronously. We must wait until the GPU has actually finished writing
  // to the render target before letting the UI thread read it via D3DImage.
  // D3DQUERYTYPE_EVENT acts as a lightweight GPU fence for this purpose.
  if (slot->renderDoneQuery) {
    hr = slot->renderDoneQuery->Issue(D3DISSUE_END);
    if (FAILED(hr))
      return false;

    // Poll with D3DGETDATA_FLUSH to ensure commands are flushed to the GPU.
    while (true) {
      hr = slot->renderDoneQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH);
      if (hr == S_OK)
        break;
      if (hr == S_FALSE) {
        YieldProcessor(); // back-off to avoid burning a full CPU core
        continue;
      }
      return false;
    }
  }

  return true;
}

// ==========================================================================
//  Public C ABI
// ==========================================================================
extern "C" {
// -----------------------------------------------------------------------
__declspec(dllexport) bool __cdecl FDV_IsBridgeReady() {
  // The D3D9 capability check on the C# side also calls this; return true
  // to signal the implementation is complete and should be used.
  return true;
}

// -----------------------------------------------------------------------
__declspec(dllexport) void *__cdecl FDV_CreateRenderer(void *hwnd, int width,
                                                       int height) {
  if (hwnd == nullptr || width <= 0 || height <= 0)
    return nullptr;

  auto *s = new (std::nothrow) BridgeRenderer();
  if (!s)
    return nullptr;

  s->hwnd = static_cast<HWND>(hwnd);
  s->width = width;
  s->height = height;
  InitializeCriticalSectionAndSpinCount(&s->cs, 1000);
  s->csInitialized = true;

  if (!CreateDeviceAndSurface(s)) {
    DeleteCriticalSection(&s->cs);
    s->csInitialized = false;
    delete s;
    return nullptr;
  }

  return s;
}

// -----------------------------------------------------------------------
__declspec(dllexport) void __cdecl FDV_DestroyRenderer(void *renderer) {
  auto *s = static_cast<BridgeRenderer *>(renderer);
  if (!s)
    return;

  ReleaseDeviceResources(s);

  if (s->d3d9) {
    s->d3d9->Release();
    s->d3d9 = nullptr;
  }

  if (s->csInitialized) {
    DeleteCriticalSection(&s->cs);
    s->csInitialized = false;
  }
  delete s;
}

// -----------------------------------------------------------------------
__declspec(dllexport) bool __cdecl FDV_Resize(void *renderer, int width,
                                              int height) {
  auto *s = static_cast<BridgeRenderer *>(renderer);
  if (!s || width <= 0 || height <= 0)
    return false;

  EnterCriticalSection(&s->cs);

  bool ok = false;
  if (s->width == width && s->height == height) {
    ok = true;
  } else {
    s->width = width;
    s->height = height;
    if (!s->device)
      ok = CreateDeviceAndSurface(s);
    else
      ok = CreateFrameResources(s);
  }

  LeaveCriticalSection(&s->cs);
  return ok;
}

// -----------------------------------------------------------------------
__declspec(dllexport) bool __cdecl FDV_SubmitCommands(void *renderer,
                                                      const void *commands,
                                                      int commandBytes) {
  auto *s = static_cast<BridgeRenderer *>(renderer);
  if (!s || !commands || commandBytes <= 0)
    return false;
  if (!s->device)
    return false;

  EnterCriticalSection(&s->cs);
  HRESULT hr = s->device->TestCooperativeLevel();
  if (hr == D3DERR_DEVICENOTRESET) {
    if (!ResetDeviceAndSurface(s)) {
      LeaveCriticalSection(&s->cs);
      return false;
    }
  } else if (FAILED(hr)) {
    LeaveCriticalSection(&s->cs);
    return false;
  }

  int drawSlotIndex = FindSlotByState(s, SurfaceState::Ready);
  if (drawSlotIndex < 0) {
    LeaveCriticalSection(&s->cs);
    return false;
  }

  SurfaceSlot *drawSlot = &s->slots[drawSlotIndex];
  drawSlot->state = SurfaceState::Drawing;
  bool result = ExecuteCommands(s, drawSlot, static_cast<const uint8_t *>(commands),
                                commandBytes);
  if (result) {
    DemoteReadyForPresentSlots(s, drawSlotIndex);
    drawSlot->state = SurfaceState::ReadyForPresent;
  } else {
    drawSlot->state = SurfaceState::Ready;
  }

  LeaveCriticalSection(&s->cs);
  return result;
}

// -----------------------------------------------------------------------
// Called on the UI thread (WPF's CompositionTarget.Rendering callback).
// Returns a raw IDirect3DSurface9* that WPF's D3DImage can wrap.
__declspec(dllexport) bool __cdecl FDV_TryAcquirePresentSurface(
    void *renderer, void **outSurface9) {
  if (!renderer || !outSurface9)
    return false;

  auto *s = static_cast<BridgeRenderer *>(renderer);
  *outSurface9 = nullptr;

  EnterCriticalSection(&s->cs);
  bool ok = false;
  if (s->device) {
    int readySlotIndex = FindSlotByState(s, SurfaceState::ReadyForPresent);
    if (readySlotIndex >= 0 && s->slots[readySlotIndex].renderTarget) {
      s->slots[readySlotIndex].state = SurfaceState::Presenting;
      s->currentPresentingSlot = readySlotIndex;
      *outSurface9 = s->slots[readySlotIndex].renderTarget;
      ok = true;
    }
  }
  LeaveCriticalSection(&s->cs);
  return ok;
}

// -----------------------------------------------------------------------
// Called after WPF has consumed the surface (D3DImage.Unlock).
__declspec(dllexport) void __cdecl FDV_OnSurfacePresented(void *renderer) {
  // Recycle stale presenting slots after the UI has switched back-buffer.
  auto *s = static_cast<BridgeRenderer *>(renderer);
  if (!s)
    return;

  EnterCriticalSection(&s->cs);
  RecycleStalePresentingSlots(s);
  LeaveCriticalSection(&s->cs);
}

// -----------------------------------------------------------------------
__declspec(dllexport) void __cdecl FDV_OnFrontBufferAvailable(void *renderer,
                                                              bool available) {
  auto *s = static_cast<BridgeRenderer *>(renderer);
  if (!s)
    return;
  EnterCriticalSection(&s->cs);
  s->frontBufferAvailable = available;

  if (!available) {
    // Surface is going away �C mark as not ready so TryAcquirePresentSurface
    // returns false until the device is recovered.
    for (int i = 0; i < kFrameCount; i++)
      s->slots[i].state = SurfaceState::Ready;
    s->currentPresentingSlot = -1;
  } else if (s->device) {
    // Front buffer became available again; try to recover if needed.
    HRESULT hr = s->device->TestCooperativeLevel();
    if (hr == D3DERR_DEVICENOTRESET)
      ResetDeviceAndSurface(s);
  }
  LeaveCriticalSection(&s->cs);
}
}

FDV_NATIVE_REGION_END
