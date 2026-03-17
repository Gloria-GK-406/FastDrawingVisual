#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d9.h>

#include "D3DBatchTypes.h"

namespace fdv::d3d9::draw {

struct TriangleBatchDrawContext {
  IDirect3DDevice9* device = nullptr;
  IDirect3DVertexDeclaration9* vertexDeclaration = nullptr;
  IDirect3DVertexShader9* vertexShader = nullptr;
  IDirect3DPixelShader9* pixelShader = nullptr;
  IDirect3DVertexBuffer9* vertexBuffer = nullptr;
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
  IDirect3DDevice9* device = nullptr;
  IDirect3DVertexDeclaration9* vertexDeclaration = nullptr;
  IDirect3DVertexShader9* vertexShader = nullptr;
  IDirect3DPixelShader9* pixelShader = nullptr;
  IDirect3DVertexBuffer9* geometryVertexBuffer = nullptr;
  IDirect3DIndexBuffer9* geometryIndexBuffer = nullptr;
  UINT geometryVertexStrideBytes = 0;
  UINT geometryVertexCount = 0;
  UINT geometryPrimitiveCount = 0;
  IDirect3DVertexBuffer9* instanceBuffer = nullptr;
  UINT instanceBufferCapacityBytes = 0;
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

struct TextBatchDrawContext {};

struct DrawTextData {
  const batch::TextBatchItem* items = nullptr;
  int count = 0;
};

} // namespace fdv::d3d9::draw
