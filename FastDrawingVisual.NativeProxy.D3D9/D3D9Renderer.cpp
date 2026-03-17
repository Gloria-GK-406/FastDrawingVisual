#include "D3D9Renderer.h"

#include "D3D9RendererTypes.h"
#include "D3DBatchDraw.h"
#include "../FastDrawingVisual.LogCore/FdvLogCoreExports.h"

#include <chrono>
#include <cstdint>
#include <cwchar>
#include <new>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace fdv::d3d9 {

namespace {

constexpr uint32_t kMetricWindowSec = 1;
constexpr const wchar_t* kInstanceVertexShaderPath = L"Shader\\InstanceVS_Model3.cso";
constexpr const wchar_t* kInstancePixelShaderPath = L"Shader\\InstancePS_Model3.cso";

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

void ReleaseVertexDeclaration(IDirect3DVertexDeclaration9*& declaration) {
  if (declaration != nullptr) {
    declaration->Release();
    declaration = nullptr;
  }
}

void ReleaseVertexShader(IDirect3DVertexShader9*& shader) {
  if (shader != nullptr) {
    shader->Release();
    shader = nullptr;
  }
}

void ReleasePixelShader(IDirect3DPixelShader9*& shader) {
  if (shader != nullptr) {
    shader->Release();
    shader = nullptr;
  }
}

void ReleaseVertexBuffer(IDirect3DVertexBuffer9*& buffer) {
  if (buffer != nullptr) {
    buffer->Release();
    buffer = nullptr;
  }
}

void ReleaseIndexBuffer(IDirect3DIndexBuffer9*& buffer) {
  if (buffer != nullptr) {
    buffer->Release();
    buffer = nullptr;
  }
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
      &state->unitQuadVertexBuffer, nullptr);
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
      D3DFMT_INDEX16, D3DPOOL_DEFAULT, &state->unitQuadIndexBuffer, nullptr);
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
      kInstanceElements, &state->instanceVertexDeclaration);
  if (FAILED(hr) || state->instanceVertexDeclaration == nullptr) {
    ReleaseDrawPipeline(state);
    return false;
  }

  hr = state->device->CreateVertexShader(
      reinterpret_cast<const DWORD*>(instanceVertexBytecode.data()),
      &state->instanceVertexShader);
  if (FAILED(hr) || state->instanceVertexShader == nullptr) {
    ReleaseDrawPipeline(state);
    return false;
  }

  hr = state->device->CreatePixelShader(
      reinterpret_cast<const DWORD*>(instancePixelBytecode.data()),
      &state->instancePixelShader);
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

  if (state->presentingSurface != nullptr) {
    state->presentingSurface->Release();
    state->presentingSurface = nullptr;
  }

  for (int i = 0; i < kFrameCount; ++i) {
    if (state->slots[i].renderDoneQuery != nullptr) {
      state->slots[i].renderDoneQuery->Release();
      state->slots[i].renderDoneQuery = nullptr;
    }

    if (state->slots[i].renderTarget != nullptr) {
      state->slots[i].renderTarget->Release();
      state->slots[i].renderTarget = nullptr;
    }

    state->slots[i].state = SurfaceState::Ready;
  }
}

bool CreateFrameResources(D3D9RendererState* state) {
  if (state == nullptr || state->device == nullptr || state->width <= 0 ||
      state->height <= 0) {
    return false;
  }

  ReleaseFrameResources(state);

  for (int i = 0; i < kFrameCount; ++i) {
    HRESULT hr = state->device->CreateRenderTarget(
        static_cast<UINT>(state->width), static_cast<UINT>(state->height),
        D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
        &state->slots[i].renderTarget, nullptr);
    if (FAILED(hr)) {
      ReleaseFrameResources(state);
      return false;
    }

    state->device->CreateQuery(D3DQUERYTYPE_EVENT,
                               &state->slots[i].renderDoneQuery);
    state->slots[i].state = SurfaceState::Ready;
  }

  HRESULT hr = state->device->CreateRenderTarget(
      static_cast<UINT>(state->width), static_cast<UINT>(state->height),
      D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
      &state->presentingSurface, nullptr);
  if (FAILED(hr)) {
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
    IDirect3D9Ex* d3d9 = nullptr;
    if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9)) || d3d9 == nullptr) {
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
      &parameters, nullptr, &state->device);
  if (FAILED(hr)) {
    hr = state->d3d9->CreateDeviceEx(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, state->hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED |
            D3DCREATE_FPU_PRESERVE,
        &parameters, nullptr, &state->device);
  }

  if (FAILED(hr) || state->device == nullptr) {
    return false;
  }

  if (!CreateDrawPipeline(state)) {
    state->device->Release();
    state->device = nullptr;
    return false;
  }

  if (!CreateFrameResources(state)) {
    ReleaseDrawPipeline(state);
    state->device->Release();
    state->device = nullptr;
    return false;
  }

  return true;
}

