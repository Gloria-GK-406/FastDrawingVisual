#pragma once

#include "D3DTaskType.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <d3dcompiler.h>
#include <wrl/client.h>

namespace fdv::d3d11 {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kCreationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
constexpr const wchar_t* kHiddenWindowClassName =
    L"FastDrawingVisual.D3D11.SharedDeviceWindow";
constexpr DXGI_FORMAT kRenderTargetFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr const wchar_t* kInstanceVertexShaderObject =
    L"Shader\\InstanceVS_Model4.cso";
constexpr const wchar_t* kInstancePixelShaderObject =
    L"Shader\\InstancePS_Model4.cso";

struct ViewConstants {
  float viewportWidth = 0.0f;
  float viewportHeight = 0.0f;
  float padding0 = 0.0f;
  float padding1 = 0.0f;
};

inline HRESULT CreateHardwareD3D11Device(ComPtr<ID3D11Device>& deviceOut,
                                         ComPtr<ID3D11DeviceContext>& contextOut,
                                         bool allowWarpFallback,
                                         bool allowFeatureLevel9X) {
  const D3D_FEATURE_LEVEL featureLevelsWith9X[] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_1,
  };
  const D3D_FEATURE_LEVEL featureLevelsNo9X[] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };

  const auto* featureLevels =
      allowFeatureLevel9X ? featureLevelsWith9X : featureLevelsNo9X;
  const UINT featureLevelCount = allowFeatureLevel9X
                                     ? ARRAYSIZE(featureLevelsWith9X)
                                     : ARRAYSIZE(featureLevelsNo9X);

  D3D_FEATURE_LEVEL createdFeatureLevel = D3D_FEATURE_LEVEL_9_1;
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, kCreationFlags, featureLevels,
      featureLevelCount, D3D11_SDK_VERSION,
      deviceOut.ReleaseAndGetAddressOf(), &createdFeatureLevel,
      contextOut.ReleaseAndGetAddressOf());
  if (FAILED(hr) && allowWarpFallback) {
    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, kCreationFlags, featureLevels,
        featureLevelCount, D3D11_SDK_VERSION,
        deviceOut.ReleaseAndGetAddressOf(), &createdFeatureLevel,
        contextOut.ReleaseAndGetAddressOf());
  }

  if (FAILED(hr) || deviceOut == nullptr || contextOut == nullptr) {
    deviceOut.Reset();
    contextOut.Reset();
    return FAILED(hr) ? hr : E_FAIL;
  }

  static_cast<void>(createdFeatureLevel);
  return S_OK;
}

