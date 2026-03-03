#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

struct BridgeRendererD3D11 {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  IDXGIFactory2* dxgiFactory = nullptr;
  IDXGISwapChain1* swapChain = nullptr;
  ID3D11RenderTargetView* rtv0 = nullptr;

  int width = 0;
  int height = 0;
  HRESULT lastErrorHr = S_OK;

  bool csInitialized = false;
  CRITICAL_SECTION cs;
};

bool CreateDeviceAndSwapChain(BridgeRendererD3D11* s);
void ReleaseRendererResources(BridgeRendererD3D11* s);
bool ResizeSwapChain(BridgeRendererD3D11* s, int width, int height);
bool ClearAndPresent(BridgeRendererD3D11* s, float red, float green, float blue,
                     float alpha, int syncInterval);

