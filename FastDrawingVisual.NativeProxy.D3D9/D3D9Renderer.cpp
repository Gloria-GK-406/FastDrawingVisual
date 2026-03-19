#include "D3D9Renderer.h"

#include "BatchComplier.h"
#include "D3DBatchDraw.h"
#include "../FastDrawingVisual.LogCore/FdvLogCoreExports.h"
#include "../FastDrawingVisual.NativeProxy.TextD2D/D2DTextRenderer.h"

#include <d3d9.h>
#include <d3d11.h>
#include <d3dx9.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <new>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace fdv::d3d9 {

using Microsoft::WRL::ComPtr;
constexpr int kFrameCount = 2;

enum class SurfaceState : uint8_t {
  Ready = 0,
  Drawing = 1,
  ReadyForPresent = 2,
};

struct SurfaceSlot {
  ComPtr<IDirect3DTexture9> renderTexture = nullptr;
  ComPtr<IDirect3DSurface9> renderTarget = nullptr;
  ComPtr<IDirect3DQuery9> renderDoneQuery = nullptr;
  ComPtr<ID3D11Texture2D> textSharedTexture = nullptr;
  HANDLE sharedHandle = nullptr;
  SurfaceState state = SurfaceState::Ready;
};

struct D3D9RendererState {
  ComPtr<IDirect3D9Ex> d3d9 = nullptr;
  ComPtr<IDirect3DDevice9Ex> device = nullptr;
  ComPtr<ID3D11Device> textDevice = nullptr;
  ComPtr<ID3D11DeviceContext> textContext = nullptr;
  ComPtr<ID3D11Query> textDoneQuery = nullptr;
  ComPtr<IDirect3DVertexDeclaration9> instanceVertexDeclaration = nullptr;
  ComPtr<IDirect3DVertexShader9> instanceVertexShader = nullptr;
  ComPtr<IDirect3DPixelShader9> instancePixelShader = nullptr;
  ComPtr<IDirect3DVertexBuffer9> unitQuadVertexBuffer = nullptr;
  ComPtr<IDirect3DIndexBuffer9> unitQuadIndexBuffer = nullptr;
  ComPtr<IDirect3DVertexBuffer9> dynamicInstanceVertexBuffer = nullptr;
  UINT dynamicInstanceVertexCapacityBytes = 0;
  batch::BatchCompiler batchCompiler{};
  SurfaceSlot slots[kFrameCount];
  ComPtr<IDirect3DSurface9> presentingSurface = nullptr;
  std::unique_ptr<fdv::textd2d::D2DTextRenderer> textRenderer;
  int activeTextTargetSlotIndex = -1;

  HWND hwnd = nullptr;
  int width = 0;
  int height = 0;
  bool frontBufferAvailable = true;
  bool csInitialized = false;
  CRITICAL_SECTION cs{};
};

