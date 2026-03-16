#include "D3DBatchDraw.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fdv::d3d9::draw {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kEllipseSegmentCount = 48;

struct ColorF {
  float r;
  float g;
  float b;
  float a;
};

float ToNdcX(float width, float x) {
  return (x / width) * 2.0f - 1.0f;
}

float ToNdcY(float height, float y) {
  return 1.0f - (y / height) * 2.0f;
}

batch::TriangleVertex MakeVertex(float width, float height, float x, float y,
                                 float r, float g, float b, float a) {
  return {ToNdcX(width, x), ToNdcY(height, y), 0.0f, r, g, b, a};
}

ColorF ShapeFillColor(const batch::ShapeInstance& instance) {
  return {instance.fillR, instance.fillG, instance.fillB, instance.fillA};
}

ColorF ShapeStrokeColor(const batch::ShapeInstance& instance) {
  return {instance.strokeR, instance.strokeG, instance.strokeB,
          instance.strokeA};
}

batch::ShapeInstanceType GetShapeType(const batch::ShapeInstance& instance) {
  const auto rawType =
      static_cast<std::uint32_t>(instance.type < 0.0f ? 0.0f
                                                       : instance.type + 0.5f);
  return static_cast<batch::ShapeInstanceType>(rawType);
}

void AppendFilledRect(float width, float height,
                      std::vector<batch::TriangleVertex>& out,
                      const batch::RectInstance& instance) {
  if (instance.width <= 0.0f || instance.height <= 0.0f) {
    return;
  }

  const float x0 = instance.x;
  const float y0 = instance.y;
  const float x1 = instance.x + instance.width;
  const float y1 = instance.y + instance.height;

  out.push_back(MakeVertex(width, height, x0, y0, instance.r, instance.g,
                           instance.b, instance.a));
  out.push_back(MakeVertex(width, height, x1, y0, instance.r, instance.g,
                           instance.b, instance.a));
  out.push_back(MakeVertex(width, height, x0, y1, instance.r, instance.g,
                           instance.b, instance.a));
  out.push_back(MakeVertex(width, height, x1, y0, instance.r, instance.g,
                           instance.b, instance.a));
  out.push_back(MakeVertex(width, height, x1, y1, instance.r, instance.g,
                           instance.b, instance.a));
  out.push_back(MakeVertex(width, height, x0, y1, instance.r, instance.g,
                           instance.b, instance.a));
}

void AppendStrokeRect(float width, float height,
                      std::vector<batch::TriangleVertex>& out,
                      const batch::RectInstance& instance) {
  const float t = (std::max)(1.0f, instance.thickness);
  if (t * 2.0f >= instance.width || t * 2.0f >= instance.height) {
    AppendFilledRect(width, height, out, instance);
    return;
  }

  batch::RectInstance edge = instance;
  edge.height = t;
  AppendFilledRect(width, height, out, edge);

  edge.y = instance.y + instance.height - t;
  AppendFilledRect(width, height, out, edge);

  edge = instance;
  edge.width = t;
  edge.y = instance.y + t;
  edge.height = instance.height - 2.0f * t;
  AppendFilledRect(width, height, out, edge);

  edge.x = instance.x + instance.width - t;
  AppendFilledRect(width, height, out, edge);
}

void AppendFilledEllipse(float width, float height,
                         std::vector<batch::TriangleVertex>& out,
                         const batch::EllipseInstance& instance) {
  if (instance.radiusX <= 0.0f || instance.radiusY <= 0.0f) {
    return;
  }

  const auto center = MakeVertex(width, height, instance.centerX,
                                 instance.centerY, instance.r, instance.g,
                                 instance.b, instance.a);
  for (int i = 0; i < kEllipseSegmentCount; ++i) {
    const float a0 = (2.0f * kPi * static_cast<float>(i)) /
                     static_cast<float>(kEllipseSegmentCount);
    const float a1 = (2.0f * kPi * static_cast<float>(i + 1)) /
                     static_cast<float>(kEllipseSegmentCount);
    const float x0 = instance.centerX + std::cos(a0) * instance.radiusX;
    const float y0 = instance.centerY + std::sin(a0) * instance.radiusY;
    const float x1 = instance.centerX + std::cos(a1) * instance.radiusX;
    const float y1 = instance.centerY + std::sin(a1) * instance.radiusY;

    out.push_back(center);
    out.push_back(MakeVertex(width, height, x0, y0, instance.r, instance.g,
                             instance.b, instance.a));
    out.push_back(MakeVertex(width, height, x1, y1, instance.r, instance.g,
                             instance.b, instance.a));
  }
}