void ReleaseDeviceResources(D3D9RendererState* state) {
  if (state == nullptr) {
    return;
  }

  ReleaseFrameResources(state);
  ReleaseDrawPipeline(state);

  if (state->device != nullptr) {
    state->device->Release();
    state->device = nullptr;
  }
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
  if (state_->d3d9 != nullptr) {
    state_->d3d9->Release();
    state_->d3d9 = nullptr;
  }

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

  IDirect3DDevice9* device = state_->device;
  HRESULT hr = device->SetRenderTarget(0, drawSlot->renderTarget);
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

  hr = device->BeginScene();
  if (FAILED(hr)) {
    return hr;
  }

  draw::SetupRenderState(device);
  state_->batchCompiler.Reset(state_->width, state_->height, layer.commandData,
                              layer.commandBytes, layer.blobData,
                              layer.blobBytes);

  HRESULT submitHr = S_OK;
  batch::CompiledBatchView batch{};
  HRESULT batchHr = S_OK;
  while ((batchHr = state_->batchCompiler.TryGetNextBatch(batch)) == S_OK) {
    switch (batch.kind) {
    case batch::BatchKind::Clear:
      device->Clear(0, nullptr, D3DCLEAR_TARGET,
                    ToD3DClearColor(batch.clearColor), 1.0f, 0);
      break;

    case batch::BatchKind::Triangles: {
      submitHr = E_NOTIMPL;
      break;
    }

    case batch::BatchKind::ShapeInstances: {
      const auto& shapeInstances = state_->batchCompiler.GetShapeInstances();
      draw::InstanceBatchDrawContext instanceContext{};
      instanceContext.device = device;
      instanceContext.vertexDeclaration = state_->instanceVertexDeclaration;
      instanceContext.vertexShader = state_->instanceVertexShader;
      instanceContext.pixelShader = state_->instancePixelShader;
      instanceContext.geometryVertexBuffer = state_->unitQuadVertexBuffer;
      instanceContext.geometryIndexBuffer = state_->unitQuadIndexBuffer;
      instanceContext.geometryVertexStrideBytes = sizeof(float) * 2;
      instanceContext.geometryVertexCount = 4;
      instanceContext.geometryPrimitiveCount = 2;
      instanceContext.instanceBuffer = state_->dynamicInstanceVertexBuffer;
      instanceContext.instanceBufferCapacityBytes =
          state_->dynamicInstanceVertexCapacityBytes;
      instanceContext.viewportWidth = static_cast<float>(state_->width);
      instanceContext.viewportHeight = static_cast<float>(state_->height);
      const draw::ShapeInstanceData instanceData{
          shapeInstances.data(), static_cast<int>(shapeInstances.size())};
      submitHr = draw::DrawShapeInstanceBatch(instanceContext, instanceData);
      state_->dynamicInstanceVertexBuffer = instanceContext.instanceBuffer;
      state_->dynamicInstanceVertexCapacityBytes =
          instanceContext.instanceBufferCapacityBytes;
      break;
    }

    case batch::BatchKind::Text: {
      const auto& textItems = state_->batchCompiler.GetTextItems();
      const draw::TextBatchDrawContext textContext{};
      const draw::DrawTextData textData{textItems.data(),
                                        static_cast<int>(textItems.size())};
      submitHr = draw::DrawTextBatch(textContext, textData);
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

  const HRESULT endSceneHr = device->EndScene();
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

  *outSurface9 = state_->presentingSurface;
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
      state_->slots[readySlotIndex].renderTarget, nullptr,
      state_->presentingSurface, nullptr, D3DTEXF_NONE);
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
  FDVLOG_MetricSpec spec{};
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