namespace {

constexpr uint32_t kMetricWindowSec = 1;
constexpr const wchar_t* kInstanceVertexShaderPath = L"Shader\\InstanceVS_Model3.cso";
constexpr const wchar_t* kInstancePixelShaderPath = L"Shader\\InstancePS_Model3.cso";
constexpr DXGI_FORMAT kTextInteropFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

int FindSlotIndex(const D3D9RendererState* state, const SurfaceSlot* slot) {
  if (state == nullptr || slot == nullptr) {
    return -1;
  }

  for (int index = 0; index < kFrameCount; ++index) {
    if (&state->slots[index] == slot) {
      return index;
    }
  }

  return -1;
}

int FindSlotByState(const D3D9RendererState* state, SurfaceState slotState) {
  if (state == nullptr) {
    return -1;
  }

  for (int index = 0; index < kFrameCount; ++index) {
    if (state->slots[index].state == slotState) {
      return index;
    }
  }

  return -1;
}

void DemoteReadyForPresentSlots(D3D9RendererState* state, int keepIndex) {
  if (state == nullptr) {
    return;
  }

  for (int index = 0; index < kFrameCount; ++index) {
    if (index != keepIndex &&
        state->slots[index].state == SurfaceState::ReadyForPresent) {
      state->slots[index].state = SurfaceState::Ready;
    }
  }
}

constexpr const wchar_t* kParseSubmitMetricFormat =
    L"name={name} periodSec={periodSec}s samples={count} avgMs={avg} minMs={min} maxMs={max}";

D3DCOLOR ToD3DClearColor(const float color[4]) {
  return D3DCOLOR_COLORVALUE(color[0], color[1], color[2], color[3]);
}

void ReleaseVertexDeclaration(ComPtr<IDirect3DVertexDeclaration9>& declaration) {
  declaration.Reset();
}

void ReleaseVertexShader(ComPtr<IDirect3DVertexShader9>& shader) {
  shader.Reset();
}

void ReleasePixelShader(ComPtr<IDirect3DPixelShader9>& shader) {
  shader.Reset();
}

void ReleaseVertexBuffer(ComPtr<IDirect3DVertexBuffer9>& buffer) {
  buffer.Reset();
}

void ReleaseIndexBuffer(ComPtr<IDirect3DIndexBuffer9>& buffer) {
  buffer.Reset();
}

void LogShaderCompileError(ID3DXBuffer* errorBuffer) {
  if (errorBuffer == nullptr || errorBuffer->GetBufferPointer() == nullptr) {
    return;
  }

  OutputDebugStringA("FDV: D3D9 shader compile failed: ");
  OutputDebugStringA(static_cast<const char*>(errorBuffer->GetBufferPointer()));
  OutputDebugStringA("\n");
}

void ReleaseDrawPipeline(D3D9RendererState* state) {
  if (state == nullptr) {
    return;
  }

  ReleaseVertexBuffer(state->dynamicInstanceVertexBuffer);
  state->dynamicInstanceVertexCapacityBytes = 0;
  ReleaseVertexBuffer(state->unitQuadVertexBuffer);
  ReleaseIndexBuffer(state->unitQuadIndexBuffer);
  ReleasePixelShader(state->instancePixelShader);
  ReleaseVertexShader(state->instanceVertexShader);
  ReleaseVertexDeclaration(state->instanceVertexDeclaration);
}

bool LoadShaderBytecode(const wchar_t* sourcePath,
                        std::vector<std::uint8_t>& bytecodeOut) {
  if (sourcePath == nullptr || *sourcePath == L'\0') {
    return false;
  }

  bytecodeOut.clear();
  HANDLE file = CreateFileW(sourcePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
      size.QuadPart > static_cast<LONGLONG>(UINT32_MAX)) {
    CloseHandle(file);
    return false;
  }

  bytecodeOut.resize(static_cast<std::size_t>(size.QuadPart));
  DWORD bytesRead = 0;
  const BOOL readOk = ReadFile(file, bytecodeOut.data(),
                               static_cast<DWORD>(bytecodeOut.size()),
                               &bytesRead, nullptr);
  CloseHandle(file);
  if (!readOk || bytesRead != bytecodeOut.size()) {
    bytecodeOut.clear();
    return false;
  }

  return true;
}

bool CreateUnitQuadVertexBuffer(D3D9RendererState* state) {
  if (state == nullptr || state->device == nullptr) {
    return false;
  }

  ReleaseVertexBuffer(state->unitQuadVertexBuffer);

  constexpr float kUnitQuadVertices[8] = {
      -1.0f, -1.0f,
       1.0f, -1.0f,
      -1.0f,  1.0f,
       1.0f,  1.0f,
  };

  HRESULT hr = state->device->CreateVertexBuffer(
      static_cast<UINT>(sizeof(kUnitQuadVertices)),
      D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
      state->unitQuadVertexBuffer.ReleaseAndGetAddressOf(), nullptr);
  if (FAILED(hr) || state->unitQuadVertexBuffer == nullptr) {
    ReleaseVertexBuffer(state->unitQuadVertexBuffer);
    return false;
  }

  void* vertices = nullptr;
  hr = state->unitQuadVertexBuffer->Lock(
      0, sizeof(kUnitQuadVertices), &vertices, D3DLOCK_DISCARD);
  if (FAILED(hr) || vertices == nullptr) {
    ReleaseVertexBuffer(state->unitQuadVertexBuffer);
    return false;
  }

  memcpy(vertices, kUnitQuadVertices, sizeof(kUnitQuadVertices));
  state->unitQuadVertexBuffer->Unlock();
  return true;
}

bool CreateUnitQuadIndexBuffer(D3D9RendererState* state) {
  if (state == nullptr || state->device == nullptr) {
    return false;
  }

  ReleaseIndexBuffer(state->unitQuadIndexBuffer);

  constexpr std::uint16_t kUnitQuadIndices[6] = {
      0, 1, 2,
      2, 1, 3,
  };

  HRESULT hr = state->device->CreateIndexBuffer(
      static_cast<UINT>(sizeof(kUnitQuadIndices)), D3DUSAGE_WRITEONLY,
      D3DFMT_INDEX16, D3DPOOL_DEFAULT,
      state->unitQuadIndexBuffer.ReleaseAndGetAddressOf(), nullptr);
  if (FAILED(hr) || state->unitQuadIndexBuffer == nullptr) {
    ReleaseIndexBuffer(state->unitQuadIndexBuffer);
    return false;
  }

  void* indices = nullptr;
  hr = state->unitQuadIndexBuffer->Lock(0, sizeof(kUnitQuadIndices), &indices, 0);
  if (FAILED(hr) || indices == nullptr) {
    ReleaseIndexBuffer(state->unitQuadIndexBuffer);
    return false;
  }

  memcpy(indices, kUnitQuadIndices, sizeof(kUnitQuadIndices));
  state->unitQuadIndexBuffer->Unlock();
  return true;
}

bool CreateDrawPipeline(D3D9RendererState* state) {
  if (state == nullptr || state->device == nullptr) {
    return false;
  }

  ReleaseDrawPipeline(state);

  D3DCAPS9 caps{};
  if (FAILED(state->device->GetDeviceCaps(&caps)) ||
      caps.VertexShaderVersion < D3DVS_VERSION(3, 0) ||
      caps.PixelShaderVersion < D3DPS_VERSION(3, 0)) {
    ReleaseDrawPipeline(state);
    return false;
  }

  std::vector<std::uint8_t> instanceVertexBytecode;
  std::vector<std::uint8_t> instancePixelBytecode;
  if (!LoadShaderBytecode(kInstanceVertexShaderPath, instanceVertexBytecode) ||
      !LoadShaderBytecode(kInstancePixelShaderPath, instancePixelBytecode)) {
    ReleaseDrawPipeline(state);
    return false;
  }

  static const D3DVERTEXELEMENT9 kInstanceElements[] = {
      {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,
       0},
      {1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,
       0},
      {1, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
       D3DDECLUSAGE_TEXCOORD, 1},
      {1, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
       D3DDECLUSAGE_TEXCOORD, 2},
      {1, 48, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
       D3DDECLUSAGE_TEXCOORD, 3},
      {1, 64, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
       D3DDECLUSAGE_TEXCOORD, 4},
      D3DDECL_END()};

  HRESULT hr = state->device->CreateVertexDeclaration(
      kInstanceElements, state->instanceVertexDeclaration.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state->instanceVertexDeclaration == nullptr) {
    ReleaseDrawPipeline(state);
    return false;
  }

  hr = state->device->CreateVertexShader(
      reinterpret_cast<const DWORD*>(instanceVertexBytecode.data()),
      state->instanceVertexShader.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state->instanceVertexShader == nullptr) {
    ReleaseDrawPipeline(state);
    return false;
  }

  hr = state->device->CreatePixelShader(
      reinterpret_cast<const DWORD*>(instancePixelBytecode.data()),
      state->instancePixelShader.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state->instancePixelShader == nullptr) {
    ReleaseDrawPipeline(state);
    return false;
  }

  if (!CreateUnitQuadVertexBuffer(state)) {
    return false;
  }

  return CreateUnitQuadIndexBuffer(state);
}

void ReleaseFrameResources(D3D9RendererState* state) {
  if (state == nullptr) {
    return;
  }

  if (state->textRenderer != nullptr) {
    state->textRenderer->ReleaseRenderTargetResources();
  }
  state->activeTextTargetSlotIndex = -1;
  state->presentingSurface.Reset();

  for (int i = 0; i < kFrameCount; ++i) {
    state->slots[i].textSharedTexture.Reset();
    state->slots[i].sharedHandle = nullptr;
    state->slots[i].renderDoneQuery.Reset();
    state->slots[i].renderTarget.Reset();
    state->slots[i].renderTexture.Reset();
    state->slots[i].state = SurfaceState::Ready;
  }
}

void ReleaseTextInteropDeviceResources(D3D9RendererState* state) {
  if (state == nullptr) {
    return;
  }

  if (state->textRenderer != nullptr) {
    state->textRenderer->ReleaseRenderTargetResources();
    state->textRenderer->ReleaseDeviceResources();
    state->textRenderer.reset();
  }

  state->activeTextTargetSlotIndex = -1;
  state->textDoneQuery.Reset();
  state->textContext.Reset();
  state->textDevice.Reset();

  for (int i = 0; i < kFrameCount; ++i) {
    state->slots[i].textSharedTexture.Reset();
  }
}

HRESULT WaitForD3D9Query(IDirect3DQuery9* query) {
  if (query == nullptr) {
    return S_OK;
  }

  HRESULT hr = query->Issue(D3DISSUE_END);
  if (FAILED(hr)) {
    return hr;
  }

  while (true) {
    hr = query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    if (hr == S_OK) {
      return S_OK;
    }
    if (hr != S_FALSE) {
      return hr;
    }
    YieldProcessor();
  }
}

HRESULT WaitForD3D11Query(ID3D11DeviceContext* context, ID3D11Query* query) {
  if (context == nullptr || query == nullptr) {
    return E_POINTER;
  }

  context->End(query);
  context->Flush();

  while (true) {
    const HRESULT hr = context->GetData(query, nullptr, 0, 0);
    if (hr == S_OK) {
      return S_OK;
    }
    if (hr != S_FALSE) {
      return hr;
    }
    YieldProcessor();
  }
}

HRESULT OpenTextInteropTargets(D3D9RendererState* state) {
  if (state == nullptr || state->textDevice == nullptr) {
    return E_UNEXPECTED;
  }

  if (state->textRenderer != nullptr) {
    state->textRenderer->ReleaseRenderTargetResources();
  }
  state->activeTextTargetSlotIndex = -1;

  for (int i = 0; i < kFrameCount; ++i) {
    auto& slot = state->slots[i];
    slot.textSharedTexture.Reset();

    if (slot.renderTexture == nullptr || slot.sharedHandle == nullptr) {
      return E_FAIL;
    }

    HRESULT hr = state->textDevice->OpenSharedResource(
        slot.sharedHandle, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(slot.textSharedTexture.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || slot.textSharedTexture == nullptr) {
      return FAILED(hr) ? hr : E_FAIL;
    }
  }

  return S_OK;
}

HRESULT EnsureTextInteropResources(D3D9RendererState* state) {
  if (state == nullptr) {
    return E_OUTOFMEMORY;
  }

  if (state->textDevice != nullptr && state->textContext != nullptr &&
      state->textDoneQuery != nullptr && state->textRenderer != nullptr) {
    return S_OK;
  }

  ReleaseTextInteropDeviceResources(state);

  const D3D_FEATURE_LEVEL featureLevels[] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_1,
  };
  D3D_FEATURE_LEVEL supportedFeatureLevel = D3D_FEATURE_LEVEL_9_1;
  constexpr UINT kTextCreationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, kTextCreationFlags,
      featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
      state->textDevice.ReleaseAndGetAddressOf(), &supportedFeatureLevel,
      state->textContext.ReleaseAndGetAddressOf());
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, kTextCreationFlags,
        featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
        state->textDevice.ReleaseAndGetAddressOf(), &supportedFeatureLevel,
        state->textContext.ReleaseAndGetAddressOf());
  }

  if (FAILED(hr) || state->textDevice == nullptr || state->textContext == nullptr) {
    ReleaseTextInteropDeviceResources(state);
    return FAILED(hr) ? hr : E_FAIL;
  }
  static_cast<void>(supportedFeatureLevel);

  D3D11_QUERY_DESC textDoneQueryDesc = {};
  textDoneQueryDesc.Query = D3D11_QUERY_EVENT;
  hr = state->textDevice->CreateQuery(
      &textDoneQueryDesc, state->textDoneQuery.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state->textDoneQuery == nullptr) {
    ReleaseTextInteropDeviceResources(state);
    return FAILED(hr) ? hr : E_FAIL;
  }

  state->textRenderer = std::make_unique<fdv::textd2d::D2DTextRenderer>();
  if (state->textRenderer == nullptr) {
    ReleaseTextInteropDeviceResources(state);
    return E_OUTOFMEMORY;
  }

  hr = state->textRenderer->EnsureDeviceResources(state->textDevice.Get());
  if (FAILED(hr)) {
    ReleaseTextInteropDeviceResources(state);
    return hr;
  }

  hr = OpenTextInteropTargets(state);
  if (FAILED(hr)) {
    ReleaseTextInteropDeviceResources(state);
    return hr;
  }

  return S_OK;
}