void AppendStrokeEllipse(float width, float height,
                         std::vector<batch::TriangleVertex>& out,
                         const batch::EllipseInstance& instance) {
  const float t = (std::max)(1.0f, instance.thickness);
  const float outerRx = instance.radiusX + t * 0.5f;
  const float outerRy = instance.radiusY + t * 0.5f;
  const float innerRx = instance.radiusX - t * 0.5f;
  const float innerRy = instance.radiusY - t * 0.5f;

  batch::EllipseInstance outer = instance;
  outer.radiusX = outerRx;
  outer.radiusY = outerRy;
  if (innerRx <= 0.0f || innerRy <= 0.0f) {
    AppendFilledEllipse(width, height, out, outer);
    return;
  }

  for (int i = 0; i < kEllipseSegmentCount; ++i) {
    const float a0 = (2.0f * kPi * static_cast<float>(i)) /
                     static_cast<float>(kEllipseSegmentCount);
    const float a1 = (2.0f * kPi * static_cast<float>(i + 1)) /
                     static_cast<float>(kEllipseSegmentCount);
    const float ox0 = instance.centerX + std::cos(a0) * outerRx;
    const float oy0 = instance.centerY + std::sin(a0) * outerRy;
    const float ox1 = instance.centerX + std::cos(a1) * outerRx;
    const float oy1 = instance.centerY + std::sin(a1) * outerRy;
    const float ix0 = instance.centerX + std::cos(a0) * innerRx;
    const float iy0 = instance.centerY + std::sin(a0) * innerRy;
    const float ix1 = instance.centerX + std::cos(a1) * innerRx;
    const float iy1 = instance.centerY + std::sin(a1) * innerRy;

    out.push_back(MakeVertex(width, height, ox0, oy0, instance.r, instance.g,
                             instance.b, instance.a));
    out.push_back(MakeVertex(width, height, ox1, oy1, instance.r, instance.g,
                             instance.b, instance.a));
    out.push_back(MakeVertex(width, height, ix0, iy0, instance.r, instance.g,
                             instance.b, instance.a));
    out.push_back(MakeVertex(width, height, ox1, oy1, instance.r, instance.g,
                             instance.b, instance.a));
    out.push_back(MakeVertex(width, height, ix1, iy1, instance.r, instance.g,
                             instance.b, instance.a));
    out.push_back(MakeVertex(width, height, ix0, iy0, instance.r, instance.g,
                             instance.b, instance.a));
  }
}

void AppendStrokeEllipseFromOuterBounds(float width, float height,
                                        std::vector<batch::TriangleVertex>& out,
                                        float centerX, float centerY,
                                        float outerRadiusX,
                                        float outerRadiusY, float thickness,
                                        const ColorF& color) {
  if (outerRadiusX <= 0.0f || outerRadiusY <= 0.0f) {
    return;
  }

  const float t = (std::max)(1.0f, thickness);
  const float innerRadiusX = outerRadiusX - t;
  const float innerRadiusY = outerRadiusY - t;

  batch::EllipseInstance outer{centerX, centerY, outerRadiusX, outerRadiusY,
                               0.0f,    color.r, color.g,       color.b,
                               color.a};
  if (innerRadiusX <= 0.0f || innerRadiusY <= 0.0f) {
    AppendFilledEllipse(width, height, out, outer);
    return;
  }

  for (int i = 0; i < kEllipseSegmentCount; ++i) {
    const float a0 = (2.0f * kPi * static_cast<float>(i)) /
                     static_cast<float>(kEllipseSegmentCount);
    const float a1 = (2.0f * kPi * static_cast<float>(i + 1)) /
                     static_cast<float>(kEllipseSegmentCount);
    const float ox0 = centerX + std::cos(a0) * outerRadiusX;
    const float oy0 = centerY + std::sin(a0) * outerRadiusY;
    const float ox1 = centerX + std::cos(a1) * outerRadiusX;
    const float oy1 = centerY + std::sin(a1) * outerRadiusY;
    const float ix0 = centerX + std::cos(a0) * innerRadiusX;
    const float iy0 = centerY + std::sin(a0) * innerRadiusY;
    const float ix1 = centerX + std::cos(a1) * innerRadiusX;
    const float iy1 = centerY + std::sin(a1) * innerRadiusY;

    out.push_back(MakeVertex(width, height, ox0, oy0, color.r, color.g,
                             color.b, color.a));
    out.push_back(MakeVertex(width, height, ox1, oy1, color.r, color.g,
                             color.b, color.a));
    out.push_back(MakeVertex(width, height, ix0, iy0, color.r, color.g,
                             color.b, color.a));
    out.push_back(MakeVertex(width, height, ox1, oy1, color.r, color.g,
                             color.b, color.a));
    out.push_back(MakeVertex(width, height, ix1, iy1, color.r, color.g,
                             color.b, color.a));
    out.push_back(MakeVertex(width, height, ix0, iy0, color.r, color.g,
                             color.b, color.a));
  }
}

