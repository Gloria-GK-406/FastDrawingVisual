// BridgeExports.cpp
// C++/CLI mixed-mode DLL.
// The managed part exposes BridgeMetadata (API version constant consumed by the
// C# capability-check layer).  The unmanaged part implements the flat C ABI
// that NativeD3D9Bridge.cs P/Invokes into.

#include <atomic>
#include <math.h> // ceilf
#include <new>
#include <stdint.h>
#include <string.h> // memcpy

// ---- Windows / Direct3D 9 headers ----------------------------------------
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d9.h>
#include <d3dx9.h>
#include <windows.h>

// The DXSDK NuGet package injects the correct lib via its .targets file,
// but list them explicitly so an IDE-only build also links correctly.
#pragma comment(lib, "d3d9.lib")

// ---- Managed metadata exposed to C# --------------------------------------
using namespace System;

namespace FastDrawingVisual::NativeD3D9Bridge {
public
ref class BridgeMetadata abstract sealed {
public:
  literal int ApiVersion = 1;
};
} // namespace FastDrawingVisual::NativeD3D9Bridge

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
struct BridgeRenderer {
  IDirect3D9 *d3d9 = nullptr;
  IDirect3DDevice9 *device = nullptr;
  IDirect3DSurface9 *renderTarget = nullptr; // ARGB offscreen surface
  IDirect3DSurface9 *depthStencil =
      nullptr; // optional Z-buffer (not strictly needed for 2-D)

  HWND hwnd = nullptr;
  int width = 0;
  int height = 0;
  bool frontBufferAvailable = true;
  // Written by worker thread, read by UI thread -- must be atomic.
  std::atomic<bool> surfaceReady{false};
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
static void SetupRenderState(IDirect3DDevice9 *dev, int w, int h);
static bool ExecuteCommands(BridgeRenderer *s, const uint8_t *data, int bytes);

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

  // ---- ARGB render target surface ---------------------------------------
  // D3DImage.SetBackBuffer(IDirect3DSurface9) requires the surface to be a
  // proper render target (not an offscreen plain surface).
  hr = s->device->CreateRenderTarget(
      (UINT)s->width, (UINT)s->height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
      FALSE, // lockable=FALSE -- D3DImage does not need CPU lock
      &s->renderTarget, nullptr);

  if (FAILED(hr)) {
    s->device->Release();
    s->device = nullptr;
    return false;
  }

  InitializeCriticalSectionAndSpinCount(&s->cs, 1000);
  s->surfaceReady.store(false, std::memory_order_release);
  return true;
}

static void ReleaseDeviceResources(BridgeRenderer *s) {
  if (s->renderTarget) {
    s->renderTarget->Release();
    s->renderTarget = nullptr;
  }
  if (s->depthStencil) {
    s->depthStencil->Release();
    s->depthStencil = nullptr;
  }
  if (s->device) {
    s->device->Release();
    s->device = nullptr;
  }
  s->surfaceReady.store(false, std::memory_order_release);
}

static bool ResetDeviceAndSurface(BridgeRenderer *s) {
  if (s->renderTarget) {
    s->renderTarget->Release();
    s->renderTarget = nullptr;
  }
  if (s->depthStencil) {
    s->depthStencil->Release();
    s->depthStencil = nullptr;
  }
  s->surfaceReady.store(false, std::memory_order_release);

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

  hr = s->device->CreateRenderTarget((UINT)s->width, (UINT)s->height,
                                     D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
                                     FALSE, &s->renderTarget, nullptr);

  bool ok = SUCCEEDED(hr);
  s->surfaceReady.store(ok, std::memory_order_release);
  return ok;
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
static bool ExecuteCommands(BridgeRenderer *s, const uint8_t *data, int bytes) {
  IDirect3DDevice9 *dev = s->device;

  // Set the offscreen surface as the current render target
  HRESULT hr = dev->SetRenderTarget(0, s->renderTarget);
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
  dev->EndScene();
  s->surfaceReady.store(true, std::memory_order_release);
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

  if (!CreateDeviceAndSurface(s)) {
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

  DeleteCriticalSection(&s->cs);
  delete s;
}

// -----------------------------------------------------------------------
__declspec(dllexport) bool __cdecl FDV_Resize(void *renderer, int width,
                                              int height) {
  auto *s = static_cast<BridgeRenderer *>(renderer);
  if (!s || width <= 0 || height <= 0)
    return false;

  if (s->width == width && s->height == height)
    return true;

  s->width = width;
  s->height = height;

  if (!s->device)
    return CreateDeviceAndSurface(s);

  // Release the old surface and recreate it at the new size.
  // A full device Reset is not needed just for an offscreen surface resize.
  if (s->renderTarget) {
    s->renderTarget->Release();
    s->renderTarget = nullptr;
  }
  s->surfaceReady.store(false, std::memory_order_release);

  HRESULT hr = s->device->CreateRenderTarget(
      (UINT)width, (UINT)height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
      &s->renderTarget, nullptr);

  return SUCCEEDED(hr);
}

// -----------------------------------------------------------------------
__declspec(dllexport) bool __cdecl FDV_SubmitCommands(void *renderer,
                                                      const void *commands,
                                                      int commandBytes) {
  auto *s = static_cast<BridgeRenderer *>(renderer);
  if (!s || !commands || commandBytes <= 0)
    return false;
  if (!s->device || !s->renderTarget)
    return false;

  // Handle device-lost
  HRESULT hr = s->device->TestCooperativeLevel();
  if (hr == D3DERR_DEVICENOTRESET) {
    if (!ResetDeviceAndSurface(s))
      return false;
  } else if (FAILED(hr)) {
    return false; // D3DERR_DEVICELOST �C wait for next call
  }

  EnterCriticalSection(&s->cs);
  bool result =
      ExecuteCommands(s, static_cast<const uint8_t *>(commands), commandBytes);
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

  // Lock to prevent the worker thread from rendering while WPF reads the
  // surface.
  EnterCriticalSection(&s->cs);
  bool ok = s->device && s->renderTarget &&
            s->surfaceReady.load(std::memory_order_acquire);
  if (ok)
    *outSurface9 = s->renderTarget;
  LeaveCriticalSection(&s->cs);
  return ok;
}

// -----------------------------------------------------------------------
// Called after WPF has consumed the surface (D3DImage.Unlock).
__declspec(dllexport) void __cdecl FDV_OnSurfacePresented(void *renderer) {
  // Nothing to do for an offscreen surface �� the same surface is reused
  // for the next frame.  A future double-buffered implementation would
  // swap front/back here.
  (void)renderer;
}

// -----------------------------------------------------------------------
__declspec(dllexport) void __cdecl FDV_OnFrontBufferAvailable(void *renderer,
                                                              bool available) {
  auto *s = static_cast<BridgeRenderer *>(renderer);
  if (!s)
    return;
  s->frontBufferAvailable = available;

  if (!available) {
    // Surface is going away �C mark as not ready so TryAcquirePresentSurface
    // returns false until the device is recovered.
    s->surfaceReady.store(false, std::memory_order_release);
  } else if (s->device) {
    // Front buffer became available again; try to recover if needed.
    HRESULT hr = s->device->TestCooperativeLevel();
    if (hr == D3DERR_DEVICENOTRESET)
      ResetDeviceAndSurface(s);
  }
}
}
