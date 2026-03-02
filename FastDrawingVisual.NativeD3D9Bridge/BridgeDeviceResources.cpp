#include "BridgeRendererInternal.h"

#include "BridgePathUtils.h"

static void ReleaseSdfShaders(BridgeRenderer *s) {
  if (!s)
    return;

  if (s->sdfEllipseShader) {
    s->sdfEllipseShader->Release();
    s->sdfEllipseShader = nullptr;
  }

  if (s->sdfLineShader) {
    s->sdfLineShader->Release();
    s->sdfLineShader = nullptr;
  }
}

static bool CompileShaderVariant(IDirect3DDevice9 *dev, const char *csoFileName,
                                 IDirect3DPixelShader9 **outShader) {
  if (!dev || !csoFileName || !outShader)
    return false;

  *outShader = nullptr;

  char csoPath[MAX_PATH] = {};
  if (!ResolveShaderCsoPathA(csoFileName, csoPath, _countof(csoPath))) {
    OutputDebugStringA("FDV: shader cso not found: ");
    OutputDebugStringA(csoFileName);
    OutputDebugStringA("\n");
    return false;
  }

  ID3DXBuffer *bytecode = nullptr;
  if (!ReadFileToBufferA(csoPath, &bytecode) || !bytecode) {
    OutputDebugStringA("FDV: failed to read shader cso: ");
    OutputDebugStringA(csoPath);
    OutputDebugStringA("\n");
    return false;
  }

  HRESULT hr = dev->CreatePixelShader(
      static_cast<const DWORD *>(bytecode->GetBufferPointer()), outShader);
  bytecode->Release();

  return SUCCEEDED(hr) && *outShader != nullptr;
}

static bool CreateSdfShaders(BridgeRenderer *s) {
  if (!s || !s->device)
    return false;

  ReleaseSdfShaders(s);

  if (!CompileShaderVariant(s->device, "PixelShader_ellipse.cso",
                            &s->sdfEllipseShader)) {
    ReleaseSdfShaders(s);
    return false;
  }

  if (!CompileShaderVariant(s->device, "PixelShader_line.cso",
                            &s->sdfLineShader)) {
    ReleaseSdfShaders(s);
    return false;
  }

  return true;
}

void ReleaseFrameResources(BridgeRenderer *s) {
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

bool CreateFrameResources(BridgeRenderer *s) {
  if (!s || !s->device || s->width <= 0 || s->height <= 0)
    return false;

  ReleaseFrameResources(s);

  for (int i = 0; i < kFrameCount; i++) {
    HRESULT hr = s->device->CreateRenderTarget(
        static_cast<UINT>(s->width), static_cast<UINT>(s->height),
        D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
        FALSE,
        &s->slots[i].renderTarget, nullptr);

    if (FAILED(hr)) {
      ReleaseFrameResources(s);
      return false;
    }

    s->device->CreateQuery(D3DQUERYTYPE_EVENT, &s->slots[i].renderDoneQuery);
    s->slots[i].state = SurfaceState::Ready;
  }

  const int presentingSlotIndex = kFrameCount - 1;
  s->slots[presentingSlotIndex].state = SurfaceState::Presenting;
  s->currentPresentingSlot = presentingSlotIndex;

  return true;
}

int FindSlotByState(const BridgeRenderer *s, SurfaceState state) {
  if (!s)
    return -1;

  for (int i = 0; i < kFrameCount; i++) {
    if (s->slots[i].state == state)
      return i;
  }

  return -1;
}

void DemoteReadyForPresentSlots(BridgeRenderer *s, int keepIndex) {
  if (!s)
    return;

  for (int i = 0; i < kFrameCount; i++) {
    if (i == keepIndex)
      continue;

    if (s->slots[i].state == SurfaceState::ReadyForPresent)
      s->slots[i].state = SurfaceState::Ready;
  }
}

void RecycleStalePresentingSlots(BridgeRenderer *s) {
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

bool CreateDeviceAndSurface(BridgeRenderer *s) {
  if (s->d3d9 == nullptr) {
    s->d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (s->d3d9 == nullptr)
      return false;
  }

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

  if (!CreateSdfShaders(s)) {
    s->device->Release();
    s->device = nullptr;
    return false;
  }

  if (!CreateFrameResources(s)) {
    ReleaseSdfShaders(s);
    s->device->Release();
    s->device = nullptr;
    return false;
  }

  return true;
}

void ReleaseDeviceResources(BridgeRenderer *s) {
  ReleaseFrameResources(s);
  ReleaseSdfShaders(s);

  if (s->device) {
    s->device->Release();
    s->device = nullptr;
  }
}

bool ResetDeviceAndSurface(BridgeRenderer *s) {
  ReleaseFrameResources(s);
  ReleaseSdfShaders(s);

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

  if (!CreateSdfShaders(s))
    return false;

  return CreateFrameResources(s);
}
