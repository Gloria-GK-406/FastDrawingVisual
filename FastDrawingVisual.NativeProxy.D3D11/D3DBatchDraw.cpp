#include "D3DBatchDraw.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace fdv::d3d11::draw {
namespace {

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

D2D1_COLOR_F ToD2DColor(const fdv::protocol::ColorArgb8& color) {
  return {
      static_cast<float>(color.r) / 255.0f,
      static_cast<float>(color.g) / 255.0f,
      static_cast<float>(color.b) / 255.0f,
      static_cast<float>(color.a) / 255.0f,
  };
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

HRESULT DrawTextBatch(const TextBatchDrawContext& context,
                      TextFormatCacheStore& textFormatCache,
                      const DrawTextData& textData,
                      TextBatchDrawStats* stats) {
  if (textData.items == nullptr || textData.count <= 0) {
    return S_OK;
  }

  if (context.d3dContext == nullptr || context.d2dContext == nullptr ||
      context.solidBrush == nullptr) {
    return E_POINTER;
  }

  const auto flushStart = std::chrono::steady_clock::now();
  context.d3dContext->Flush();
  const auto flushEnd = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->flushMs += DurationMs(flushStart, flushEnd);
  }

  context.d2dContext->BeginDraw();

  HRESULT result = S_OK;
  const auto recordStart = std::chrono::steady_clock::now();
  for (int i = 0; i < textData.count; ++i) {
    const auto& item = textData.items[i];
    if (item.text.empty()) {
      continue;
    }

    IDWriteTextFormat* textFormat =
        textFormatCache.Acquire(item.fontFamily, item.fontSize);
    if (textFormat == nullptr) {
      result = E_FAIL;
      break;
    }

    const D2D1_RECT_F layoutRect = {
        item.layoutLeft,
        item.layoutTop,
        item.layoutRight,
        item.layoutBottom,
    };

    context.solidBrush->SetColor(ToD2DColor(item.color));
    context.d2dContext->DrawTextW(
        item.text.c_str(), static_cast<UINT32>(item.text.size()), textFormat,
        &layoutRect, context.solidBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
        DWRITE_MEASURING_MODE_NATURAL);
  }
  const auto recordEnd = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->recordTextMs += DurationMs(recordStart, recordEnd);
  }

  const auto endDrawStart = std::chrono::steady_clock::now();
  const HRESULT endDrawHr = context.d2dContext->EndDraw();
  const auto endDrawEnd = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->endDrawMs += DurationMs(endDrawStart, endDrawEnd);
  }
  if (FAILED(endDrawHr)) {
    return endDrawHr;
  }

  return result;
}

} // namespace fdv::d3d11::draw