HRESULT DrawSharedTextBatch(
    D3D9RendererState* state, SurfaceSlot* drawSlot,
    const fdv::nativeproxy::shared::batch::TextBatchItem* items, int count) {
  if (items == nullptr || count <= 0) {
    return S_OK;
  }

  if (state == nullptr || drawSlot == nullptr) {
    return E_INVALIDARG;
  }

  const int slotIndex = FindSlotIndex(state, drawSlot);
  if (slotIndex < 0) {
    return E_INVALIDARG;
  }

  HRESULT hr = EnsureTextInteropResources(state);
  if (FAILED(hr)) {
    return hr;
  }

  if (drawSlot->textSharedTexture == nullptr || state->textRenderer == nullptr ||
      state->textContext == nullptr || state->textDoneQuery == nullptr) {
    return E_UNEXPECTED;
  }

  hr = WaitForD3D9Query(drawSlot->renderDoneQuery.Get());
  if (FAILED(hr)) {
    return hr;
  }

  if (state->activeTextTargetSlotIndex != slotIndex) {
    hr = state->textRenderer->CreateTargetFromTexture(
        drawSlot->textSharedTexture.Get(), kTextInteropFormat);
    if (FAILED(hr)) {
      state->activeTextTargetSlotIndex = -1;
      return hr;
    }
    state->activeTextTargetSlotIndex = slotIndex;
  }

  hr = state->textRenderer->DrawTextBatch(state->textContext.Get(), items, count);
  if (FAILED(hr)) {
    return hr;
  }

  return WaitForD3D11Query(state->textContext.Get(), state->textDoneQuery.Get());
}

