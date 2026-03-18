#include "D3DBatchDraw.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace fdv::d3d9::draw {
namespace {

constexpr float kEllipseEdgePadding = 1.0f;

struct ViewConstants {
  float viewportWidth = 0.0f;
  float viewportHeight = 0.0f;
  float padding0 = 0.0f;
  float padding1 = 0.0f;
};

double DurationMs(const std::chrono::steady_clock::time_point& start,
                  const std::chrono::steady_clock::time_point& end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

void ReleaseVertexBuffer(IDirect3DVertexBuffer9*& buffer) {
  if (buffer != nullptr) {
    buffer->Release();
    buffer = nullptr;
  }
}

HRESULT EnsureDynamicVertexBuffer(TriangleBatchDrawContext& context,
                                  UINT requiredBytes,
                                  TriangleBatchDrawStats* stats) {
  if (context.device == nullptr) {
    return E_POINTER;
  }

  const UINT minBytes = (std::max)(requiredBytes, 1024u);
  if (context.vertexBuffer != nullptr &&
      context.vertexBufferCapacityBytes >= minBytes) {
    if (stats != nullptr) {
      stats->vertexBufferCapacityBytes = context.vertexBufferCapacityBytes;
    }
    return S_OK;
  }

  ReleaseVertexBuffer(context.vertexBuffer);
  const HRESULT hr = context.device->CreateVertexBuffer(
      minBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
      &context.vertexBuffer, nullptr);
  if (FAILED(hr) || context.vertexBuffer == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  context.vertexBufferCapacityBytes = minBytes;
  if (stats != nullptr) {
    stats->resizedVertexBuffer = true;
    stats->vertexBufferCapacityBytes = minBytes;
  }
  return S_OK;
}

HRESULT EnsureDynamicInstanceBuffer(InstanceBatchDrawContext& context,
                                    UINT requiredBytes,
                                    InstanceBatchDrawStats* stats) {
  if (context.device == nullptr) {
    return E_POINTER;
  }

  const UINT minBytes = (std::max)(requiredBytes, 1024u);
  if (context.instanceBuffer != nullptr &&
      context.instanceBufferCapacityBytes >= minBytes) {
    if (stats != nullptr) {
      stats->instanceBufferCapacityBytes = context.instanceBufferCapacityBytes;
    }
    return S_OK;
  }

  ReleaseVertexBuffer(context.instanceBuffer);
  const HRESULT hr = context.device->CreateVertexBuffer(
      minBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
      &context.instanceBuffer, nullptr);
  if (FAILED(hr) || context.instanceBuffer == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  context.instanceBufferCapacityBytes = minBytes;
  if (stats != nullptr) {
    stats->resizedInstanceBuffer = true;
    stats->instanceBufferCapacityBytes = minBytes;
  }
  return S_OK;
}

HRESULT UpdateViewConstants(InstanceBatchDrawContext& context) {
  if (context.device == nullptr || context.viewportWidth <= 0.0f ||
      context.viewportHeight <= 0.0f) {
    return E_POINTER;
  }

  const ViewConstants constants{context.viewportWidth, context.viewportHeight,
                                0.0f, 0.0f};
  const HRESULT hr =
      context.device->SetVertexShaderConstantF(0, &constants.viewportWidth, 1);
  return FAILED(hr) ? hr : S_OK;
}

template <typename TInstance>
HRESULT DrawInstanceBatch(InstanceBatchDrawContext& context,
                          const TInstance* instances, int instanceCount,
                          InstanceBatchDrawStats* stats) {
  if (instances == nullptr || instanceCount <= 0) {
    return S_OK;
  }

  if (context.device == nullptr || context.vertexDeclaration == nullptr ||
      context.vertexShader == nullptr || context.pixelShader == nullptr ||
      context.geometryVertexBuffer == nullptr ||
      context.geometryIndexBuffer == nullptr ||
      context.geometryVertexStrideBytes == 0 ||
      context.geometryVertexCount < 4 || context.geometryPrimitiveCount == 0) {
    return E_POINTER;
  }

  const UINT byteSize = static_cast<UINT>(instanceCount * sizeof(TInstance));
  const auto ensureStart = std::chrono::steady_clock::now();
  const HRESULT ensureHr =
      EnsureDynamicInstanceBuffer(context, byteSize, stats);
  const auto ensureEnd = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->ensureInstanceBufferMs += DurationMs(ensureStart, ensureEnd);
    stats->instanceBufferCapacityBytes = context.instanceBufferCapacityBytes;
  }
  if (FAILED(ensureHr)) {
    return ensureHr;
  }

  const HRESULT constantsHr = UpdateViewConstants(context);
  if (FAILED(constantsHr)) {
    return constantsHr;
  }

  void* mapped = nullptr;
  const auto uploadStart = std::chrono::steady_clock::now();
  const HRESULT lockHr =
      context.instanceBuffer->Lock(0, byteSize, &mapped, D3DLOCK_DISCARD);
  if (FAILED(lockHr) || mapped == nullptr) {
    return FAILED(lockHr) ? lockHr : E_FAIL;
  }

  std::memcpy(mapped, instances, byteSize);
  context.instanceBuffer->Unlock();
  const auto uploadEnd = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->uploadInstanceDataMs += DurationMs(uploadStart, uploadEnd);
    stats->uploadedBytes += byteSize;
  }

  const HRESULT declHr =
      context.device->SetVertexDeclaration(context.vertexDeclaration);
  if (FAILED(declHr)) {
    return declHr;
  }

  const HRESULT vsHr = context.device->SetVertexShader(context.vertexShader);
  if (FAILED(vsHr)) {
    return vsHr;
  }

  const HRESULT psHr = context.device->SetPixelShader(context.pixelShader);
  if (FAILED(psHr)) {
    return psHr;
  }

  HRESULT hr = context.device->SetStreamSource(0, context.geometryVertexBuffer,
                                               0,
                                               context.geometryVertexStrideBytes);
  if (FAILED(hr)) {
    return hr;
  }

  hr = context.device->SetStreamSource(1, context.instanceBuffer, 0,
                                       static_cast<UINT>(sizeof(TInstance)));
  if (FAILED(hr)) {
    context.device->SetStreamSource(0, nullptr, 0, 0);
    return hr;
  }

  hr = context.device->SetIndices(context.geometryIndexBuffer);
  if (FAILED(hr)) {
    context.device->SetStreamSource(1, nullptr, 0, 0);
    context.device->SetStreamSource(0, nullptr, 0, 0);
    return hr;
  }

  hr = context.device->SetStreamSourceFreq(
      0, D3DSTREAMSOURCE_INDEXEDDATA | static_cast<UINT>(instanceCount));
  if (FAILED(hr)) {
    context.device->SetIndices(nullptr);
    context.device->SetStreamSource(1, nullptr, 0, 0);
    context.device->SetStreamSource(0, nullptr, 0, 0);
    return hr;
  }

  hr = context.device->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
  if (FAILED(hr)) {
    context.device->SetStreamSourceFreq(0, 1u);
    context.device->SetIndices(nullptr);
    context.device->SetStreamSource(1, nullptr, 0, 0);
    context.device->SetStreamSource(0, nullptr, 0, 0);
    return hr;
  }

  const auto drawStart = std::chrono::steady_clock::now();
  hr = context.device->DrawIndexedPrimitive(
      D3DPT_TRIANGLELIST, 0, 0, context.geometryVertexCount, 0,
      context.geometryPrimitiveCount);
  const auto drawEnd = std::chrono::steady_clock::now();
  if (stats != nullptr && SUCCEEDED(hr)) {
    stats->issueDrawMs += DurationMs(drawStart, drawEnd);
  }

  context.device->SetIndices(nullptr);
  context.device->SetStreamSourceFreq(1, 1u);
  context.device->SetStreamSourceFreq(0, 1u);
  context.device->SetStreamSource(1, nullptr, 0, 0);
  context.device->SetStreamSource(0, nullptr, 0, 0);
  return FAILED(hr) ? hr : S_OK;
}

batch::ShapeInstance MakeShapeRectInstance(const batch::RectInstance& instance) {
  const bool stroke = instance.thickness > 0.0f;
  const float type = stroke
                         ? static_cast<float>(batch::ShapeInstanceType::StrokeRect)
                         : static_cast<float>(batch::ShapeInstanceType::FillRect);
  return {
      instance.x,
      instance.y,
      instance.width,
      instance.height,
      0.0f,
      0.0f,
      0.0f,
      0.0f,
      stroke ? 0.0f : instance.r,
      stroke ? 0.0f : instance.g,
      stroke ? 0.0f : instance.b,
      stroke ? 0.0f : instance.a,
      stroke ? instance.r : 0.0f,
      stroke ? instance.g : 0.0f,
      stroke ? instance.b : 0.0f,
      stroke ? instance.a : 0.0f,
      stroke ? (std::max)(1.0f, instance.thickness) : 0.0f,
      0.0f,
      type,
      0.0f,
  };
}

batch::ShapeInstance MakeShapeEllipseInstance(
    const batch::EllipseInstance& instance) {
  const bool stroke = instance.thickness > 0.0f;
  const float strokeWidth = stroke ? (std::max)(1.0f, instance.thickness) : 0.0f;
  const float outerRadiusX = instance.radiusX + (stroke ? strokeWidth * 0.5f : 0.0f);
  const float outerRadiusY = instance.radiusY + (stroke ? strokeWidth * 0.5f : 0.0f);
  const float x = instance.centerX - outerRadiusX - kEllipseEdgePadding;
  const float y = instance.centerY - outerRadiusY - kEllipseEdgePadding;
  const float width = (outerRadiusX + kEllipseEdgePadding) * 2.0f;
  const float height = (outerRadiusY + kEllipseEdgePadding) * 2.0f;

  const float type =
      stroke ? static_cast<float>(batch::ShapeInstanceType::StrokeEllipse)
             : static_cast<float>(batch::ShapeInstanceType::FillEllipse);
  return {
      x,
      y,
      width,
      height,
      outerRadiusX,
      outerRadiusY,
      0.0f,
      0.0f,
      stroke ? 0.0f : instance.r,
      stroke ? 0.0f : instance.g,
      stroke ? 0.0f : instance.b,
      stroke ? 0.0f : instance.a,
      stroke ? instance.r : 0.0f,
      stroke ? instance.g : 0.0f,
      stroke ? instance.b : 0.0f,
      stroke ? instance.a : 0.0f,
      strokeWidth,
      0.0f,
      type,
      0.0f,
  };
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
  device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
  device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
  device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
  device->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
  device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
  device->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, FALSE);
  device->SetTexture(0, nullptr);
  device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DISABLE);
  device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

HRESULT DrawTriangleBatch(TriangleBatchDrawContext& context,
                          const TriangleVertexData& vertexData,
                          TriangleBatchDrawStats* stats) {
  if (vertexData.vertices == nullptr || vertexData.vertexCount <= 0) {
    return S_OK;
  }

  if ((vertexData.vertexCount % 3) != 0) {
    return E_INVALIDARG;
  }

  if (context.device == nullptr || context.vertexDeclaration == nullptr ||
      context.vertexShader == nullptr || context.pixelShader == nullptr) {
    return E_POINTER;
  }

  const UINT byteSize = static_cast<UINT>(vertexData.vertexCount *
                                          sizeof(batch::TriangleVertex));
  const auto ensureStart = std::chrono::steady_clock::now();
  const HRESULT ensureHr = EnsureDynamicVertexBuffer(context, byteSize, stats);
  const auto ensureEnd = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->ensureVertexBufferMs += DurationMs(ensureStart, ensureEnd);
    stats->vertexBufferCapacityBytes = context.vertexBufferCapacityBytes;
  }
  if (FAILED(ensureHr)) {
    return ensureHr;
  }

