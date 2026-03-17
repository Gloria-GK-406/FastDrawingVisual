#include "D3DBatchDraw.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace fdv::d3d11::draw {
namespace {

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

HRESULT EnsureDynamicVertexBuffer(TriangleBatchDrawContext& context,
                                  UINT requiredBytes,
                                  TriangleBatchDrawStats* stats) {
  if (context.context == nullptr) {
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

  ComPtr<ID3D11Device> device;
  context.context->GetDevice(device.GetAddressOf());
  if (device == nullptr) {
    return E_UNEXPECTED;
  }

  D3D11_BUFFER_DESC bufferDesc = {};
  bufferDesc.ByteWidth = minBytes;
  bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
  bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  ComPtr<ID3D11Buffer> newBuffer;
  const HRESULT hr =
      device->CreateBuffer(&bufferDesc, nullptr, newBuffer.GetAddressOf());
  if (FAILED(hr) || newBuffer == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  context.vertexBuffer = newBuffer;
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
  if (context.context == nullptr) {
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

  ComPtr<ID3D11Device> device;
  context.context->GetDevice(device.GetAddressOf());
  if (device == nullptr) {
    return E_UNEXPECTED;
  }

  D3D11_BUFFER_DESC bufferDesc = {};
  bufferDesc.ByteWidth = minBytes;
  bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
  bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  ComPtr<ID3D11Buffer> newBuffer;
  const HRESULT hr =
      device->CreateBuffer(&bufferDesc, nullptr, newBuffer.GetAddressOf());
  if (FAILED(hr) || newBuffer == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  context.instanceBuffer = newBuffer;
  context.instanceBufferCapacityBytes = minBytes;
  if (stats != nullptr) {
    stats->resizedInstanceBuffer = true;
    stats->instanceBufferCapacityBytes = minBytes;
  }
  return S_OK;
}

HRESULT UpdateViewConstants(InstanceBatchDrawContext& context) {
  if (context.context == nullptr || context.viewConstantsBuffer == nullptr ||
      context.viewportWidth <= 0.0f || context.viewportHeight <= 0.0f) {
    return E_POINTER;
  }

  ViewConstants constants{};
  constants.viewportWidth = context.viewportWidth;
  constants.viewportHeight = context.viewportHeight;

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  const HRESULT mapHr = context.context->Map(context.viewConstantsBuffer.Get(), 0,
                                             D3D11_MAP_WRITE_DISCARD, 0,
                                             &mapped);
  if (FAILED(mapHr) || mapped.pData == nullptr) {
    return FAILED(mapHr) ? mapHr : E_FAIL;
  }

  std::memcpy(mapped.pData, &constants, sizeof(constants));
  context.context->Unmap(context.viewConstantsBuffer.Get(), 0);
  return S_OK;
}

template <typename TInstance>
HRESULT DrawInstanceBatch(InstanceBatchDrawContext& context,
                          const TInstance* instances, int instanceCount,
                          InstanceBatchDrawStats* stats) {
  if (instances == nullptr || instanceCount <= 0) {
    return S_OK;
  }

  if (context.context == nullptr || context.inputLayout == nullptr ||
      context.vertexShader == nullptr || context.pixelShader == nullptr ||
      context.blendState == nullptr || context.rasterizerState == nullptr ||
      context.geometryVertexBuffer == nullptr ||
      context.geometryVertexStrideBytes == 0 ||
      context.geometryVertexCount == 0 ||
      context.viewConstantsBuffer == nullptr) {
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

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  const auto uploadStart = std::chrono::steady_clock::now();
  const HRESULT mapHr = context.context->Map(context.instanceBuffer.Get(), 0,
                                             D3D11_MAP_WRITE_DISCARD, 0,
                                             &mapped);
  if (FAILED(mapHr) || mapped.pData == nullptr) {
    return FAILED(mapHr) ? mapHr : E_FAIL;
  }

  std::memcpy(mapped.pData, instances, byteSize);
  context.context->Unmap(context.instanceBuffer.Get(), 0);
  const auto uploadEnd = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->uploadInstanceDataMs += DurationMs(uploadStart, uploadEnd);
    stats->uploadedBytes += byteSize;
  }

  UINT strides[2] = {context.geometryVertexStrideBytes, sizeof(TInstance)};
  UINT offsets[2] = {0, 0};
  ID3D11Buffer* vertexBuffers[2] = {context.geometryVertexBuffer.Get(),
                                    context.instanceBuffer.Get()};
  ID3D11Buffer* cb = context.viewConstantsBuffer.Get();
  context.context->IASetInputLayout(context.inputLayout.Get());
  context.context->IASetVertexBuffers(0, 2, vertexBuffers, strides, offsets);
  context.context->IASetPrimitiveTopology(
      D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  context.context->VSSetShader(context.vertexShader.Get(), nullptr, 0);
  context.context->VSSetConstantBuffers(0, 1, &cb);
  context.context->PSSetShader(context.pixelShader.Get(), nullptr, 0);

  const float blendFactor[4] = {0, 0, 0, 0};
  context.context->OMSetBlendState(context.blendState.Get(), blendFactor,
                                   0xFFFFFFFF);
  context.context->RSSetState(context.rasterizerState.Get());
  const auto drawStart = std::chrono::steady_clock::now();
  context.context->DrawInstanced(context.geometryVertexCount,
                                 static_cast<UINT>(instanceCount), 0, 0);
  const auto drawEnd = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->issueDrawMs += DurationMs(drawStart, drawEnd);
  }
  return S_OK;
}

} // namespace

HRESULT DrawTriangleBatch(TriangleBatchDrawContext& context,
                          const TriangleVertexData& vertexData,
                          TriangleBatchDrawStats* stats) {
  if (vertexData.vertices == nullptr || vertexData.vertexCount <= 0) {
    return S_OK;
  }

  if (context.context == nullptr || context.inputLayout == nullptr ||
      context.vertexShader == nullptr || context.pixelShader == nullptr ||
      context.blendState == nullptr || context.rasterizerState == nullptr) {
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

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  const auto uploadStart = std::chrono::steady_clock::now();
  const HRESULT mapHr = context.context->Map(context.vertexBuffer.Get(), 0,
                                             D3D11_MAP_WRITE_DISCARD, 0,
                                             &mapped);
  if (FAILED(mapHr) || mapped.pData == nullptr) {
    return FAILED(mapHr) ? mapHr : E_FAIL;
  }

  std::memcpy(mapped.pData, vertexData.vertices, byteSize);
  context.context->Unmap(context.vertexBuffer.Get(), 0);
  const auto uploadEnd = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->uploadVertexDataMs += DurationMs(uploadStart, uploadEnd);
    stats->uploadedBytes += byteSize;
  }

  UINT stride = sizeof(batch::TriangleVertex);
  UINT offset = 0;
  ID3D11Buffer* vb = context.vertexBuffer.Get();
  context.context->IASetInputLayout(context.inputLayout.Get());
  context.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  context.context->IASetPrimitiveTopology(
      D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context.context->VSSetShader(context.vertexShader.Get(), nullptr, 0);
  context.context->PSSetShader(context.pixelShader.Get(), nullptr, 0);

  const float blendFactor[4] = {0, 0, 0, 0};
  context.context->OMSetBlendState(context.blendState.Get(), blendFactor,
                                   0xFFFFFFFF);
  context.context->RSSetState(context.rasterizerState.Get());
  const auto drawStart = std::chrono::steady_clock::now();
  context.context->Draw(static_cast<UINT>(vertexData.vertexCount), 0);
  const auto drawEnd = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->issueDrawMs += DurationMs(drawStart, drawEnd);
  }
  return S_OK;
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
  return DrawInstanceBatch(context, instanceData.instances,
                           instanceData.instanceCount, stats);
}

HRESULT DrawEllipseInstanceBatch(InstanceBatchDrawContext& context,
                                 const EllipseInstanceData& instanceData,
                                 InstanceBatchDrawStats* stats) {
  return DrawInstanceBatch(context, instanceData.instances,
                           instanceData.instanceCount, stats);
}

} // namespace fdv::d3d11::draw