HRESULT DrawSharedImageBatch(
    D3D9RendererState* state, SurfaceSlot* drawSlot,
    const fdv::nativeproxy::shared::batch::ImageBatchItem* items, int count) {
  if (items == nullptr || count <= 0) {
    return S_OK;
  }

  if (state == nullptr || drawSlot == nullptr) {
    return E_INVALIDARG;
  }

  const int slotIndex = FindSlotIndex(state, drawSlot);
  if (slotIndex < 0) {
    return E_INVALIDARG;
  }

  HRESULT hr = EnsureTextInteropResources(state);
  if (FAILED(hr)) {
    return hr;
  }

  if (drawSlot->textSharedTexture == nullptr || state->textRenderer == nullptr ||
      state->textContext == nullptr || state->textDoneQuery == nullptr) {
    return E_UNEXPECTED;
  }

  hr = WaitForD3D9Query(drawSlot->renderDoneQuery.Get());
  if (FAILED(hr)) {
    return hr;
  }

  if (state->activeTextTargetSlotIndex != slotIndex) {
    hr = state->textRenderer->CreateTargetFromTexture(
        drawSlot->textSharedTexture.Get(), kTextInteropFormat);
    if (FAILED(hr)) {
      state->activeTextTargetSlotIndex = -1;
      return hr;
    }
    state->activeTextTargetSlotIndex = slotIndex;
  }

  hr = state->textRenderer->DrawImageBatch(state->textContext.Get(), items, count);
  if (FAILED(hr)) {
    return hr;
  }

  return WaitForD3D11Query(state->textContext.Get(), state->textDoneQuery.Get());
}