  void* mapped = nullptr;
  const auto uploadStart = std::chrono::steady_clock::now();
  const HRESULT lockHr =
      context.vertexBuffer->Lock(0, byteSize, &mapped, D3DLOCK_DISCARD);
  if (FAILED(lockHr) || mapped == nullptr) {
    return FAILED(lockHr) ? lockHr : E_FAIL;
  }

  std::memcpy(mapped, vertexData.vertices, byteSize);
  context.vertexBuffer->Unlock();
  const auto uploadEnd = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->uploadVertexDataMs += DurationMs(uploadStart, uploadEnd);
    stats->uploadedBytes += byteSize;
  }

  HRESULT hr = context.device->SetVertexDeclaration(context.vertexDeclaration);
  if (FAILED(hr)) {
    return hr;
  }

  hr = context.device->SetVertexShader(context.vertexShader);
  if (FAILED(hr)) {
    return hr;
  }

  hr = context.device->SetPixelShader(context.pixelShader);
  if (FAILED(hr)) {
    return hr;
  }

  hr = context.device->SetStreamSource(0, context.vertexBuffer, 0,
                                       sizeof(batch::TriangleVertex));
  if (FAILED(hr)) {
    return hr;
  }

  const auto drawStart = std::chrono::steady_clock::now();
  hr = context.device->DrawPrimitive(
      D3DPT_TRIANGLELIST, 0, static_cast<UINT>(vertexData.vertexCount / 3));
  const auto drawEnd = std::chrono::steady_clock::now();
  context.device->SetStreamSource(0, nullptr, 0, 0);

  if (stats != nullptr && SUCCEEDED(hr)) {
    stats->issueDrawMs += DurationMs(drawStart, drawEnd);
  }
  return FAILED(hr) ? hr : S_OK;
}

