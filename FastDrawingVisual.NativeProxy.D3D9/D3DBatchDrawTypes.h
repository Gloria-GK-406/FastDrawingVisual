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
};

struct TriangleVertexData {
  const batch::TriangleVertex* vertices = nullptr;
  int vertexCount = 0;
};

struct InstanceBatchDrawContext {
  IDirect3DDevice9* device = nullptr;
  IDirect3DVertexDeclaration9* vertexDeclaration = nullptr;
  IDirect3DVertexShader9* vertexShader = nullptr;
  IDirect3DPixelShader9* pixelShader = nullptr;
  int viewportWidth = 0;
  int viewportHeight = 0;
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

struct TextBatchDrawContext {};

struct DrawTextData {
  const batch::TextBatchItem* items = nullptr;
  int count = 0;
};

} // namespace fdv::d3d9::draw