bool CreateFrameResources(D3D9RendererState* state) {
  if (state == nullptr || state->device == nullptr || state->width <= 0 ||
      state->height <= 0) {
    return false;
  }

  ReleaseFrameResources(state);

  for (int i = 0; i < kFrameCount; ++i) {
    HANDLE sharedHandle = nullptr;
    HRESULT hr = state->device->CreateTexture(
        static_cast<UINT>(state->width), static_cast<UINT>(state->height),
        1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        state->slots[i].renderTexture.ReleaseAndGetAddressOf(), &sharedHandle);
    if (FAILED(hr)) {
      ReleaseFrameResources(state);
      return false;
    }

    state->slots[i].sharedHandle = sharedHandle;

    hr = state->slots[i].renderTexture->GetSurfaceLevel(
        0, state->slots[i].renderTarget.ReleaseAndGetAddressOf());
    if (FAILED(hr) || state->slots[i].renderTarget == nullptr) {
      ReleaseFrameResources(state);
      return false;
    }

    hr = state->device->CreateQuery(
        D3DQUERYTYPE_EVENT,
        state->slots[i].renderDoneQuery.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      ReleaseFrameResources(state);
      return false;
    }
    state->slots[i].state = SurfaceState::Ready;
  }

  HRESULT hr = state->device->CreateRenderTarget(
      static_cast<UINT>(state->width), static_cast<UINT>(state->height),
      D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
      state->presentingSurface.ReleaseAndGetAddressOf(), nullptr);
  if (FAILED(hr)) {
    ReleaseFrameResources(state);
    return false;
  }

  if (state->textDevice != nullptr && FAILED(OpenTextInteropTargets(state))) {
    ReleaseFrameResources(state);
    return false;
  }

  return true;
}

HRESULT CheckDeviceReady(D3D9RendererState* state) {
  if (state == nullptr || state->device == nullptr) {
    return E_UNEXPECTED;
  }

  const HRESULT hr = state->device->CheckDeviceState(state->hwnd);
  if (hr == S_PRESENT_OCCLUDED || hr == S_PRESENT_MODE_CHANGED || hr == S_OK) {
    return S_OK;
  }

  if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET ||
      hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED) {
    return hr;
  }

  return hr;
}

bool CreateDeviceAndSurface(D3D9RendererState* state) {
  if (state == nullptr || state->hwnd == nullptr || state->width <= 0 ||
      state->height <= 0) {
    return false;
  }

  if (state->d3d9 == nullptr) {
    ComPtr<IDirect3D9Ex> d3d9;
    if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, d3d9.GetAddressOf())) ||
        d3d9 == nullptr) {
      return false;
    }
    state->d3d9 = d3d9;
  }

  D3DPRESENT_PARAMETERS parameters = {};
  parameters.Windowed = TRUE;
  parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
  parameters.BackBufferFormat = D3DFMT_UNKNOWN;
  parameters.BackBufferWidth = 1;
  parameters.BackBufferHeight = 1;
  parameters.BackBufferCount = 1;
  parameters.hDeviceWindow = state->hwnd;
  parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  parameters.EnableAutoDepthStencil = FALSE;

  HRESULT hr = state->d3d9->CreateDeviceEx(
      D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, state->hwnd,
      D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED |
          D3DCREATE_FPU_PRESERVE,
      &parameters, nullptr, state->device.ReleaseAndGetAddressOf());
  if (FAILED(hr)) {
    hr = state->d3d9->CreateDeviceEx(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, state->hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED |
            D3DCREATE_FPU_PRESERVE,
        &parameters, nullptr, state->device.ReleaseAndGetAddressOf());
  }

  if (FAILED(hr) || state->device == nullptr) {
    return false;
  }

  if (!CreateDrawPipeline(state)) {
    state->device.Reset();
    return false;
  }

  if (!CreateFrameResources(state)) {
    ReleaseDrawPipeline(state);
    state->device.Reset();
    return false;
  }

  return true;
}

void ReleaseDeviceResources(D3D9RendererState* state) {
  if (state == nullptr) {
    return;
  }

  ReleaseFrameResources(state);
  ReleaseTextInteropDeviceResources(state);
  ReleaseDrawPipeline(state);
  state->device.Reset();
}

