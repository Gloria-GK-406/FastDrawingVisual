#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <wrl/client.h>

#include "D3DBatchTypes.h"

using Microsoft::WRL::ComPtr;

namespace fdv::d3d11::draw {

struct TriangleBatchDrawContext {
  ComPtr<ID3D11DeviceContext> context = nullptr;
  ComPtr<ID3D11InputLayout> inputLayout = nullptr;
  ComPtr<ID3D11VertexShader> vertexShader = nullptr;
  ComPtr<ID3D11PixelShader> pixelShader = nullptr;
  ComPtr<ID3D11BlendState> blendState = nullptr;
  ComPtr<ID3D11RasterizerState> rasterizerState = nullptr;
  ComPtr<ID3D11Buffer> vertexBuffer = nullptr;
  UINT vertexBufferCapacityBytes = 0;
};

struct TriangleVertexData {
  const batch::TriangleVertex* vertices = nullptr;
  int vertexCount = 0;
};

struct TextBatchDrawContext {
  ComPtr<ID3D11DeviceContext> d3dContext = nullptr;
  ComPtr<ID2D1DeviceContext> d2dContext = nullptr;
  ComPtr<ID2D1SolidColorBrush> solidBrush = nullptr;
};

struct DrawTextData {
  const batch::TextBatchItem* items = nullptr;
  int count = 0;
};

} // namespace fdv::d3d11::draw