void AppendLine(float width, float height,
                std::vector<batch::TriangleVertex>& out, float x0, float y0,
                float x1, float y1, float thickness, const ColorF& color) {
  const float t = (std::max)(1.0f, thickness);
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const float len = std::sqrt(dx * dx + dy * dy);

  if (len < 0.0001f) {
    const batch::RectInstance point{x0 - t * 0.5f, y0 - t * 0.5f, t, t, 0.0f,
                                    color.r,       color.g,       color.b,
                                    color.a};
    AppendFilledRect(width, height, out, point);
    return;
  }

  const float half = t * 0.5f;
  const float nx = -dy / len * half;
  const float ny = dx / len * half;

  const auto v0 = MakeVertex(width, height, x0 + nx, y0 + ny, color.r, color.g,
                             color.b, color.a);
  const auto v1 = MakeVertex(width, height, x1 + nx, y1 + ny, color.r, color.g,
                             color.b, color.a);
  const auto v2 = MakeVertex(width, height, x1 - nx, y1 - ny, color.r, color.g,
                             color.b, color.a);
  const auto v3 = MakeVertex(width, height, x0 - nx, y0 - ny, color.r, color.g,
                             color.b, color.a);

  out.push_back(v0);
  out.push_back(v1);
  out.push_back(v2);
  out.push_back(v0);
  out.push_back(v2);
  out.push_back(v3);
}

void AppendShape(float width, float height,
                 std::vector<batch::TriangleVertex>& out,
                 const batch::ShapeInstance& instance) {
  const auto type = GetShapeType(instance);
  switch (type) {
  case batch::ShapeInstanceType::FillRect: {
    const auto color = ShapeFillColor(instance);
    const batch::RectInstance rect{instance.x, instance.y, instance.width,
                                   instance.height, 0.0f, color.r, color.g,
                                   color.b,         color.a};
    AppendFilledRect(width, height, out, rect);
    break;
  }
  case batch::ShapeInstanceType::StrokeRect: {
    const auto color = ShapeStrokeColor(instance);
    const batch::RectInstance rect{
        instance.x, instance.y, instance.width,  instance.height,
        instance.strokeWidth, color.r, color.g, color.b, color.a};
    AppendStrokeRect(width, height, out, rect);
    break;
  }
  case batch::ShapeInstanceType::FillEllipse: {
    const auto color = ShapeFillColor(instance);
    const batch::EllipseInstance ellipse{
        instance.x + instance.width * 0.5f, instance.y + instance.height * 0.5f,
        instance.width * 0.5f,              instance.height * 0.5f,
        0.0f,                               color.r,
        color.g,                            color.b,
        color.a};
    AppendFilledEllipse(width, height, out, ellipse);
    break;
  }
  case batch::ShapeInstanceType::StrokeEllipse: {
    AppendStrokeEllipseFromOuterBounds(
        width, height, out, instance.x + instance.width * 0.5f,
        instance.y + instance.height * 0.5f, instance.width * 0.5f,
        instance.height * 0.5f, instance.strokeWidth,
        ShapeStrokeColor(instance));
    break;
  }
  case batch::ShapeInstanceType::Line: {
    const float centerX = instance.x + instance.width * 0.5f;
    const float centerY = instance.y + instance.height * 0.5f;
    AppendLine(width, height, out, centerX + instance.data0x,
               centerY + instance.data0y, centerX + instance.data0z,
               centerY + instance.data0w, instance.strokeWidth,
               ShapeStrokeColor(instance));
    break;
  }
  }
}

HRESULT DrawTriangleVector(const TriangleBatchDrawContext& context,
                           const std::vector<batch::TriangleVertex>& vertices) {
  const TriangleVertexData vertexData{vertices.data(),
                                      static_cast<int>(vertices.size())};
  return DrawTriangleBatch(context, vertexData);
}

} // namespace