bool ResetDeviceAndSurface(D3D9RendererState* state) {
  if (state == nullptr || state->device == nullptr) {
    return false;
  }

  ReleaseFrameResources(state);
  ReleaseDrawPipeline(state);

  D3DPRESENT_PARAMETERS parameters = {};
  parameters.Windowed = TRUE;
  parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
  parameters.BackBufferFormat = D3DFMT_UNKNOWN;
  parameters.BackBufferWidth = 1;
  parameters.BackBufferHeight = 1;
  parameters.BackBufferCount = 1;
  parameters.hDeviceWindow = state->hwnd;
  parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  parameters.EnableAutoDepthStencil = FALSE;

  const HRESULT hr = state->device->ResetEx(&parameters, nullptr);
  if (FAILED(hr)) {
    return false;
  }

  if (!CreateDrawPipeline(state)) {
    return false;
  }

  return CreateFrameResources(state);
}

}  // namespace

D3D9Renderer::D3D9Renderer(HWND hwnd, int width, int height) {
  state_ = new (std::nothrow) D3D9RendererState();
  if (state_ == nullptr) {
    return;
  }

  state_->hwnd = hwnd;
  state_->width = width;
  state_->height = height;
  InitializeCriticalSectionAndSpinCount(&state_->cs, 1000);
  state_->csInitialized = true;
}

D3D9Renderer::~D3D9Renderer() {
  UnregisterMetrics();

  if (state_ == nullptr) {
    return;
  }

  ReleaseDeviceResources(state_);
  state_->d3d9.Reset();

  if (state_->csInitialized) {
    DeleteCriticalSection(&state_->cs);
    state_->csInitialized = false;
  }

  delete state_;
  state_ = nullptr;
}

HRESULT D3D9Renderer::Initialize() {
  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  if (state_->device != nullptr) {
    return S_OK;
  }

  if (state_->hwnd == nullptr || state_->width <= 0 || state_->height <= 0) {
    return E_INVALIDARG;
  }

  RendererLockGuard lock(state_->csInitialized ? &state_->cs : nullptr);
  return CreateDeviceAndSurface(state_) ? S_OK : E_FAIL;
}

HRESULT D3D9Renderer::Resize(int width, int height) {
  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  if (width <= 0 || height <= 0) {
    return E_INVALIDARG;
  }

  RendererLockGuard lock(state_->csInitialized ? &state_->cs : nullptr);
  state_->width = width;
  state_->height = height;

  if (state_->device == nullptr) {
    return CreateDeviceAndSurface(state_) ? S_OK : E_FAIL;
  }

  return CreateFrameResources(state_) ? S_OK : E_FAIL;
}

bool D3D9Renderer::ValidateFramePacket(const void* framePacket,
                                       int framePacketBytes) const {
  return framePacket != nullptr &&
         framePacketBytes >= static_cast<int>(sizeof(LayeredFramePacket));
}

HRESULT D3D9Renderer::BeginSubmitFrame(SurfaceSlot*& drawSlot,
                                       int& drawSlotIndex) {
  drawSlot = nullptr;
  drawSlotIndex = -1;

  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  if (state_->device == nullptr) {
    return E_UNEXPECTED;
  }

  HRESULT hr = CheckDeviceReady(state_);
  if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET ||
      hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED) {
    if (!ResetDeviceAndSurface(state_)) {
      return E_FAIL;
    }
  } else if (FAILED(hr)) {
    return hr;
  }

  const int readySlotIndex = FindSlotByState(state_, SurfaceState::Ready);
  if (readySlotIndex < 0) {
    return S_FALSE;
  }

  drawSlot = &state_->slots[readySlotIndex];
  drawSlot->state = SurfaceState::Drawing;

  drawSlotIndex = readySlotIndex;
  return S_OK;
}

