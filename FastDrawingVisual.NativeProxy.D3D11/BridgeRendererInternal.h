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
  ID3D11VertexShader* vertexShader = nullptr;
  ID3D11PixelShader* pixelShader = nullptr;
  ID3D11InputLayout* inputLayout = nullptr;
  ID3D11BlendState* blendState = nullptr;
  ID3D11RasterizerState* rasterizerState = nullptr;
  ID3D11Buffer* dynamicVertexBuffer = nullptr;
  UINT dynamicVertexCapacityBytes = 0;

  int width = 0;
  int height = 0;
  HRESULT lastErrorHr = S_OK;

  bool csInitialized = false;
  CRITICAL_SECTION cs;
};

bool CreateDeviceAndSwapChain(BridgeRendererD3D11* s);
void ReleaseRendererResources(BridgeRendererD3D11* s);
bool ResizeSwapChain(BridgeRendererD3D11* s, int width, int height);
bool SubmitCommandsAndPresent(BridgeRendererD3D11* s, const void* commands,
                              int commandBytes);
