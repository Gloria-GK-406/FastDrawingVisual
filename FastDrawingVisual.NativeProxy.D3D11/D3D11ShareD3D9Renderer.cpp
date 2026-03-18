#include "D3D11ShareD3D9Renderer.h"
#include "../FastDrawingVisual.LogCore/FdvLogCoreExports.h"
#include "../FastDrawingVisual.NativeProxy.TextD2D/D2DTextRenderer.h"
#include "BatchComplier.h"
#include "D3DBatchDraw.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <d3d11.h>
#include <dxgi.h>
#include <memory>
#include <new>
#include <vector>
#include <wrl/client.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace fdv::d3d11 {
using Microsoft::WRL::ComPtr;

constexpr int kFrameCount = 2;

enum class SurfaceState : uint8_t {
  Ready = 0,
  Drawing = 1,
  ReadyForPresent = 2,
};

struct SurfaceSlot {
  ComPtr<ID3D11Texture2D> workTexture = nullptr;
  ComPtr<ID3D11RenderTargetView> workRtv = nullptr;
  ComPtr<ID3D11Query> renderDoneQuery = nullptr;
  ComPtr<ID3D11Texture2D> sharedTexture11 = nullptr;
  ComPtr<ID3D11Query> copyDoneQuery = nullptr;
  ComPtr<IDirect3DTexture9> texture9 = nullptr;
  ComPtr<IDirect3DSurface9> surface9 = nullptr;
  ComPtr<IDirect3DQuery9> presentDoneQuery = nullptr;
  HANDLE sharedHandle = nullptr;
  SurfaceState state = SurfaceState::Ready;
};

struct D3D11ShareD3D9RendererState {
  ComPtr<ID3D11Device> device = nullptr;
  ComPtr<ID3D11DeviceContext> context = nullptr;
  ComPtr<IDirect3D9Ex> d3d9 = nullptr;
  ComPtr<IDirect3DDevice9Ex> d3d9Device = nullptr;
  ComPtr<IDirect3DSurface9> presentingSurface = nullptr;
  ComPtr<ID3D11VertexShader> instanceVertexShader = nullptr;
  ComPtr<ID3D11PixelShader> instancePixelShader = nullptr;
  ComPtr<ID3D11InputLayout> instanceInputLayout = nullptr;
  ComPtr<ID3D11BlendState> blendState = nullptr;
  ComPtr<ID3D11RasterizerState> rasterizerState = nullptr;
  ComPtr<ID3D11Buffer> unitQuadVertexBuffer = nullptr;
  ComPtr<ID3D11Buffer> dynamicInstanceBuffer = nullptr;
  UINT dynamicInstanceCapacityBytes = 0;
  ComPtr<ID3D11Buffer> viewConstantsBuffer = nullptr;
  std::unique_ptr<fdv::textd2d::D2DTextRenderer> textRenderer;
  batch::BatchCompiler batchCompiler;
  SurfaceSlot slots[kFrameCount];
  int activeTextTargetSlotIndex = -1;
  HWND hwnd = nullptr;
  bool frontBufferAvailable = true;
};

struct SubmitFrameDiagnostics {
  int layerCount = 0;
  int commandCount = 0;
  int clearCommandCount = 0;
  int fillRectCommandCount = 0;
  int strokeRectCommandCount = 0;
  int fillEllipseCommandCount = 0;
  int strokeEllipseCommandCount = 0;
  int lineCommandCount = 0;
  int textRunCommandCount = 0;
  int clearBatchCount = 0;
  int triangleBatchCount = 0;
  int shapeBatchCount = 0;
  int textBatchCount = 0;
  int triangleVertexCount = 0;
  int shapeInstanceCount = 0;
  int maxTriangleBatchVertexCount = 0;
  int textItemCount = 0;
  int maxTextBatchItemCount = 0;
  int textCharCount = 0;
  int vertexBufferResizeCount = 0;
  UINT vertexBytesUploaded = 0;
  UINT maxVertexBufferCapacityBytes = 0;
  double beginFrameMs = 0.0;
  double compileMs = 0.0;
  double commandReadMs = 0.0;
  double commandBuildMs = 0.0;
  double triangleCpuMs = 0.0;
  double triangleEnsureVertexBufferMs = 0.0;
  double triangleUploadMs = 0.0;
  double triangleDrawCallMs = 0.0;
  double textDrawMs = 0.0;
  double textFlushMs = 0.0;
  double textRecordMs = 0.0;
  double textEndDrawMs = 0.0;
  double presentMs = 0.0;
};