HRESULT D3D9Renderer::SubmitCompiledBatches(SurfaceSlot* drawSlot,
                                            const LayerPacket& layer) {
  if (state_ == nullptr || state_->device == nullptr || drawSlot == nullptr ||
      drawSlot->renderTarget == nullptr) {
    return E_UNEXPECTED;
  }

  if (layer.commandData == nullptr || layer.commandBytes <= 0) {
    return S_OK;
  }

  IDirect3DDevice9* device = state_->device.Get();
  HRESULT hr = device->SetRenderTarget(0, drawSlot->renderTarget.Get());
  if (FAILED(hr)) {
    return hr;
  }

  D3DVIEWPORT9 viewport{};
  viewport.X = 0;
  viewport.Y = 0;
  viewport.Width = static_cast<DWORD>(state_->width);
  viewport.Height = static_cast<DWORD>(state_->height);
  viewport.MinZ = 0.0f;
  viewport.MaxZ = 1.0f;
  hr = device->SetViewport(&viewport);
  if (FAILED(hr)) {
    return hr;
  }

  state_->batchCompiler.Reset(state_->width, state_->height, layer.commandData,
                              layer.commandBytes, layer.blobData,
                              layer.blobBytes);

  HRESULT submitHr = S_OK;
  batch::CompiledBatchView batch{};
  HRESULT batchHr = S_OK;
  bool sceneOpen = false;
  while ((batchHr = state_->batchCompiler.TryGetNextBatch(batch)) == S_OK) {
    switch (batch.kind) {
    case batch::BatchKind::Clear:
      if (sceneOpen) {
        const HRESULT endSceneHr = device->EndScene();
        sceneOpen = false;
        if (FAILED(endSceneHr)) {
          submitHr = endSceneHr;
          break;
        }
      }
      device->Clear(0, nullptr, D3DCLEAR_TARGET,
                    ToD3DClearColor(batch.clearColor), 1.0f, 0);
      break;

    case batch::BatchKind::Triangles: {
      if (!sceneOpen) {
        hr = device->BeginScene();
        if (FAILED(hr)) {
          submitHr = hr;
          break;
        }
        sceneOpen = true;
        draw::SetupRenderState(device);
      }
      submitHr = E_NOTIMPL;
      break;
    }

    case batch::BatchKind::ShapeInstances: {
      if (!sceneOpen) {
        hr = device->BeginScene();
        if (FAILED(hr)) {
          submitHr = hr;
          break;
        }
        sceneOpen = true;
        draw::SetupRenderState(device);
      }

      const auto& shapeInstances = state_->batchCompiler.GetShapeInstances();
      draw::InstanceBatchDrawContext instanceContext{};
      instanceContext.device = device;
      instanceContext.vertexDeclaration = state_->instanceVertexDeclaration.Get();
      instanceContext.vertexShader = state_->instanceVertexShader.Get();
      instanceContext.pixelShader = state_->instancePixelShader.Get();
      instanceContext.geometryVertexBuffer = state_->unitQuadVertexBuffer.Get();
      instanceContext.geometryIndexBuffer = state_->unitQuadIndexBuffer.Get();
      instanceContext.geometryVertexStrideBytes = sizeof(float) * 2;
      instanceContext.geometryVertexCount = 4;
      instanceContext.geometryPrimitiveCount = 2;
      instanceContext.instanceBuffer = state_->dynamicInstanceVertexBuffer.Detach();
      instanceContext.instanceBufferCapacityBytes =
          state_->dynamicInstanceVertexCapacityBytes;
      instanceContext.viewportWidth = static_cast<float>(state_->width);
      instanceContext.viewportHeight = static_cast<float>(state_->height);
      const draw::ShapeInstanceData instanceData{
          shapeInstances.data(), static_cast<int>(shapeInstances.size())};
      submitHr = draw::DrawShapeInstanceBatch(instanceContext, instanceData);
      state_->dynamicInstanceVertexBuffer.Attach(instanceContext.instanceBuffer);
      state_->dynamicInstanceVertexCapacityBytes =
          instanceContext.instanceBufferCapacityBytes;
      break;
    }

    case batch::BatchKind::Text: {
      if (sceneOpen) {
        const HRESULT endSceneHr = device->EndScene();
        sceneOpen = false;
        if (FAILED(endSceneHr)) {
          submitHr = endSceneHr;
          break;
        }
      }

      const auto& textItems = state_->batchCompiler.GetTextItems();
      submitHr = DrawSharedTextBatch(state_, drawSlot, textItems.data(),
                                     static_cast<int>(textItems.size()));
      break;
    }

    case batch::BatchKind::Image: {
      if (sceneOpen) {
        const HRESULT endSceneHr = device->EndScene();
        sceneOpen = false;
        if (FAILED(endSceneHr)) {
          submitHr = endSceneHr;
          break;
        }
      }

      const auto& imageItems = state_->batchCompiler.GetImageItems();
      submitHr = DrawSharedImageBatch(state_, drawSlot, imageItems.data(),
                                      static_cast<int>(imageItems.size()));
      break;
    }
    }

    if (FAILED(submitHr)) {
      break;
    }
  }

  if (SUCCEEDED(submitHr) && batchHr != S_FALSE) {
    submitHr = FAILED(batchHr) ? batchHr : E_INVALIDARG;
  }

  HRESULT endSceneHr = S_OK;
  if (sceneOpen) {
    endSceneHr = device->EndScene();
  }
  if (FAILED(submitHr)) {
    return submitHr;
  }
  if (FAILED(endSceneHr)) {
    return endSceneHr;
  }

  if (drawSlot->renderDoneQuery != nullptr) {
    hr = drawSlot->renderDoneQuery->Issue(D3DISSUE_END);
    if (FAILED(hr)) {
      return hr;
    }

    while (true) {
      hr = drawSlot->renderDoneQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH);
      if (hr == S_OK) {
        break;
      }
      if (hr == S_FALSE) {
        YieldProcessor();
        continue;
      }
      return hr;
    }
  }

  return S_OK;
}

