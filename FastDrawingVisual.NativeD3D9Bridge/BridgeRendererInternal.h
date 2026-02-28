#pragma once

#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d9.h>
#include <d3dx9.h>
#include <windows.h>

constexpr int kFrameCount = 3;

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
  IDirect3DPixelShader9 *sdfEllipseShader = nullptr;
  IDirect3DPixelShader9 *sdfLineShader = nullptr;
  SurfaceSlot slots[kFrameCount];

  HWND hwnd = nullptr;
  int width = 0;
  int height = 0;
  bool frontBufferAvailable = true;
  int currentPresentingSlot = -1;
  bool csInitialized = false;
  CRITICAL_SECTION cs;
};

bool CreateDeviceAndSurface(BridgeRenderer *s);
void ReleaseDeviceResources(BridgeRenderer *s);
bool ResetDeviceAndSurface(BridgeRenderer *s);

void ReleaseFrameResources(BridgeRenderer *s);
bool CreateFrameResources(BridgeRenderer *s);

int FindSlotByState(const BridgeRenderer *s, SurfaceState state);
void DemoteReadyForPresentSlots(BridgeRenderer *s, int keepIndex);
void RecycleStalePresentingSlots(BridgeRenderer *s);

void SetupRenderState(IDirect3DDevice9 *dev);
bool ExecuteCommands(BridgeRenderer *s, SurfaceSlot *slot, const uint8_t *data,
                     int bytes);