namespace {
constexpr const wchar_t* kLogCategory = L"NativeProxy.D3D11ShareD3D9";
constexpr double kSlowFrameThresholdMs = 33.0;
constexpr std::uint64_t kSlowFrameLogEveryNFrames = 120;
constexpr std::uint64_t kDetailedFrameLogEveryNFrames = 30;
constexpr uint32_t kMetricWindowSec = 1;
constexpr const wchar_t* kDrawMetricFormat =
    L"name={name} periodSec={periodSec}s samples={count} avgMs={avg} minMs={min} maxMs={max}";
constexpr const wchar_t* kFpsMetricFormat =
    L"name={name} periodSec={periodSec}s samples={count} avgFps={avg} minFps={min} maxFps={max}";
constexpr UINT kCreationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
constexpr DXGI_FORMAT kSharedTextureFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr const wchar_t* kInstanceVertexShaderObject = L"Shader\\InstanceVS_Model4.cso";
constexpr const wchar_t* kInstancePixelShaderObject = L"Shader\\InstancePS_Model4.cso";
constexpr int kEllipseSegmentCount = 48;

std::uint64_t QueryQpcNow() {
  LARGE_INTEGER value{};
  QueryPerformanceCounter(&value);
  return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t QueryQpcFrequency() {
  static const std::uint64_t cached = []() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
  }();
  return cached;
}

HRESULT LoadShaderBlob(const wchar_t* filePath, ComPtr<ID3DBlob>& blobOut) {
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

HRESULT EnsureDynamicBuffer(ID3D11Device* device, UINT byteWidth, UINT bindFlags,
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

  const HRESULT hr =
      device->CreateBuffer(&bufferDesc, nullptr, bufferOut.ReleaseAndGetAddressOf());
  if (FAILED(hr) || bufferOut == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  return S_OK;
}

HRESULT EnsureUnitQuadVertexBuffer(ID3D11Device* device,
                                   ComPtr<ID3D11Buffer>& bufferOut) {
  if (device == nullptr) {
    return E_INVALIDARG;
  }

  if (bufferOut != nullptr) {
    return S_OK;
  }

  constexpr float kUnitQuadVertices[8] = {
      -1.0f, -1.0f,
       1.0f, -1.0f,
      -1.0f,  1.0f,
       1.0f,  1.0f,
  };

  D3D11_BUFFER_DESC bufferDesc = {};
  bufferDesc.ByteWidth = static_cast<UINT>(sizeof(kUnitQuadVertices));
  bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
  bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

  D3D11_SUBRESOURCE_DATA initialData = {};
  initialData.pSysMem = kUnitQuadVertices;

  const HRESULT hr =
      device->CreateBuffer(&bufferDesc, &initialData,
                           bufferOut.ReleaseAndGetAddressOf());
  if (FAILED(hr) || bufferOut == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  return S_OK;
}

HRESULT EnsureEllipseTemplateVertexBuffer(
    ID3D11Device* device, bool stroke, ComPtr<ID3D11Buffer>& bufferOut,
    UINT& vertexCountOut) {
  if (device == nullptr) {
    return E_INVALIDARG;
  }

  if (bufferOut != nullptr && vertexCountOut > 0) {
    return S_OK;
  }

  struct EllipseTemplateVertex {
    float x;
    float y;
    float blend;
  };

  std::vector<EllipseTemplateVertex> vertices;
  if (!stroke) {
    vertices.reserve(static_cast<std::size_t>(kEllipseSegmentCount) * 3u);
    for (int i = 0; i < kEllipseSegmentCount; ++i) {
      const float a0 = (2.0f * 3.14159265358979323846f * static_cast<float>(i)) /
                       static_cast<float>(kEllipseSegmentCount);
      const float a1 =
          (2.0f * 3.14159265358979323846f * static_cast<float>(i + 1)) /
          static_cast<float>(kEllipseSegmentCount);
      vertices.push_back({0.0f, 0.0f, 0.0f});
      vertices.push_back({static_cast<float>(std::cos(a0)),
                          static_cast<float>(std::sin(a0)), 1.0f});
      vertices.push_back({static_cast<float>(std::cos(a1)),
                          static_cast<float>(std::sin(a1)), 1.0f});
    }
  } else {
    vertices.reserve(static_cast<std::size_t>(kEllipseSegmentCount) * 6u);
    for (int i = 0; i < kEllipseSegmentCount; ++i) {
      const float a0 = (2.0f * 3.14159265358979323846f * static_cast<float>(i)) /
                       static_cast<float>(kEllipseSegmentCount);
      const float a1 =
          (2.0f * 3.14159265358979323846f * static_cast<float>(i + 1)) /
          static_cast<float>(kEllipseSegmentCount);
      const float c0 = static_cast<float>(std::cos(a0));
      const float s0 = static_cast<float>(std::sin(a0));
      const float c1 = static_cast<float>(std::cos(a1));
      const float s1 = static_cast<float>(std::sin(a1));
      vertices.push_back({c0, s0, 1.0f});
      vertices.push_back({c1, s1, 1.0f});
      vertices.push_back({c0, s0, 0.0f});
      vertices.push_back({c1, s1, 1.0f});
      vertices.push_back({c1, s1, 0.0f});
      vertices.push_back({c0, s0, 0.0f});
    }
  }

  D3D11_BUFFER_DESC bufferDesc = {};
  bufferDesc.ByteWidth =
      static_cast<UINT>(vertices.size() * sizeof(EllipseTemplateVertex));
  bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
  bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

  D3D11_SUBRESOURCE_DATA initialData = {};
  initialData.pSysMem = vertices.data();

  const HRESULT hr =
      device->CreateBuffer(&bufferDesc, &initialData,
                           bufferOut.ReleaseAndGetAddressOf());
  if (FAILED(hr) || bufferOut == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  vertexCountOut = static_cast<UINT>(vertices.size());
  return S_OK;
}

double DurationMs(const std::chrono::steady_clock::time_point& start,
                  const std::chrono::steady_clock::time_point& end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

void RegisterDurationMetric(int& metricId, const void* renderer,
                            const wchar_t* suffix) {
  if (metricId > 0) {
    return;
  }

  wchar_t metricName[112]{};
  swprintf_s(metricName, L"native.d3d11.r%p.%ls", renderer, suffix);
  FDVLOG_MetricSpec spec{};
  spec.name = metricName;
  spec.periodSec = kMetricWindowSec;
  spec.format = kDrawMetricFormat;
  spec.level = FDVLOG_LevelInfo;
  metricId = FDVLOG_RegisterMetric(&spec);
}

void UnregisterMetric(int& metricId) {
  if (metricId > 0) {
    FDVLOG_UnregisterMetric(metricId);
    metricId = 0;
  }
}

void AccumulateCompileStats(SubmitFrameDiagnostics& diagnostics,
                            const batch::BatchCompileStats& stats) {
  diagnostics.commandCount += stats.commandCount;
  diagnostics.clearCommandCount += stats.commands.clearCount;
  diagnostics.fillRectCommandCount += stats.commands.fillRectCount;
  diagnostics.strokeRectCommandCount += stats.commands.strokeRectCount;
  diagnostics.fillEllipseCommandCount += stats.commands.fillEllipseCount;
  diagnostics.strokeEllipseCommandCount += stats.commands.strokeEllipseCount;
  diagnostics.lineCommandCount += stats.commands.lineCount;
  diagnostics.textRunCommandCount += stats.commands.drawTextRunCount;
  diagnostics.triangleVertexCount += stats.triangleVertexCount;
  diagnostics.shapeInstanceCount += stats.shapeInstanceCount;
  diagnostics.textItemCount += stats.textItemCount;
  diagnostics.textCharCount += stats.textCharCount;
  diagnostics.commandReadMs += stats.commandReadMs;
  diagnostics.commandBuildMs += stats.commandBuildMs;
}

void LogFrameBreakdown(const void* renderer, std::uint64_t frameId, int width,
                       int height, double drawDurationMs,
                       const SubmitFrameDiagnostics& diagnostics, int level) {
  wchar_t message[1536]{};
  swprintf_s(
      message,
      L"frame breakdown renderer=0x%p frame=%llu size=%dx%d layers=%d cmds=%d "
      L"cmds(clear=%d fillRect=%d strokeRect=%d fillEllipse=%d strokeEllipse=%d line=%d textRun=%d) "
      L"batches(clear=%d tri=%d shape=%d text=%d) triVerts=%d shapeInstances=%d maxTriBatchVerts=%d textItems=%d maxTextBatchItems=%d textChars=%d "
      L"beginMs=%.3f compileMs=%.3f readMs=%.3f buildMs=%.3f triCpuMs=%.3f triEnsureMs=%.3f triUploadMs=%.3f triDrawMs=%.3f "
      L"textMs=%.3f textFlushMs=%.3f textRecordMs=%.3f textEndMs=%.3f presentMs=%.3f vbResizes=%d uploadedBytes=%u maxVbBytes=%u totalMs=%.3f.",
      renderer, static_cast<unsigned long long>(frameId), width, height,
      diagnostics.layerCount, diagnostics.commandCount,
      diagnostics.clearCommandCount, diagnostics.fillRectCommandCount,
      diagnostics.strokeRectCommandCount,
      diagnostics.fillEllipseCommandCount,
      diagnostics.strokeEllipseCommandCount, diagnostics.lineCommandCount,
      diagnostics.textRunCommandCount, diagnostics.clearBatchCount,
      diagnostics.triangleBatchCount, diagnostics.shapeBatchCount,
      diagnostics.textBatchCount, diagnostics.triangleVertexCount,
      diagnostics.shapeInstanceCount,
      diagnostics.maxTriangleBatchVertexCount, diagnostics.textItemCount,
      diagnostics.maxTextBatchItemCount, diagnostics.textCharCount,
      diagnostics.beginFrameMs,
      diagnostics.compileMs, diagnostics.commandReadMs,
      diagnostics.commandBuildMs, diagnostics.triangleCpuMs,
      diagnostics.triangleEnsureVertexBufferMs,
      diagnostics.triangleUploadMs, diagnostics.triangleDrawCallMs,
      diagnostics.textDrawMs, diagnostics.textFlushMs,
      diagnostics.textRecordMs, diagnostics.textEndDrawMs,
      diagnostics.presentMs, diagnostics.vertexBufferResizeCount,
      diagnostics.vertexBytesUploaded, diagnostics.maxVertexBufferCapacityBytes,
      drawDurationMs);
  FDVLOG_Log(level, kLogCategory, message, false);
  if (level >= FDVLOG_LevelWarn) {
    FDVLOG_WriteETW(level, kLogCategory, message, false);
  }
}

void LogStageFailure(const void* renderer, std::uint64_t frameId,
                     const wchar_t* stage, HRESULT hr,
                     const SubmitFrameDiagnostics& diagnostics, int width,
                     int height) {
  wchar_t message[768]{};
  swprintf_s(
      message,
      L"submit failed renderer=0x%p frame=%llu stage=%ls hr=0x%08X size=%dx%d "
      L"layers=%d cmds=%d shapeInstances=%d textItems=%d compileMs=%.3f triCpuMs=%.3f textMs=%.3f presentMs=%.3f.",
      renderer, static_cast<unsigned long long>(frameId), stage,
      static_cast<unsigned int>(hr), width, height, diagnostics.layerCount,
      diagnostics.commandCount, diagnostics.shapeInstanceCount,
      diagnostics.textItemCount, diagnostics.compileMs,
      diagnostics.triangleCpuMs, diagnostics.textDrawMs,
      diagnostics.presentMs);
  FDVLOG_Log(FDVLOG_LevelError, kLogCategory, message, false);
  FDVLOG_WriteETW(FDVLOG_LevelError, kLogCategory, message, false);
}

int FindSlotByState(const D3D11ShareD3D9RendererState* state,
                    SurfaceState slotState) {
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

void DemoteReadyForPresentSlots(D3D11ShareD3D9RendererState* state,
                                int keepIndex) {
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

HRESULT WaitForD3D11Query(ID3D11DeviceContext* context, ID3D11Query* query) {
  if (query == nullptr) {
    return S_OK;
  }

  if (context == nullptr) {
    return E_POINTER;
  }

  while (true) {
    const HRESULT hr = context->GetData(query, nullptr, 0, 0);
    if (hr == S_OK) {
      return S_OK;
    }
    if (hr == S_FALSE) {
      YieldProcessor();
      continue;
    }
    return hr;
  }
}

HRESULT WaitForD3D9Query(IDirect3DQuery9* query) {
  if (query == nullptr) {
    return S_OK;
  }

  while (true) {
    const HRESULT hr = query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    if (hr == S_OK) {
      return S_OK;
    }
    if (hr == S_FALSE) {
      YieldProcessor();
      continue;
    }
    return hr;
  }
}

void ReleaseFrameResources(D3D11ShareD3D9RendererState* state) {
  if (state == nullptr) {
    return;
  }

  if (state->textRenderer != nullptr) {
    state->textRenderer->ReleaseRenderTargetResources();
  }

  state->activeTextTargetSlotIndex = -1;
  state->presentingSurface.Reset();

  for (int index = 0; index < kFrameCount; ++index) {
    auto& slot = state->slots[index];
    slot.presentDoneQuery.Reset();
    slot.surface9.Reset();
    slot.texture9.Reset();
    slot.copyDoneQuery.Reset();
    slot.sharedTexture11.Reset();
    slot.renderDoneQuery.Reset();
    slot.workRtv.Reset();
    slot.workTexture.Reset();
    slot.sharedHandle = nullptr;
    slot.state = SurfaceState::Ready;
  }
}
} // namespace

void D3D11ShareD3D9Renderer::RegisterMetrics() {
  RegisterDurationMetric(parseSubmitDurationMetricId_, static_cast<void*>(this),
                         L"parse_submit_ms");
  RegisterDurationMetric(drawDurationMetricId_, static_cast<void*>(this),
                         L"draw_ms");
  RegisterDurationMetric(compileDurationMetricId_, static_cast<void*>(this),
                         L"compile_ms");
  RegisterDurationMetric(commandReadDurationMetricId_,
                         static_cast<void*>(this), L"command_read_ms");
  RegisterDurationMetric(commandBuildDurationMetricId_,
                         static_cast<void*>(this), L"command_build_ms");
  RegisterDurationMetric(triangleCpuDurationMetricId_,
                         static_cast<void*>(this), L"triangle_cpu_ms");
  RegisterDurationMetric(triangleUploadDurationMetricId_,
                         static_cast<void*>(this), L"triangle_upload_ms");
  RegisterDurationMetric(triangleDrawCallDurationMetricId_,
                         static_cast<void*>(this), L"triangle_drawcall_ms");
  RegisterDurationMetric(textDurationMetricId_, static_cast<void*>(this),
                         L"text_draw_ms");
  RegisterDurationMetric(presentDurationMetricId_, static_cast<void*>(this),
                         L"present_ms");

  if (fpsMetricId_ <= 0) {
    wchar_t metricName[96]{};
    swprintf_s(metricName, L"native.d3d11.r%p.fps", static_cast<void*>(this));
    FDVLOG_MetricSpec spec{};
    spec.name = metricName;
    spec.periodSec = kMetricWindowSec;
    spec.format = kFpsMetricFormat;
    spec.level = FDVLOG_LevelInfo;
    fpsMetricId_ = FDVLOG_RegisterMetric(&spec);
  }
}

void D3D11ShareD3D9Renderer::UnregisterMetrics() {
  UnregisterMetric(parseSubmitDurationMetricId_);
  UnregisterMetric(drawDurationMetricId_);
  UnregisterMetric(fpsMetricId_);
  UnregisterMetric(compileDurationMetricId_);
  UnregisterMetric(commandReadDurationMetricId_);
  UnregisterMetric(commandBuildDurationMetricId_);
  UnregisterMetric(triangleCpuDurationMetricId_);
  UnregisterMetric(triangleUploadDurationMetricId_);
  UnregisterMetric(triangleDrawCallDurationMetricId_);
  UnregisterMetric(textDurationMetricId_);
  UnregisterMetric(presentDurationMetricId_);
}

HRESULT D3D11ShareD3D9Renderer::BeginSubmitFrame(void*& currentRtv,
                                                 int& drawSlotIndex) {
  drawSlotIndex = -1;
  if (state_ == nullptr || state_->context == nullptr || width_ <= 0 ||
      height_ <= 0 || !state_->frontBufferAvailable) {
    return E_UNEXPECTED;
  }

  const int readySlotIndex = FindSlotByState(state_, SurfaceState::Ready);
  if (readySlotIndex < 0) {
    return S_FALSE;
  }

  const HRESULT pipelineHr = CreateDrawPipeline();
  if (FAILED(pipelineHr)) {
    return pipelineHr;
  }

  drawSlotIndex = readySlotIndex;
  state_->slots[readySlotIndex].state = SurfaceState::Drawing;

  ID3D11RenderTargetView* nativeRtv = state_->slots[readySlotIndex].workRtv.Get();
  if (nativeRtv == nullptr) {
    state_->slots[readySlotIndex].state = SurfaceState::Ready;
    drawSlotIndex = -1;
    return E_UNEXPECTED;
  }
  currentRtv = nativeRtv;
  state_->context->OMSetRenderTargets(1, &nativeRtv, nullptr);

  D3D11_VIEWPORT viewport = {};
  viewport.Width = static_cast<float>(width_);
  viewport.Height = static_cast<float>(height_);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  state_->context->RSSetViewports(1, &viewport);
  return S_OK;
}

HRESULT D3D11ShareD3D9Renderer::SubmitCompiledBatches(
    const LayerPacket& layer, int layerIndex, int drawSlotIndex, void* currentRtv,
    SubmitFrameDiagnostics& diagnostics) {
  if (state_ == nullptr) {
    return E_UNEXPECTED;
  }

  if (layer.commandData == nullptr || layer.commandBytes <= 0) {
    return S_OK;
  }

  ++diagnostics.layerCount;
  auto* nativeRtv = static_cast<ID3D11RenderTargetView*>(currentRtv);
  state_->batchCompiler.Reset(width_, height_, layer.commandData,
                              layer.commandBytes, layer.blobData,
                              layer.blobBytes);

  batch::CompiledBatchView batch{};
  HRESULT batchHr = S_OK;
  int batchIndex = 0;
  while (true) {
    const auto compileStart = std::chrono::steady_clock::now();
    batchHr = state_->batchCompiler.TryGetNextBatch(batch);
    const auto compileEnd = std::chrono::steady_clock::now();
    diagnostics.compileMs += DurationMs(compileStart, compileEnd);
    AccumulateCompileStats(diagnostics, state_->batchCompiler.lastBatchStats());
    if (batchHr != S_OK) {
      break;
    }

    ++batchIndex;
    switch (batch.kind) {
    case batch::BatchKind::Clear:
      state_->context->ClearRenderTargetView(nativeRtv, batch.clearColor);
      break;

    case batch::BatchKind::Triangles: {
      return E_NOTIMPL;
    }

    case batch::BatchKind::ShapeInstances: {
      const auto& shapeInstances = state_->batchCompiler.GetShapeInstances();
      const int shapeInstanceCount = static_cast<int>(shapeInstances.size());
      draw::InstanceBatchDrawContext instanceContext{};
      instanceContext.context = state_->context;
      instanceContext.inputLayout = state_->instanceInputLayout;
      instanceContext.vertexShader = state_->instanceVertexShader;
      instanceContext.pixelShader = state_->instancePixelShader;
      instanceContext.blendState = state_->blendState;
      instanceContext.rasterizerState = state_->rasterizerState;
      instanceContext.geometryVertexBuffer = state_->unitQuadVertexBuffer;
      instanceContext.geometryVertexStrideBytes = sizeof(float) * 2;
      instanceContext.geometryVertexCount = 4;
      instanceContext.instanceBuffer = state_->dynamicInstanceBuffer;
      instanceContext.instanceBufferCapacityBytes =
          state_->dynamicInstanceCapacityBytes;
      instanceContext.viewConstantsBuffer = state_->viewConstantsBuffer;
      instanceContext.viewportWidth = static_cast<float>(width_);
      instanceContext.viewportHeight = static_cast<float>(height_);

      const draw::ShapeInstanceData instanceData{shapeInstances.data(),
                                                 shapeInstanceCount};
      draw::InstanceBatchDrawStats instanceStats{};
      const auto instanceStart = std::chrono::steady_clock::now();
      const HRESULT drawHr = draw::DrawShapeInstanceBatch(instanceContext,
                                                          instanceData,
                                                          &instanceStats);
      const auto instanceEnd = std::chrono::steady_clock::now();
      diagnostics.triangleCpuMs += DurationMs(instanceStart, instanceEnd);
      diagnostics.triangleUploadMs += instanceStats.uploadInstanceDataMs;
      diagnostics.triangleDrawCallMs += instanceStats.issueDrawMs;
      diagnostics.vertexBytesUploaded += instanceStats.uploadedBytes;
      diagnostics.maxVertexBufferCapacityBytes =
          (std::max)(diagnostics.maxVertexBufferCapacityBytes,
                     instanceStats.instanceBufferCapacityBytes);
      state_->dynamicInstanceBuffer = instanceContext.instanceBuffer;
      state_->dynamicInstanceCapacityBytes =
          instanceContext.instanceBufferCapacityBytes;
      if (FAILED(drawHr)) {
        return drawHr;
      }
      break;
    }

    case batch::BatchKind::Text: {
      const auto& textItems = state_->batchCompiler.GetTextItems();
      if (state_->textRenderer == nullptr ||
          state_->slots[drawSlotIndex].workTexture == nullptr) {
        return E_UNEXPECTED;
      }

      if (state_->activeTextTargetSlotIndex != drawSlotIndex) {
        const HRESULT targetHr = state_->textRenderer->CreateTargetFromTexture(
            state_->slots[drawSlotIndex].workTexture.Get(), kSharedTextureFormat);
        if (FAILED(targetHr)) {
          state_->activeTextTargetSlotIndex = -1;
          return targetHr;
        }
        state_->activeTextTargetSlotIndex = drawSlotIndex;
      }

      fdv::textd2d::TextBatchDrawStats textStats{};
      const auto textStart = std::chrono::steady_clock::now();
      const HRESULT drawHr = state_->textRenderer->DrawTextBatch(
          state_->context.Get(), textItems.data(), static_cast<int>(textItems.size()),
          &textStats);
      const auto textEnd = std::chrono::steady_clock::now();
      diagnostics.textDrawMs += DurationMs(textStart, textEnd);
      if (FAILED(drawHr)) {
        return drawHr;
      }
      break;
    }
    }
  }

  if (batchHr != S_FALSE) {
    return FAILED(batchHr) ? batchHr : E_INVALIDARG;
  }

  return S_OK;
}

void D3D11ShareD3D9Renderer::RecordFramePerformance(double drawDurationMs) {
  ++submittedFrameCount_;

  if (drawDurationMetricId_ > 0) {
    FDVLOG_LogMetric(drawDurationMetricId_, drawDurationMs);
  }

  const std::uint64_t nowQpc = QueryQpcNow();
  if (lastPresentQpc_ != 0 && fpsMetricId_ > 0) {
    const std::uint64_t deltaTicks = nowQpc - lastPresentQpc_;
    const std::uint64_t freq = QueryQpcFrequency();
    if (deltaTicks > 0 && freq > 0) {
      const double fps =
          static_cast<double>(freq) / static_cast<double>(deltaTicks);
      FDVLOG_LogMetric(fpsMetricId_, fps);
    }
  }
  lastPresentQpc_ = nowQpc;

  if (drawDurationMs >= kSlowFrameThresholdMs &&
      (submittedFrameCount_ % kSlowFrameLogEveryNFrames) == 0) {
    wchar_t message[320]{};
    swprintf_s(message,
               L"slow frame renderer=0x%p drawMs=%.3f frame=%llu size=%dx%d.",
               static_cast<void*>(this), drawDurationMs,
               static_cast<unsigned long long>(submittedFrameCount_), width_,
               height_);
    FDVLOG_Log(FDVLOG_LevelWarn, kLogCategory, message, false);
    FDVLOG_WriteETW(FDVLOG_LevelWarn, kLogCategory, message, false);
  }
}

bool D3D11ShareD3D9Renderer::ValidateFramePacket(const void* framePacket,
                                                 int framePacketBytes) const {
  return framePacket != nullptr &&
         framePacketBytes >= static_cast<int>(sizeof(LayeredFramePacket));
}

HRESULT D3D11ShareD3D9Renderer::SubmitLayeredCommands(const void* framePacket,
                                                      int framePacketBytes) {
  RendererLockGuard lock(&cs_);

  if (!ValidateFramePacket(framePacket, framePacketBytes)) {
    return E_INVALIDARG;
  }

  const auto parseSubmitStart = std::chrono::steady_clock::now();
  const HRESULT hr = SubmitLayeredCommandsAndPreparePresent(
      static_cast<const LayeredFramePacket*>(framePacket));
  const auto parseSubmitEnd = std::chrono::steady_clock::now();

  if (parseSubmitDurationMetricId_ > 0) {
    const double parseSubmitDurationMs =
        std::chrono::duration<double, std::milli>(parseSubmitEnd -
                                                  parseSubmitStart)
            .count();
    FDVLOG_LogMetric(parseSubmitDurationMetricId_, parseSubmitDurationMs);
  }

  return hr;
}

HRESULT D3D11ShareD3D9Renderer::SubmitLayeredCommandsAndPreparePresent(
    const LayeredFramePacket* framePacket) {
  SubmitFrameDiagnostics diagnostics{};
  const auto drawStart = std::chrono::steady_clock::now();

  if (framePacket == nullptr) {
    return E_INVALIDARG;
  }

  void* currentRtv = nullptr;
  int drawSlotIndex = -1;
  const auto beginStart = std::chrono::steady_clock::now();
  const HRESULT beginHr = BeginSubmitFrame(currentRtv, drawSlotIndex);
  const auto beginEnd = std::chrono::steady_clock::now();
  diagnostics.beginFrameMs = DurationMs(beginStart, beginEnd);
  if (FAILED(beginHr) || drawSlotIndex < 0) {
    return beginHr;
  }

  for (int layerIndex = 0; layerIndex < LayeredFramePacket::kMaxLayerCount;
       ++layerIndex) {
    const auto& layer = framePacket->layers[layerIndex];
    if (layer.commandBytes <= 0 || layer.commandData == nullptr) {
      continue;
    }

    const HRESULT submitHr = SubmitCompiledBatches(layer, layerIndex,
                                                   drawSlotIndex, currentRtv,
                                                   diagnostics);
    if (FAILED(submitHr)) {
      state_->slots[drawSlotIndex].state = SurfaceState::Ready;
      return submitHr;
    }
  }

  if (state_->slots[drawSlotIndex].renderDoneQuery == nullptr) {
    state_->slots[drawSlotIndex].state = SurfaceState::Ready;
    return E_UNEXPECTED;
  }

  state_->context->End(state_->slots[drawSlotIndex].renderDoneQuery.Get());
  state_->context->Flush();
  DemoteReadyForPresentSlots(state_, drawSlotIndex);
  state_->slots[drawSlotIndex].state = SurfaceState::ReadyForPresent;

  const auto drawEnd = std::chrono::steady_clock::now();
  const double drawDurationMs = DurationMs(drawStart, drawEnd);

  RecordFramePerformance(drawDurationMs);
  return S_OK;
}

D3D11ShareD3D9Renderer::D3D11ShareD3D9Renderer(HWND hwnd, int width, int height)
    : width_(width), height_(height) {
  state_ = new (std::nothrow) D3D11ShareD3D9RendererState();
  if (state_ != nullptr) {
    state_->hwnd = hwnd;
  }
  InitializeCriticalSectionAndSpinCount(&cs_, 1000);
  csInitialized_ = true;
}

D3D11ShareD3D9Renderer::~D3D11ShareD3D9Renderer() {
  UnregisterMetrics();
  ReleaseRendererResources();
  delete state_;
  state_ = nullptr;

  if (csInitialized_) {
    DeleteCriticalSection(&cs_);
    csInitialized_ = false;
  }
}

void D3D11ShareD3D9Renderer::ReleaseRenderTargetResources() {
  if (state_ == nullptr) {
    return;
  }

  ReleaseFrameResources(state_);
}

HRESULT D3D11ShareD3D9Renderer::EnsureTextRenderer() {
  if (state_ == nullptr || state_->device == nullptr) {
    return E_UNEXPECTED;
  }

  if (state_->textRenderer == nullptr) {
    state_->textRenderer = std::make_unique<fdv::textd2d::D2DTextRenderer>();
    if (state_->textRenderer == nullptr) {
      return E_OUTOFMEMORY;
    }
  }

  return state_->textRenderer->EnsureDeviceResources(state_->device.Get());
}

HRESULT D3D11ShareD3D9Renderer::CreateDrawPipeline() {
  if (state_ == nullptr || state_->device == nullptr) {
    return E_UNEXPECTED;
  }

  if (state_->instanceVertexShader != nullptr &&
      state_->instancePixelShader != nullptr &&
      state_->instanceInputLayout != nullptr &&
      state_->unitQuadVertexBuffer != nullptr &&
      state_->viewConstantsBuffer != nullptr &&
      state_->blendState != nullptr && state_->rasterizerState != nullptr) {
    return S_OK;
  }

  ComPtr<ID3DBlob> instanceVsBlob;
  ComPtr<ID3DBlob> instancePsBlob;

  HRESULT hr = LoadShaderBlob(kInstanceVertexShaderObject, instanceVsBlob);
  if (FAILED(hr)) {
    return hr;
  }

  hr = LoadShaderBlob(kInstancePixelShaderObject, instancePsBlob);
  if (FAILED(hr)) {
    return hr;
  }

  hr = state_->device->CreateVertexShader(
      instanceVsBlob->GetBufferPointer(), instanceVsBlob->GetBufferSize(),
      nullptr, state_->instanceVertexShader.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state_->instanceVertexShader == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  hr = state_->device->CreatePixelShader(
      instancePsBlob->GetBufferPointer(), instancePsBlob->GetBufferSize(),
      nullptr, state_->instancePixelShader.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state_->instancePixelShader == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  D3D11_INPUT_ELEMENT_DESC instanceInputLayout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,
       D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16,
       D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32,
       D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48,
       D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64,
       D3D11_INPUT_PER_INSTANCE_DATA, 1},
  };
  hr = state_->device->CreateInputLayout(
      instanceInputLayout, ARRAYSIZE(instanceInputLayout),
      instanceVsBlob->GetBufferPointer(), instanceVsBlob->GetBufferSize(),
      state_->instanceInputLayout.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state_->instanceInputLayout == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  D3D11_BLEND_DESC blendDesc = {};
  blendDesc.AlphaToCoverageEnable = FALSE;
  blendDesc.IndependentBlendEnable = FALSE;
  blendDesc.RenderTarget[0].BlendEnable = TRUE;
  blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blendDesc.RenderTarget[0].RenderTargetWriteMask =
      D3D11_COLOR_WRITE_ENABLE_ALL;

  hr = state_->device->CreateBlendState(
      &blendDesc, state_->blendState.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state_->blendState == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  D3D11_RASTERIZER_DESC rsDesc = {};
  rsDesc.FillMode = D3D11_FILL_SOLID;
  rsDesc.CullMode = D3D11_CULL_NONE;
  rsDesc.FrontCounterClockwise = FALSE;
  rsDesc.DepthBias = 0;
  rsDesc.DepthBiasClamp = 0.0f;
  rsDesc.SlopeScaledDepthBias = 0.0f;
  rsDesc.DepthClipEnable = FALSE;
  rsDesc.ScissorEnable = FALSE;
  rsDesc.MultisampleEnable = FALSE;
  rsDesc.AntialiasedLineEnable = FALSE;

  hr = state_->device->CreateRasterizerState(
      &rsDesc, state_->rasterizerState.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state_->rasterizerState == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  hr = EnsureDynamicBuffer(state_->device.Get(), 16u,
                           D3D11_BIND_CONSTANT_BUFFER,
                           state_->viewConstantsBuffer);
  if (FAILED(hr)) {
    return hr;
  }

  hr = EnsureUnitQuadVertexBuffer(state_->device.Get(),
                                  state_->unitQuadVertexBuffer);
  if (FAILED(hr)) {
    return hr;
  }

  return S_OK;
}

void D3D11ShareD3D9Renderer::ReleaseRendererResources() {
  if (state_ == nullptr) {
    return;
  }

  state_->batchCompiler = batch::BatchCompiler();

  ReleaseRenderTargetResources();
  state_->textRenderer.reset();
  state_->unitQuadVertexBuffer.Reset();
  state_->dynamicInstanceBuffer.Reset();
  state_->dynamicInstanceCapacityBytes = 0;
  state_->viewConstantsBuffer.Reset();
  state_->rasterizerState.Reset();
  state_->blendState.Reset();
  state_->instanceInputLayout.Reset();
  state_->instancePixelShader.Reset();
  state_->instanceVertexShader.Reset();
  state_->d3d9Device.Reset();
  state_->d3d9.Reset();
  state_->context.Reset();
  state_->device.Reset();
}

HRESULT D3D11ShareD3D9Renderer::Initialize() {
  RendererLockGuard lock(&cs_);
  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  if (state_->hwnd == nullptr || width_ <= 0 || height_ <= 0) {
    return E_INVALIDARG;
  }

  return CreateDevicesAndResources();
}

HRESULT D3D11ShareD3D9Renderer::CreateD3D11Device() {
  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  if (state_->device != nullptr && state_->context != nullptr) {
    return S_OK;
  }

  const D3D_FEATURE_LEVEL featureLevels[] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_1,
  };
  D3D_FEATURE_LEVEL createdFeatureLevel = D3D_FEATURE_LEVEL_9_1;

  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, kCreationFlags, featureLevels,
      ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
      state_->device.ReleaseAndGetAddressOf(), &createdFeatureLevel,
      state_->context.ReleaseAndGetAddressOf());

  if (FAILED(hr) || state_->device == nullptr || state_->context == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  static_cast<void>(createdFeatureLevel);
  return S_OK;
}

HRESULT D3D11ShareD3D9Renderer::CreateD3D9Device() {
  if (width_ <= 0 || height_ <= 0) {
    return E_INVALIDARG;
  }

  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  if (state_->hwnd == nullptr) {
    return E_INVALIDARG;
  }

  if (state_->d3d9Device != nullptr) {
    return S_OK;
  }

  if (state_->d3d9 == nullptr) {
    const HRESULT createD3D9Hr =
        Direct3DCreate9Ex(D3D_SDK_VERSION, state_->d3d9.ReleaseAndGetAddressOf());
    if (FAILED(createD3D9Hr) || state_->d3d9 == nullptr) {
      return FAILED(createD3D9Hr) ? createD3D9Hr : E_FAIL;
    }
  }

  D3DPRESENT_PARAMETERS parameters = {};
  parameters.Windowed = TRUE;
  parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
  parameters.BackBufferFormat = D3DFMT_UNKNOWN;
  parameters.BackBufferWidth = 1;
  parameters.BackBufferHeight = 1;
  parameters.BackBufferCount = 1;
  parameters.hDeviceWindow = state_->hwnd;
  parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  parameters.EnableAutoDepthStencil = FALSE;

  HRESULT hr = state_->d3d9->CreateDeviceEx(
      D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, state_->hwnd,
      D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED |
          D3DCREATE_FPU_PRESERVE,
      &parameters, nullptr, state_->d3d9Device.ReleaseAndGetAddressOf());
  if (FAILED(hr)) {
    hr = state_->d3d9->CreateDeviceEx(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, state_->hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED |
            D3DCREATE_FPU_PRESERVE,
        &parameters, nullptr, state_->d3d9Device.ReleaseAndGetAddressOf());
  }

  if (FAILED(hr) || state_->d3d9Device == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  return S_OK;
}

HRESULT D3D11ShareD3D9Renderer::CreateFrameResources() {
  if (state_ == nullptr || state_->device == nullptr || state_->context == nullptr ||
      state_->d3d9Device == nullptr || width_ <= 0 || height_ <= 0) {
    return E_UNEXPECTED;
  }

  ReleaseRenderTargetResources();

  D3D11_TEXTURE2D_DESC workTextureDesc = {};
  workTextureDesc.Width = static_cast<UINT>(width_);
  workTextureDesc.Height = static_cast<UINT>(height_);
  workTextureDesc.MipLevels = 1;
  workTextureDesc.ArraySize = 1;
  workTextureDesc.Format = kSharedTextureFormat;
  workTextureDesc.SampleDesc.Count = 1;
  workTextureDesc.Usage = D3D11_USAGE_DEFAULT;
  workTextureDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

  D3D11_QUERY_DESC queryDesc = {};
  queryDesc.Query = D3D11_QUERY_EVENT;

  for (int index = 0; index < kFrameCount; ++index) {
    auto& slot = state_->slots[index];

    HRESULT hr = state_->device->CreateTexture2D(
        &workTextureDesc, nullptr, slot.workTexture.ReleaseAndGetAddressOf());
    if (FAILED(hr) || slot.workTexture == nullptr) {
      ReleaseRenderTargetResources();
      return FAILED(hr) ? hr : E_FAIL;
    }

    hr = state_->device->CreateRenderTargetView(
        slot.workTexture.Get(), nullptr, slot.workRtv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || slot.workRtv == nullptr) {
      ReleaseRenderTargetResources();
      return FAILED(hr) ? hr : E_FAIL;
    }

    hr = state_->device->CreateQuery(&queryDesc,
                                     slot.renderDoneQuery.ReleaseAndGetAddressOf());
    if (FAILED(hr) || slot.renderDoneQuery == nullptr) {
      ReleaseRenderTargetResources();
      return FAILED(hr) ? hr : E_FAIL;
    }

    hr = state_->device->CreateQuery(&queryDesc,
                                     slot.copyDoneQuery.ReleaseAndGetAddressOf());
    if (FAILED(hr) || slot.copyDoneQuery == nullptr) {
      ReleaseRenderTargetResources();
      return FAILED(hr) ? hr : E_FAIL;
    }

    HANDLE sharedHandle = nullptr;
    hr = state_->d3d9Device->CreateTexture(
        static_cast<UINT>(width_), static_cast<UINT>(height_), 1,
        D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        slot.texture9.ReleaseAndGetAddressOf(), &sharedHandle);
    if (FAILED(hr) || slot.texture9 == nullptr || sharedHandle == nullptr) {
      ReleaseRenderTargetResources();
      return FAILED(hr) ? hr : E_FAIL;
    }

    slot.sharedHandle = sharedHandle;
    hr = slot.texture9->GetSurfaceLevel(0, slot.surface9.ReleaseAndGetAddressOf());
    if (FAILED(hr) || slot.surface9 == nullptr) {
      ReleaseRenderTargetResources();
      return FAILED(hr) ? hr : E_FAIL;
    }

    hr = state_->device->OpenSharedResource(
        slot.sharedHandle, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(slot.sharedTexture11.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || slot.sharedTexture11 == nullptr) {
      ReleaseRenderTargetResources();
      return FAILED(hr) ? hr : E_FAIL;
    }

    hr = state_->d3d9Device->CreateQuery(
        D3DQUERYTYPE_EVENT, slot.presentDoneQuery.ReleaseAndGetAddressOf());
    if (FAILED(hr) || slot.presentDoneQuery == nullptr) {
      ReleaseRenderTargetResources();
      return FAILED(hr) ? hr : E_FAIL;
    }

    slot.state = SurfaceState::Ready;
  }

  const HRESULT presentSurfaceHr = state_->d3d9Device->CreateRenderTarget(
      static_cast<UINT>(width_), static_cast<UINT>(height_), D3DFMT_A8R8G8B8,
      D3DMULTISAMPLE_NONE, 0, FALSE,
      state_->presentingSurface.ReleaseAndGetAddressOf(), nullptr);
  if (FAILED(presentSurfaceHr) || state_->presentingSurface == nullptr) {
    ReleaseRenderTargetResources();
    return FAILED(presentSurfaceHr) ? presentSurfaceHr : E_FAIL;
  }

  state_->activeTextTargetSlotIndex = -1;
  return S_OK;
}

HRESULT D3D11ShareD3D9Renderer::CreateDevicesAndResources() {
  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  ReleaseRendererResources();

  HRESULT hr = CreateD3D11Device();
  if (FAILED(hr)) {
    ReleaseRendererResources();
    return hr;
  }

  hr = CreateD3D9Device();
  if (FAILED(hr)) {
    ReleaseRendererResources();
    return hr;
  }

  hr = EnsureTextRenderer();
  if (FAILED(hr)) {
    ReleaseRendererResources();
    return hr;
  }

  hr = CreateDrawPipeline();
  if (FAILED(hr)) {
    ReleaseRendererResources();
    return hr;
  }

  hr = CreateFrameResources();
  if (FAILED(hr)) {
    ReleaseRendererResources();
    return hr;
  }

  return S_OK;
}

HRESULT D3D11ShareD3D9Renderer::ResizeFrameResources(int width, int height) {
  if (width <= 0 || height <= 0) {
    return E_INVALIDARG;
  }

  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  width_ = width;
  height_ = height;

  if (state_->device == nullptr || state_->context == nullptr ||
      state_->d3d9Device == nullptr) {
    return CreateDevicesAndResources();
  }

  return CreateFrameResources();
}

HRESULT D3D11ShareD3D9Renderer::Resize(int width, int height) {
  RendererLockGuard lock(&cs_);
  return ResizeFrameResources(width, height);
}

HRESULT D3D11ShareD3D9Renderer::TryAcquirePresentSurface(void** outSurface9) {
  if (outSurface9 == nullptr) {
    return E_POINTER;
  }

  *outSurface9 = nullptr;

  RendererLockGuard lock(&cs_);
  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  if (state_->presentingSurface == nullptr) {
    return S_FALSE;
  }

  *outSurface9 = state_->presentingSurface.Get();
  return S_OK;
}

HRESULT D3D11ShareD3D9Renderer::CopyReadyToPresentSurface() {
  RendererLockGuard lock(&cs_);

  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  if (state_->d3d9Device == nullptr || state_->context == nullptr ||
      state_->presentingSurface == nullptr || !state_->frontBufferAvailable) {
    return E_UNEXPECTED;
  }

  const int readySlotIndex = FindSlotByState(state_, SurfaceState::ReadyForPresent);
  if (readySlotIndex < 0) {
    return S_FALSE;
  }

  auto& slot = state_->slots[readySlotIndex];
  if (slot.surface9 == nullptr || slot.workTexture == nullptr ||
      slot.sharedTexture11 == nullptr || slot.copyDoneQuery == nullptr) {
    return S_FALSE;
  }

  const auto presentStart = std::chrono::steady_clock::now();
  HRESULT hr = WaitForD3D11Query(state_->context.Get(), slot.renderDoneQuery.Get());
  if (FAILED(hr)) {
    return hr;
  }

  state_->context->CopyResource(slot.sharedTexture11.Get(), slot.workTexture.Get());
  state_->context->End(slot.copyDoneQuery.Get());
  state_->context->Flush();

  hr = WaitForD3D11Query(state_->context.Get(), slot.copyDoneQuery.Get());
  if (FAILED(hr)) {
    return hr;
  }

  hr = state_->d3d9Device->StretchRect(slot.surface9.Get(), nullptr,
                                       state_->presentingSurface.Get(), nullptr,
                                       D3DTEXF_NONE);
  if (FAILED(hr)) {
    return hr;
  }

  if (slot.presentDoneQuery != nullptr) {
    hr = slot.presentDoneQuery->Issue(D3DISSUE_END);
    if (FAILED(hr)) {
      return hr;
    }

    hr = WaitForD3D9Query(slot.presentDoneQuery.Get());
    if (FAILED(hr)) {
      return hr;
    }
  }

  slot.state = SurfaceState::Ready;

  if (presentDurationMetricId_ > 0) {
    const auto presentEnd = std::chrono::steady_clock::now();
    FDVLOG_LogMetric(presentDurationMetricId_,
                     DurationMs(presentStart, presentEnd));
  }

  return S_OK;
}

void D3D11ShareD3D9Renderer::OnFrontBufferAvailable(bool available) {
  RendererLockGuard lock(&cs_);

  if (state_ == nullptr) {
    return;
  }

  state_->frontBufferAvailable = available;
  state_->activeTextTargetSlotIndex = -1;

  for (int index = 0; index < kFrameCount; ++index) {
    state_->slots[index].state = SurfaceState::Ready;
  }
}

} // namespace fdv::d3d11
