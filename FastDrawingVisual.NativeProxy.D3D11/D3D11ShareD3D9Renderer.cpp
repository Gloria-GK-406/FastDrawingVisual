#include "D3D11ShareD3D9Renderer.h"
#include "D3DTaskType.h"
#include "D3D11ShareD3D9DeviceManager.h"
#include "D3D11DeviceManager.h"
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
  batch::BatchCompiler batchCompiler;
  SurfaceSlot slots[kFrameCount];
  std::uint64_t deviceGeneration = 0;
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
  int imageBatchCount = 0;
  int triangleVertexCount = 0;
  int shapeInstanceCount = 0;
  int maxTriangleBatchVertexCount = 0;
  int textItemCount = 0;
  int maxTextBatchItemCount = 0;
  int textCharCount = 0;
  int imageItemCount = 0;
  int maxImageBatchItemCount = 0;
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
  double imageDrawMs = 0.0;
  double imageFlushMs = 0.0;
  double imageRecordMs = 0.0;
  double imageEndDrawMs = 0.0;
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
  diagnostics.imageItemCount += stats.imageItemCount;
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

D3D11ShareD3D9DeviceManager& GetDeviceManager() {
  return D3D11ShareD3D9DeviceManager::Instance();
}

HRESULT CreateShapeInstanceBuffer(
    ID3D11Device* device, const std::vector<batch::ShapeInstance>& shapeInstances,
    ID3D11Buffer*& bufferOut) {
  if (bufferOut != nullptr) {
    bufferOut->Release();
    bufferOut = nullptr;
  }
  if (shapeInstances.empty()) {
    return S_OK;
  }

  if (device == nullptr) {
    return E_POINTER;
  }

  D3D11_BUFFER_DESC bufferDesc = {};
  bufferDesc.ByteWidth =
      static_cast<UINT>(shapeInstances.size() * sizeof(batch::ShapeInstance));
  bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
  bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

  D3D11_SUBRESOURCE_DATA initialData = {};
  initialData.pSysMem = shapeInstances.data();

  ComPtr<ID3D11Buffer> createdBuffer;
  const HRESULT hr = device->CreateBuffer(&bufferDesc, &initialData,
                                          createdBuffer.GetAddressOf());
  if (FAILED(hr) || createdBuffer == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  bufferOut = createdBuffer.Detach();
  return S_OK;
}

HRESULT AppendImageItems(const std::vector<batch::ImageBatchItem>& sourceItems,
                        D3D11FrameTask& task) {
  const std::size_t baseIndex = task.imageItems.size();
  task.imagePixelBlobs.reserve(baseIndex + sourceItems.size());
  task.imageItems.reserve(baseIndex + sourceItems.size());

  for (const auto& sourceItem : sourceItems) {
    if (sourceItem.pixels == nullptr || sourceItem.pixelBytes == 0) {
      batch::ImageBatchItem item = sourceItem;
      item.pixels = nullptr;
      task.imagePixelBlobs.emplace_back();
      task.imageItems.push_back(item);
      continue;
    }

    task.imagePixelBlobs.emplace_back(
        sourceItem.pixels,
        sourceItem.pixels + static_cast<std::size_t>(sourceItem.pixelBytes));

    const auto& pixels = task.imagePixelBlobs.back();
    batch::ImageBatchItem item = sourceItem;
    item.pixels = pixels.data();
    task.imageItems.push_back(item);
  }

  return S_OK;
}

HRESULT WaitForTaskCompletion(
    const std::shared_ptr<D3DFrameTaskCompletion>& completion,
    D3DFrameTaskResult& result) {
  if (completion == nullptr) {
    return E_INVALIDARG;
  }

  std::unique_lock<std::mutex> lock(completion->mutex);
  completion->condition.wait(lock, [&completion]() {
    return completion->completed;
  });
  return result.hr;
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

HRESULT D3D11ShareD3D9Renderer::BeginSubmitFrame(int& drawSlotIndex,
                                                 D3D11FrameTask& task) {
  drawSlotIndex = -1;
  if (state_ == nullptr || width_ <= 0 ||
      height_ <= 0 || !state_->frontBufferAvailable) {
    return E_UNEXPECTED;
  }

  const int readySlotIndex = FindSlotByState(state_, SurfaceState::Ready);
  if (readySlotIndex < 0) {
    return S_FALSE;
  }

  drawSlotIndex = readySlotIndex;
  state_->slots[readySlotIndex].state = SurfaceState::Drawing;

  if (state_->slots[readySlotIndex].workRtv == nullptr ||
      state_->slots[readySlotIndex].renderDoneQuery == nullptr) {
    state_->slots[readySlotIndex].state = SurfaceState::Ready;
    drawSlotIndex = -1;
    return E_UNEXPECTED;
  }

  task.Reset();
  task.SetRenderTargetView(state_->slots[readySlotIndex].workRtv.Get());
  task.SetRenderDoneQuery(state_->slots[readySlotIndex].renderDoneQuery.Get());
  task.viewportWidth = static_cast<float>(width_);
  task.viewportHeight = static_cast<float>(height_);
  return S_OK;
}

HRESULT D3D11ShareD3D9Renderer::SubmitCompiledBatches(
    const LayerPacket& layer, int layerIndex, D3D11FrameTask& task,
    std::vector<batch::ShapeInstance>& shapeInstances,
    SubmitFrameDiagnostics& diagnostics) {
  if (state_ == nullptr) {
    return E_UNEXPECTED;
  }

  if (layer.commandData == nullptr || layer.commandBytes <= 0) {
    return S_OK;
  }

  ++diagnostics.layerCount;
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
      task.hasClear = true;
      task.clearColor[0] = batch.clearColor[0];
      task.clearColor[1] = batch.clearColor[1];
      task.clearColor[2] = batch.clearColor[2];
      task.clearColor[3] = batch.clearColor[3];
      break;

    case batch::BatchKind::Triangles: {
      return E_NOTIMPL;
    }

    case batch::BatchKind::ShapeInstances: {
      const auto& batchShapeInstances = state_->batchCompiler.GetShapeInstances();
      shapeInstances.insert(shapeInstances.end(), batchShapeInstances.begin(),
                            batchShapeInstances.end());
      break;
    }

    case batch::BatchKind::Text: {
      const auto& textItems = state_->batchCompiler.GetTextItems();
      task.textItems.insert(task.textItems.end(), textItems.begin(),
                            textItems.end());
      break;
    }

    case batch::BatchKind::Image: {
      ++diagnostics.imageBatchCount;
      const auto& imageItems = state_->batchCompiler.GetImageItems();
      const int imageItemCount = static_cast<int>(imageItems.size());
      diagnostics.maxImageBatchItemCount =
          (std::max)(diagnostics.maxImageBatchItemCount, imageItemCount);
      const HRESULT copyHr = AppendImageItems(imageItems, task);
      if (FAILED(copyHr)) {
        return copyHr;
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
  const std::uint64_t frameId = submittedFrameCount_ + 1;

  if (framePacket == nullptr) {
    return E_INVALIDARG;
  }

  const HRESULT sharedHr = EnsureSharedDeviceResources();
  if (FAILED(sharedHr)) {
    return sharedHr;
  }

  auto& deviceManager = GetDeviceManager();

  D3D11FrameTask task{};
  int drawSlotIndex = -1;
  const auto beginStart = std::chrono::steady_clock::now();
  const HRESULT beginHr = BeginSubmitFrame(drawSlotIndex, task);
  const auto beginEnd = std::chrono::steady_clock::now();
  diagnostics.beginFrameMs = DurationMs(beginStart, beginEnd);
  if (FAILED(beginHr) || drawSlotIndex < 0) {
    if (FAILED(beginHr)) {
      LogStageFailure(static_cast<void*>(this), frameId, L"begin_frame", beginHr,
                      diagnostics, width_, height_);
    }
    return beginHr;
  }

  std::vector<batch::ShapeInstance> shapeInstances;

  for (int layerIndex = 0; layerIndex < LayeredFramePacket::kMaxLayerCount;
       ++layerIndex) {
    const auto& layer = framePacket->layers[layerIndex];
    if (layer.commandBytes <= 0 || layer.commandData == nullptr) {
      continue;
    }

    const HRESULT submitHr =
        SubmitCompiledBatches(layer, layerIndex, task, shapeInstances, diagnostics);
    if (FAILED(submitHr)) {
      state_->slots[drawSlotIndex].state = SurfaceState::Ready;
      LogStageFailure(static_cast<void*>(this), frameId, L"compile_batches",
                      submitHr, diagnostics, width_, height_);
      return submitHr;
    }
  }

  if (!shapeInstances.empty()) {
    const HRESULT bufferHr =
        CreateShapeInstanceBuffer(state_->device.Get(), shapeInstances,
                                  task.instanceBuffer);
    if (FAILED(bufferHr)) {
      state_->slots[drawSlotIndex].state = SurfaceState::Ready;
      LogStageFailure(static_cast<void*>(this), frameId, L"build_shape_buffer",
                      bufferHr, diagnostics, width_, height_);
      return bufferHr;
    }
    task.shapeInstanceCount = static_cast<int>(shapeInstances.size());
    diagnostics.vertexBytesUploaded =
        static_cast<UINT>(shapeInstances.size() * sizeof(batch::ShapeInstance));
    diagnostics.maxVertexBufferCapacityBytes = diagnostics.vertexBytesUploaded;
  }

  auto completion = std::make_shared<D3DFrameTaskCompletion>();
  if (completion == nullptr) {
    state_->slots[drawSlotIndex].state = SurfaceState::Ready;
    return E_OUTOFMEMORY;
  }
  D3DFrameTaskResult taskResult{};
  completion->resultSink = &taskResult;
  completion->completed = false;
  task.completion = completion;
  const auto taskCompletion = completion;

  const HRESULT submitTaskHr = deviceManager.SubmitFrame(std::move(task));
  if (FAILED(submitTaskHr)) {
    state_->slots[drawSlotIndex].state = SurfaceState::Ready;
    LogStageFailure(static_cast<void*>(this), frameId, L"submit_task",
                    submitTaskHr, diagnostics, width_, height_);
    return submitTaskHr;
  }

  const HRESULT waitHr = WaitForTaskCompletion(taskCompletion, taskResult);
  diagnostics.triangleCpuMs += taskResult.stats.shapeDrawMs;
  diagnostics.textDrawMs += taskResult.stats.textDrawMs;
  diagnostics.textFlushMs += taskResult.stats.textFlushMs;
  diagnostics.textRecordMs += taskResult.stats.textRecordMs;
  diagnostics.textEndDrawMs += taskResult.stats.textEndDrawMs;
  diagnostics.imageDrawMs += taskResult.stats.imageDrawMs;
  diagnostics.imageFlushMs += taskResult.stats.imageFlushMs;
  diagnostics.imageRecordMs += taskResult.stats.imageRecordMs;
  diagnostics.imageEndDrawMs += taskResult.stats.imageEndDrawMs;
  if (FAILED(waitHr)) {
    state_->slots[drawSlotIndex].state = SurfaceState::Ready;
    LogStageFailure(static_cast<void*>(this), frameId, L"wait_task",
                    waitHr, diagnostics, width_, height_);
    return waitHr;
  }

  DemoteReadyForPresentSlots(state_, drawSlotIndex);
  state_->slots[drawSlotIndex].state = SurfaceState::ReadyForPresent;

  const auto drawEnd = std::chrono::steady_clock::now();
  const double drawDurationMs = DurationMs(drawStart, drawEnd);

  if (compileDurationMetricId_ > 0) {
    FDVLOG_LogMetric(compileDurationMetricId_, diagnostics.compileMs);
  }
  if (commandReadDurationMetricId_ > 0) {
    FDVLOG_LogMetric(commandReadDurationMetricId_, diagnostics.commandReadMs);
  }
  if (commandBuildDurationMetricId_ > 0) {
    FDVLOG_LogMetric(commandBuildDurationMetricId_, diagnostics.commandBuildMs);
  }

  RecordFramePerformance(drawDurationMs);
  return S_OK;
}

D3D11ShareD3D9Renderer::D3D11ShareD3D9Renderer(int width, int height)
    : width_(width), height_(height) {
  state_ = new (std::nothrow) D3D11ShareD3D9RendererState();
  if (state_ != nullptr) {
    GetDeviceManager().RegisterClient();
    managerClientRegistered_ = true;
  }
  InitializeCriticalSectionAndSpinCount(&cs_, 1000);
  csInitialized_ = true;
}

D3D11ShareD3D9Renderer::~D3D11ShareD3D9Renderer() {
  UnregisterMetrics();
  ReleaseRendererResources();
  if (managerClientRegistered_) {
    GetDeviceManager().ReleaseClient();
    managerClientRegistered_ = false;
  }
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

HRESULT D3D11ShareD3D9Renderer::EnsureSharedDeviceResources() {
  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  auto& deviceManager = GetDeviceManager();
  const HRESULT readyHr = deviceManager.EnsureReady();
  if (FAILED(readyHr)) {
    return readyHr;
  }

  const std::uint64_t generation = deviceManager.generation();
  if (state_->device != nullptr && state_->context != nullptr &&
      state_->d3d9 != nullptr && state_->d3d9Device != nullptr &&
      state_->deviceGeneration == generation) {
    return S_OK;
  }

  ReleaseRendererResources();

  state_->device = deviceManager.GetDevice();
  state_->context = deviceManager.GetImmediateContext();
  state_->d3d9 = deviceManager.GetD3D9();
  state_->d3d9Device = deviceManager.GetD3D9Device();
  state_->deviceGeneration = generation;

  if (state_->device == nullptr || state_->context == nullptr ||
      state_->d3d9 == nullptr || state_->d3d9Device == nullptr) {
    ReleaseRendererResources();
    return E_FAIL;
  }

  return S_OK;
}

void D3D11ShareD3D9Renderer::ReleaseRendererResources() {
  if (state_ == nullptr) {
    return;
  }

  state_->batchCompiler = batch::BatchCompiler();

  ReleaseRenderTargetResources();
  state_->deviceGeneration = 0;
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

  if (width_ <= 0 || height_ <= 0) {
    return E_INVALIDARG;
  }

  return CreateDeviceResources();
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

  return S_OK;
}

HRESULT D3D11ShareD3D9Renderer::CreateDeviceResources() {
  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  const HRESULT sharedHr = EnsureSharedDeviceResources();
  if (FAILED(sharedHr)) {
    return sharedHr;
  }

  if (state_->presentingSurface != nullptr &&
      state_->slots[0].workTexture != nullptr) {
    return S_OK;
  }

  const HRESULT hr = CreateFrameResources();
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

  const HRESULT sharedHr = EnsureSharedDeviceResources();
  if (FAILED(sharedHr)) {
    return sharedHr;
  }

  if (state_->device == nullptr || state_->context == nullptr ||
      state_->d3d9Device == nullptr) {
    return CreateDeviceResources();
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

  const HRESULT sharedHr = EnsureSharedDeviceResources();
  if (FAILED(sharedHr)) {
    return sharedHr;
  }

  auto& deviceManager = GetDeviceManager();
  ExecutionLockGuard<D3D11ShareD3D9DeviceManager> executionLock(deviceManager);

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

  for (int index = 0; index < kFrameCount; ++index) {
    state_->slots[index].state = SurfaceState::Ready;
  }
}

} // namespace fdv::d3d11
