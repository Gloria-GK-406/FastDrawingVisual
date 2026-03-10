#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <cstdint>

#include "BridgeCommandProtocol.g.h"

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
  ID2D1Factory1* d2dFactory = nullptr;
  ID2D1Device* d2dDevice = nullptr;
  ID2D1DeviceContext* d2dContext = nullptr;
  ID2D1Bitmap1* d2dTargetBitmap = nullptr;
  ID2D1SolidColorBrush* d2dSolidBrush = nullptr;
  IDWriteFactory* dwriteFactory = nullptr;

  int width = 0;
  int height = 0;
  HRESULT lastErrorHr = S_OK;
  int drawDurationMetricId = 0;
  int fpsMetricId = 0;
  uint64_t lastPresentQpc = 0;
  uint64_t submittedFrameCount = 0;

  bool csInitialized = false;
  CRITICAL_SECTION cs;
};

template <typename T> inline void SafeRelease(T** ptr) {
  if (ptr == nullptr || *ptr == nullptr) {
    return;
  }

  (*ptr)->Release();
  *ptr = nullptr;
}

inline void SetLastError(BridgeRendererD3D11* s, HRESULT hr) {
  if (s != nullptr) {
    s->lastErrorHr = hr;
  }
}

bool CreateDeviceAndSwapChain(BridgeRendererD3D11* s);
void ReleaseRendererResources(BridgeRendererD3D11* s);
bool ResizeSwapChain(BridgeRendererD3D11* s, int width, int height);
bool SubmitCommandsAndPresent(BridgeRendererD3D11* s, const void* commands,
                              int commandBytes, const void* blobs,
                              int blobBytes);
bool CreateDrawPipeline(BridgeRendererD3D11* s);
bool EnsureDynamicVertexBuffer(BridgeRendererD3D11* s, UINT requiredBytes);
bool BeginD2DDraw(BridgeRendererD3D11* s, bool& d2dDrawActive);
bool EndD2DDraw(BridgeRendererD3D11* s, bool& d2dDrawActive);
bool ExecuteDrawTextCommand(BridgeRendererD3D11* s,
                            const fdv::protocol::DrawTextPayload& payload,
                            const fdv::protocol::CommandReader& reader,
                            bool& d2dDrawActive);