void SetupRenderState(IDirect3DDevice9* device) {
  if (device == nullptr) {
    return;
  }

  device->SetRenderState(D3DRS_ZENABLE, FALSE);
  device->SetRenderState(D3DRS_LIGHTING, FALSE);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
  device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
  device->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, FALSE);
  device->SetTexture(0, nullptr);
  device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DISABLE);
  device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

HRESULT DrawTriangleBatch(const TriangleBatchDrawContext& context,
                          const TriangleVertexData& vertexData) {
  if (vertexData.vertices == nullptr || vertexData.vertexCount <= 0) {
    return S_OK;
  }

  if (context.device == nullptr || context.vertexDeclaration == nullptr ||
      context.vertexShader == nullptr || context.pixelShader == nullptr) {
    return E_POINTER;
  }

  if ((vertexData.vertexCount % 3) != 0) {
    return E_INVALIDARG;
  }

  context.device->SetVertexDeclaration(context.vertexDeclaration);
  context.device->SetVertexShader(context.vertexShader);
  context.device->SetPixelShader(context.pixelShader);

  const UINT primitiveCount = static_cast<UINT>(vertexData.vertexCount / 3);
  const HRESULT hr = context.device->DrawPrimitiveUP(
      D3DPT_TRIANGLELIST, primitiveCount, vertexData.vertices,
      sizeof(batch::TriangleVertex));
  return FAILED(hr) ? hr : S_OK;
}

HRESULT DrawShapeBatch(const InstanceBatchDrawContext& context,
                       const ShapeInstanceData& instanceData) {
  TriangleBatchDrawContext triangleContext{};
  triangleContext.device = context.device;
  triangleContext.vertexDeclaration = context.vertexDeclaration;
  triangleContext.vertexShader = context.vertexShader;
  triangleContext.pixelShader = context.pixelShader;

  std::vector<batch::TriangleVertex> vertices;
  vertices.reserve(static_cast<std::size_t>(instanceData.instanceCount) * 144u);
  for (int i = 0; i < instanceData.instanceCount; ++i) {
    AppendShape(static_cast<float>(context.viewportWidth),
                static_cast<float>(context.viewportHeight), vertices,
                instanceData.instances[i]);
  }

  return DrawTriangleVector(triangleContext, vertices);
}

HRESULT DrawRectBatch(const InstanceBatchDrawContext& context,
                      const RectInstanceData& instanceData) {
  TriangleBatchDrawContext triangleContext{};
  triangleContext.device = context.device;
  triangleContext.vertexDeclaration = context.vertexDeclaration;
  triangleContext.vertexShader = context.vertexShader;
  triangleContext.pixelShader = context.pixelShader;

  std::vector<batch::TriangleVertex> vertices;
  vertices.reserve(static_cast<std::size_t>(instanceData.instanceCount) * 24u);
  for (int i = 0; i < instanceData.instanceCount; ++i) {
    const auto& instance = instanceData.instances[i];
    if (instance.thickness > 0.0f) {
      AppendStrokeRect(static_cast<float>(context.viewportWidth),
                       static_cast<float>(context.viewportHeight), vertices,
                       instance);
    } else {
      AppendFilledRect(static_cast<float>(context.viewportWidth),
                       static_cast<float>(context.viewportHeight), vertices,
                       instance);
    }
  }

  return DrawTriangleVector(triangleContext, vertices);
}

HRESULT DrawEllipseBatch(const InstanceBatchDrawContext& context,
                         const EllipseInstanceData& instanceData) {
  TriangleBatchDrawContext triangleContext{};
  triangleContext.device = context.device;
  triangleContext.vertexDeclaration = context.vertexDeclaration;
  triangleContext.vertexShader = context.vertexShader;
  triangleContext.pixelShader = context.pixelShader;

  std::vector<batch::TriangleVertex> vertices;
  vertices.reserve(static_cast<std::size_t>(instanceData.instanceCount) * 144u);
  for (int i = 0; i < instanceData.instanceCount; ++i) {
    const auto& instance = instanceData.instances[i];
    if (instance.thickness > 0.0f) {
      AppendStrokeEllipse(static_cast<float>(context.viewportWidth),
                          static_cast<float>(context.viewportHeight), vertices,
                          instance);
    } else {
      AppendFilledEllipse(static_cast<float>(context.viewportWidth),
                          static_cast<float>(context.viewportHeight), vertices,
                          instance);
    }
  }

  return DrawTriangleVector(triangleContext, vertices);
}

HRESULT DrawTextBatch(const TextBatchDrawContext& context,
                      const DrawTextData& textData) {
  static_cast<void>(context);
  static_cast<void>(textData);
  return S_OK;
}

} // namespace fdv::d3d9::draw