HRESULT D3D9Renderer::SubmitLayeredCommandsAndPreparePresent(
    const LayeredFramePacket* framePacket) {
  if (framePacket == nullptr) {
    return E_INVALIDARG;
  }

  SurfaceSlot* drawSlot = nullptr;
  int drawSlotIndex = -1;
  HRESULT beginHr = BeginSubmitFrame(drawSlot, drawSlotIndex);
  if (FAILED(beginHr) || drawSlot == nullptr || drawSlotIndex < 0) {
    return beginHr;
  }

  for (int layerIndex = 0; layerIndex < LayeredFramePacket::kMaxLayerCount;
       ++layerIndex) {
    const auto& layer = framePacket->layers[layerIndex];
    if (layer.commandBytes <= 0 || layer.commandData == nullptr) {
      continue;
    }

    const HRESULT submitHr = SubmitCompiledBatches(drawSlot, layer);
    if (FAILED(submitHr)) {
      drawSlot->state = SurfaceState::Ready;
      return submitHr;
    }
  }

  DemoteReadyForPresentSlots(state_, drawSlotIndex);
  drawSlot->state = SurfaceState::ReadyForPresent;
  return S_OK;
}

HRESULT D3D9Renderer::SubmitLayeredCommands(const void* framePacket,
                                            int framePacketBytes) {
  if (!ValidateFramePacket(framePacket, framePacketBytes)) {
    return E_INVALIDARG;
  }

  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  RendererLockGuard lock(state_->csInitialized ? &state_->cs : nullptr);
  RegisterMetrics();

  const auto parseSubmitStart = std::chrono::steady_clock::now();
  const HRESULT submitHr = SubmitLayeredCommandsAndPreparePresent(
      static_cast<const LayeredFramePacket*>(framePacket));
  const auto parseSubmitEnd = std::chrono::steady_clock::now();

  if (parseSubmitDurationMetricId_ > 0) {
    const double parseSubmitDurationMs =
        std::chrono::duration<double, std::milli>(parseSubmitEnd -
                                                  parseSubmitStart)
            .count();
    FDVLOG_LogMetric(parseSubmitDurationMetricId_, parseSubmitDurationMs);
  }

  return submitHr;
}

HRESULT D3D9Renderer::TryAcquirePresentSurface(void** outSurface9) {
  if (outSurface9 == nullptr) {
    return E_POINTER;
  }

  *outSurface9 = nullptr;
  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  RendererLockGuard lock(state_->csInitialized ? &state_->cs : nullptr);
  if (state_->device == nullptr || state_->presentingSurface == nullptr) {
    return S_FALSE;
  }

  *outSurface9 = state_->presentingSurface.Get();
  return S_OK;
}

HRESULT D3D9Renderer::CopyReadyToPresentSurface() {
  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  RendererLockGuard lock(state_->csInitialized ? &state_->cs : nullptr);
  if (state_->device == nullptr) {
    return E_UNEXPECTED;
  }

  const int readySlotIndex = FindSlotByState(state_, SurfaceState::ReadyForPresent);
  if (readySlotIndex < 0 || state_->presentingSurface == nullptr ||
      state_->slots[readySlotIndex].renderTarget == nullptr) {
    return S_FALSE;
  }

  const HRESULT hr = state_->device->StretchRect(
      state_->slots[readySlotIndex].renderTarget.Get(), nullptr,
      state_->presentingSurface.Get(), nullptr, D3DTEXF_NONE);
  if (FAILED(hr)) {
    return hr;
  }

  state_->slots[readySlotIndex].state = SurfaceState::Ready;
  return S_OK;
}

void D3D9Renderer::OnFrontBufferAvailable(bool available) {
  if (state_ == nullptr) {
    return;
  }

  RendererLockGuard lock(state_->csInitialized ? &state_->cs : nullptr);
  state_->frontBufferAvailable = available;

  if (!available) {
    for (int i = 0; i < kFrameCount; ++i) {
      state_->slots[i].state = SurfaceState::Ready;
    }
    return;
  }

  if (state_->device != nullptr) {
    const HRESULT hr = CheckDeviceReady(state_);
    if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET ||
        hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED) {
      ResetDeviceAndSurface(state_);
    }
  }
}

void D3D9Renderer::RegisterMetrics() {
  if (parseSubmitDurationMetricId_ > 0) {
    return;
  }

  wchar_t metricName[104] = {};
  swprintf_s(metricName, L"native.d3d9.r%p.parse_submit_ms",
             static_cast<void*>(this));
  MetricSpec spec{};
  spec.name = metricName;
  spec.periodSec = kMetricWindowSec;
  spec.format = kParseSubmitMetricFormat;
  spec.level = FDVLOG_LevelInfo;
  parseSubmitDurationMetricId_ = FDVLOG_RegisterMetric(&spec);
}

void D3D9Renderer::UnregisterMetrics() {
  if (parseSubmitDurationMetricId_ <= 0) {
    return;
  }

  FDVLOG_UnregisterMetric(parseSubmitDurationMetricId_);
  parseSubmitDurationMetricId_ = 0;
}

}  // namespace fdv::d3d9
