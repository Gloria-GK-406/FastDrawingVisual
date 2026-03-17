#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
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

struct TriangleBatchDrawStats {
  double ensureVertexBufferMs = 0.0;
  double uploadVertexDataMs = 0.0;
  double issueDrawMs = 0.0;
  UINT uploadedBytes = 0;
  UINT vertexBufferCapacityBytes = 0;
  bool resizedVertexBuffer = false;
};

struct InstanceBatchDrawContext {
  ComPtr<ID3D11DeviceContext> context = nullptr;
  ComPtr<ID3D11InputLayout> inputLayout = nullptr;
  ComPtr<ID3D11VertexShader> vertexShader = nullptr;
  ComPtr<ID3D11PixelShader> pixelShader = nullptr;
  ComPtr<ID3D11BlendState> blendState = nullptr;
  ComPtr<ID3D11RasterizerState> rasterizerState = nullptr;
  ComPtr<ID3D11Buffer> geometryVertexBuffer = nullptr;
  UINT geometryVertexStrideBytes = 0;
  UINT geometryVertexCount = 0;
  ComPtr<ID3D11Buffer> instanceBuffer = nullptr;
  UINT instanceBufferCapacityBytes = 0;
  ComPtr<ID3D11Buffer> viewConstantsBuffer = nullptr;
  float viewportWidth = 0.0f;
  float viewportHeight = 0.0f;
};

struct RectInstanceData {
  const batch::RectInstance* instances = nullptr;
  int instanceCount = 0;
};

struct EllipseInstanceData {
  const batch::EllipseInstance* instances = nullptr;
  int instanceCount = 0;
};

struct ShapeInstanceData {
  const batch::ShapeInstance* instances = nullptr;
  int instanceCount = 0;
};

struct InstanceBatchDrawStats {
  double ensureInstanceBufferMs = 0.0;
  double uploadInstanceDataMs = 0.0;
  double issueDrawMs = 0.0;
  UINT uploadedBytes = 0;
  UINT instanceBufferCapacityBytes = 0;
  bool resizedInstanceBuffer = false;
};

} // namespace fdv::d3d11::draw