inline LRESULT CALLBACK SharedHiddenWindowProc(HWND hwnd, UINT message,
                                               WPARAM wParam, LPARAM lParam) {
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

inline HRESULT EnsureHiddenWindowClassRegistered() {
  static bool registered = false;
  static HRESULT registrationHr = S_OK;
  if (registered) {
    return registrationHr;
  }

  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = SharedHiddenWindowProc;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = kHiddenWindowClassName;

  const ATOM atom = RegisterClassExW(&windowClass);
  if (atom == 0) {
    const DWORD error = GetLastError();
    if (error != ERROR_CLASS_ALREADY_EXISTS) {
      registrationHr = HRESULT_FROM_WIN32(error);
      return registrationHr;
    }
  }

  registered = true;
  registrationHr = S_OK;
  return S_OK;
}

inline double DurationMs(const std::chrono::steady_clock::time_point& start,
                         const std::chrono::steady_clock::time_point& end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

inline HRESULT LoadShaderBlob(const wchar_t* filePath,
                              ComPtr<ID3DBlob>& blobOut) {
  if (filePath == nullptr || *filePath == L'\0') {
    return E_INVALIDARG;
  }

  blobOut.Reset();
  const HRESULT hr =
      D3DReadFileToBlob(filePath, blobOut.ReleaseAndGetAddressOf());
  if (FAILED(hr) || blobOut == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }
  return S_OK;
}

inline HRESULT EnsureDynamicBuffer(ID3D11Device* device, UINT byteWidth,
                                   UINT bindFlags,
                                   ComPtr<ID3D11Buffer>& bufferOut) {
  if (device == nullptr || byteWidth == 0) {
    return E_INVALIDARG;
  }

  if (bufferOut != nullptr) {
    return S_OK;
  }

  D3D11_BUFFER_DESC bufferDesc = {};
  bufferDesc.ByteWidth = byteWidth;
  bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
  bufferDesc.BindFlags = bindFlags;
  bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  const HRESULT hr = device->CreateBuffer(&bufferDesc, nullptr,
                                          bufferOut.ReleaseAndGetAddressOf());
  if (FAILED(hr) || bufferOut == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  return S_OK;
}

inline HRESULT EnsureUnitQuadVertexBuffer(ID3D11Device* device,
                                          ComPtr<ID3D11Buffer>& bufferOut) {
  if (device == nullptr) {
    return E_INVALIDARG;
  }

  if (bufferOut != nullptr) {
    return S_OK;
  }

  struct Vertex {
    float x;
    float y;
  };
  const Vertex vertices[4] = {
      {-1.0f, -1.0f},
      {1.0f, -1.0f},
      {-1.0f, 1.0f},
      {1.0f, 1.0f},
  };

  D3D11_BUFFER_DESC bufferDesc = {};
  bufferDesc.ByteWidth = static_cast<UINT>(sizeof(vertices));
  bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
  bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

  D3D11_SUBRESOURCE_DATA initialData = {};
  initialData.pSysMem = vertices;

  const HRESULT hr = device->CreateBuffer(&bufferDesc, &initialData,
                                          bufferOut.ReleaseAndGetAddressOf());
  if (FAILED(hr) || bufferOut == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  return S_OK;
}

inline HRESULT UpdateViewConstants(ID3D11DeviceContext* context,
                                   ID3D11Buffer* viewConstantsBuffer,
                                   float viewportWidth,
                                   float viewportHeight) {
  if (context == nullptr || viewConstantsBuffer == nullptr ||
      viewportWidth <= 0.0f || viewportHeight <= 0.0f) {
    return E_INVALIDARG;
  }

  ViewConstants constants{};
  constants.viewportWidth = viewportWidth;
  constants.viewportHeight = viewportHeight;

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  const HRESULT hr = context->Map(viewConstantsBuffer, 0, D3D11_MAP_WRITE_DISCARD,
                                  0, &mapped);
  if (FAILED(hr) || mapped.pData == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  std::memcpy(mapped.pData, &constants, sizeof(constants));
  context->Unmap(viewConstantsBuffer, 0);
  return S_OK;
}

inline HRESULT DrawShapePass(ID3D11DeviceContext* context,
                             ID3D11InputLayout* inputLayout,
                             ID3D11VertexShader* vertexShader,
                             ID3D11PixelShader* pixelShader,
                             ID3D11BlendState* blendState,
                             ID3D11RasterizerState* rasterizerState,
                             ID3D11Buffer* unitQuadVertexBuffer,
                             ID3D11Buffer* viewConstantsBuffer,
                             const D3D11FrameTask& task) {
  if (task.shapeInstanceCount <= 0) {
    return S_OK;
  }

  if (context == nullptr || inputLayout == nullptr || vertexShader == nullptr ||
      pixelShader == nullptr || blendState == nullptr ||
      rasterizerState == nullptr || unitQuadVertexBuffer == nullptr ||
      task.instanceBuffer == nullptr || viewConstantsBuffer == nullptr) {
    return E_POINTER;
  }

  const HRESULT constantsHr = UpdateViewConstants(
      context, viewConstantsBuffer, task.viewportWidth, task.viewportHeight);
  if (FAILED(constantsHr)) {
    return constantsHr;
  }

  UINT strides[2] = {sizeof(float) * 2u, sizeof(batch::ShapeInstance)};
  UINT offsets[2] = {0, 0};
  ID3D11Buffer* vertexBuffers[2] = {unitQuadVertexBuffer,
                                    task.instanceBuffer};
  ID3D11Buffer* cb = viewConstantsBuffer;

  context->IASetInputLayout(inputLayout);
  context->IASetVertexBuffers(0, 2, vertexBuffers, strides, offsets);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  context->VSSetShader(vertexShader, nullptr, 0);
  context->VSSetConstantBuffers(0, 1, &cb);
  context->PSSetShader(pixelShader, nullptr, 0);

  const float blendFactor[4] = {0, 0, 0, 0};
  context->OMSetBlendState(blendState, blendFactor, 0xFFFFFFFF);
  context->RSSetState(rasterizerState);
  context->DrawInstanced(4u, static_cast<UINT>(task.shapeInstanceCount), 0, 0);
  return S_OK;
}

} // namespace
} // namespace fdv::d3d11