HRESULT DrawShapeInstanceBatch(InstanceBatchDrawContext& context,
                               const ShapeInstanceData& instanceData,
                               InstanceBatchDrawStats* stats) {
  return DrawInstanceBatch(context, instanceData.instances,
                           instanceData.instanceCount, stats);
}

HRESULT DrawRectInstanceBatch(InstanceBatchDrawContext& context,
                              const RectInstanceData& instanceData,
                              InstanceBatchDrawStats* stats) {
  if (instanceData.instances == nullptr || instanceData.instanceCount <= 0) {
    return S_OK;
  }

  std::vector<batch::ShapeInstance> shapeInstances;
  shapeInstances.reserve(static_cast<std::size_t>(instanceData.instanceCount));
  for (int i = 0; i < instanceData.instanceCount; ++i) {
    shapeInstances.push_back(MakeShapeRectInstance(instanceData.instances[i]));
  }

  const ShapeInstanceData shapeData{shapeInstances.data(),
                                    static_cast<int>(shapeInstances.size())};
  return DrawShapeInstanceBatch(context, shapeData, stats);
}

HRESULT DrawEllipseInstanceBatch(InstanceBatchDrawContext& context,
                                 const EllipseInstanceData& instanceData,
                                 InstanceBatchDrawStats* stats) {
  if (instanceData.instances == nullptr || instanceData.instanceCount <= 0) {
    return S_OK;
  }

  std::vector<batch::ShapeInstance> shapeInstances;
  shapeInstances.reserve(static_cast<std::size_t>(instanceData.instanceCount));
  for (int i = 0; i < instanceData.instanceCount; ++i) {
    shapeInstances.push_back(MakeShapeEllipseInstance(instanceData.instances[i]));
  }

  const ShapeInstanceData shapeData{shapeInstances.data(),
                                    static_cast<int>(shapeInstances.size())};
  return DrawShapeInstanceBatch(context, shapeData, stats);
}

HRESULT DrawTextBatch(const TextBatchDrawContext& context,
                      const DrawTextData& textData) {
  static_cast<void>(context);
  static_cast<void>(textData);
  return S_OK;
}

} // namespace fdv::d3d9::draw
