#include "D3D9Renderer.h"

#include "D3D9RendererTypes.h"
#include "D3DBatchDraw.h"
#include "../FastDrawingVisual.LogCore/FdvLogCoreExports.h"

#include <chrono>
#include <cwchar>
#include <new>
#include <stdio.h>
#include <string.h>

namespace fdv::d3d9 {

namespace {

constexpr uint32_t kMetricWindowSec = 1;
constexpr const char* kVertexShaderSrc = R"(
struct VSInput
{
    float3 pos : POSITION0;
    float4 color : COLOR0;
};

struct PSInput
{
    float4 pos : POSITION0;
    float4 color : COLOR0;
};

PSInput main(VSInput input)
{
    PSInput output;
    output.pos = float4(input.pos, 1.0f);
    output.color = input.color;
    return output;
}
)";
constexpr const char* kPixelShaderSrc = R"(
struct PSInput
{
    float4 color : COLOR0;
};

float4 main(PSInput input) : COLOR0
{
    return input.color;
}
)";

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

void ReleaseShaderBuffer(ID3DXBuffer*& buffer) {
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

  if (state->pixelShader != nullptr) {
    state->pixelShader->Release();
    state->pixelShader = nullptr;
  }

  if (state->vertexShader != nullptr) {
    state->vertexShader->Release();
    state->vertexShader = nullptr;
  }

  if (state->vertexDeclaration != nullptr) {
    state->vertexDeclaration->Release();
    state->vertexDeclaration = nullptr;
  }
}

bool CompileShaderSource(const char* source, const char* profile,
                         ID3DXBuffer** outBytecode) {
  if (source == nullptr || profile == nullptr || outBytecode == nullptr) {
    return false;
  }

  *outBytecode = nullptr;

  ID3DXBuffer* errors = nullptr;
  ID3DXConstantTable* constants = nullptr;
  const HRESULT hr = D3DXCompileShader(
      source, static_cast<UINT>(strlen(source)), nullptr, nullptr, "main",
      profile, 0, outBytecode, &errors, &constants);
  LogShaderCompileError(errors);
  if (constants != nullptr) {
    constants->Release();
  }
  ReleaseShaderBuffer(errors);

  if (FAILED(hr) || *outBytecode == nullptr) {
    ReleaseShaderBuffer(*outBytecode);
    return false;
  }

  return true;
}

bool CreateDrawPipeline(D3D9RendererState* state) {
  if (state == nullptr || state->device == nullptr) {
    return false;
  }

  ReleaseDrawPipeline(state);

  ID3DXBuffer* vertexBytecode = nullptr;
  ID3DXBuffer* pixelBytecode = nullptr;
  if (!CompileShaderSource(kVertexShaderSrc, "vs_2_0", &vertexBytecode) ||
      !CompileShaderSource(kPixelShaderSrc, "ps_2_0", &pixelBytecode)) {
    ReleaseShaderBuffer(vertexBytecode);
    ReleaseShaderBuffer(pixelBytecode);
    ReleaseDrawPipeline(state);
    return false;
  }

  static const D3DVERTEXELEMENT9 kElements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
       D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,
       0},
      D3DDECL_END()};

  HRESULT hr = state->device->CreateVertexDeclaration(
      kElements, &state->vertexDeclaration);
  if (FAILED(hr) || state->vertexDeclaration == nullptr) {
    ReleaseShaderBuffer(vertexBytecode);
    ReleaseShaderBuffer(pixelBytecode);
    ReleaseDrawPipeline(state);
    return false;
  }

  hr = state->device->CreateVertexShader(
      static_cast<const DWORD*>(vertexBytecode->GetBufferPointer()),
      &state->vertexShader);
  if (FAILED(hr) || state->vertexShader == nullptr) {
    ReleaseShaderBuffer(vertexBytecode);
    ReleaseShaderBuffer(pixelBytecode);
    ReleaseDrawPipeline(state);
    return false;
  }

  hr = state->device->CreatePixelShader(
      static_cast<const DWORD*>(pixelBytecode->GetBufferPointer()),
      &state->pixelShader);
  ReleaseShaderBuffer(vertexBytecode);
  ReleaseShaderBuffer(pixelBytecode);

  if (FAILED(hr) || state->pixelShader == nullptr) {
    ReleaseDrawPipeline(state);
    return false;
  }

  return true;
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

bool CreateDeviceAndSurface(D3D9RendererState* state) {
  if (state == nullptr || state->hwnd == nullptr || state->width <= 0 ||
      state->height <= 0) {
    return false;
  }

  if (state->d3d9 == nullptr) {
    state->d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (state->d3d9 == nullptr) {
      return false;
    }
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

  HRESULT hr = state->d3d9->CreateDevice(
      D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, state->hwnd,
      D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED |
          D3DCREATE_FPU_PRESERVE,
      &parameters, &state->device);
  if (FAILED(hr)) {
    hr = state->d3d9->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, state->hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED |
            D3DCREATE_FPU_PRESERVE,
        &parameters, &state->device);
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

  const HRESULT hr = state->device->Reset(&parameters);
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

  HRESULT hr = state_->device->TestCooperativeLevel();
  if (hr == D3DERR_DEVICENOTRESET) {
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
  while (state_->batchCompiler.TryGetNextBatch(batch, batchHr)) {
    switch (batch.kind) {
    case batch::BatchKind::Clear:
      device->Clear(0, nullptr, D3DCLEAR_TARGET,
                    ToD3DClearColor(batch.clearColor), 1.0f, 0);
      break;

    case batch::BatchKind::Triangles: {
      draw::TriangleBatchDrawContext triangleContext{};
      triangleContext.device = device;
      triangleContext.vertexDeclaration = state_->vertexDeclaration;
      triangleContext.vertexShader = state_->vertexShader;
      triangleContext.pixelShader = state_->pixelShader;
      const draw::TriangleVertexData vertexData{batch.triangleVertices,
                                                batch.triangleVertexCount};
      submitHr = draw::DrawTriangleBatch(triangleContext, vertexData);
      break;
    }

    case batch::BatchKind::ShapeInstances: {
      draw::InstanceBatchDrawContext instanceContext{};
      instanceContext.device = device;
      instanceContext.vertexDeclaration = state_->vertexDeclaration;
      instanceContext.vertexShader = state_->vertexShader;
      instanceContext.pixelShader = state_->pixelShader;
      instanceContext.viewportWidth = state_->width;
      instanceContext.viewportHeight = state_->height;
      const draw::ShapeInstanceData instanceData{batch.shapeInstances,
                                                 batch.shapeInstanceCount};
      submitHr = draw::DrawShapeBatch(instanceContext, instanceData);
      break;
    }

    case batch::BatchKind::Text: {
      const draw::TextBatchDrawContext textContext{};
      const draw::DrawTextData textData{batch.textItems, batch.textItemCount};
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
    const HRESULT hr = state_->device->TestCooperativeLevel();
    if (hr == D3DERR_DEVICENOTRESET) {
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
