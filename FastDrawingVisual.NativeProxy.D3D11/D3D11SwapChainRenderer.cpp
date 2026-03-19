#include "D3D11SwapChainRenderer.h"
#include "../FastDrawingVisual.LogCore/FdvLogCoreExports.h"
#include "BatchComplier.h"
#include "D3D11DeviceManager.h"

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <memory>
#include <vector>
#include <wrl/client.h>

namespace fdv::d3d11 {
using Microsoft::WRL::ComPtr;

struct D3D11SwapChainRendererState {
  D3D11DeviceManager* deviceManager = nullptr;
  ComPtr<IDXGISwapChain1> swapChain = nullptr;
  ComPtr<ID3D11RenderTargetView> rtv0 = nullptr;
  ComPtr<ID3D11Buffer> instanceBuffer = nullptr;
  UINT instanceBufferCapacityBytes = 0;
  batch::BatchCompiler batchCompiler;
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
constexpr const wchar_t* kLogCategory = L"NativeProxy.D3D11";
constexpr double kSlowFrameThresholdMs = 33.0;
constexpr std::uint64_t kSlowFrameLogEveryNFrames = 120;
constexpr std::uint64_t kDetailedFrameLogEveryNFrames = 30;
constexpr uint32_t kMetricWindowSec = 1;
constexpr const wchar_t* kDrawMetricFormat =
    L"name={name} periodSec={periodSec}s samples={count} avgMs={avg} minMs={min} maxMs={max}";
constexpr const wchar_t* kFpsMetricFormat =
    L"name={name} periodSec={periodSec}s samples={count} avgFps={avg} minFps={min} maxFps={max}";
constexpr UINT kBufferCount = 3;
constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

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

double DurationMs(const std::chrono::steady_clock::time_point& start,
                  const std::chrono::steady_clock::time_point& end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

HRESULT CreateDxgiFactoryFromDevice(ID3D11Device* device,
                                    ComPtr<IDXGIFactory2>& factoryOut) {
  if (device == nullptr) {
    return E_INVALIDARG;
  }

  factoryOut.Reset();

  ComPtr<IDXGIDevice> dxgiDevice;
  ComPtr<IDXGIAdapter> adapter;

  HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice),
                                      reinterpret_cast<void**>(
                                          dxgiDevice.GetAddressOf()));
  if (FAILED(hr) || dxgiDevice == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
  if (FAILED(hr) || adapter == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  hr = adapter->GetParent(__uuidof(IDXGIFactory2),
                          reinterpret_cast<void**>(
                              factoryOut.ReleaseAndGetAddressOf()));
  if (FAILED(hr) || factoryOut == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  return S_OK;
}

HRESULT EnsureReusableInstanceBuffer(D3D11DeviceManager& deviceManager,
                                     UINT requiredBytes,
                                     ComPtr<ID3D11Buffer>& bufferOut,
                                     UINT& capacityBytesOut) {
  if (requiredBytes == 0) {
    return S_OK;
  }

  const UINT minBytes = (std::max)(requiredBytes, 1024u);
  if (bufferOut != nullptr && capacityBytesOut >= minBytes) {
    return S_OK;
  }

  ComPtr<ID3D11Device> device = deviceManager.GetDevice();
  if (device == nullptr) {
    return E_UNEXPECTED;
  }

  D3D11_BUFFER_DESC bufferDesc = {};
  bufferDesc.ByteWidth = minBytes;
  bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
  bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  ComPtr<ID3D11Buffer> instanceBuffer;
  const HRESULT hr = device->CreateBuffer(
      &bufferDesc, nullptr, instanceBuffer.GetAddressOf());
  if (FAILED(hr) || instanceBuffer == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  bufferOut = instanceBuffer;
  capacityBytesOut = minBytes;
  return S_OK;
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

int EstimateFrameCommandCount(const LayeredFramePacket* framePacket) {
  if (framePacket == nullptr) {
    return 0;
  }

  int commandCount = 0;
  for (int layerIndex = 0; layerIndex < LayeredFramePacket::kMaxLayerCount;
       ++layerIndex) {
    commandCount = (std::max)(0, commandCount) +
                   (std::max)(0, framePacket->layers[layerIndex].commandCount);
  }
  return commandCount;
}

void AppendImageBatchItems(const std::vector<batch::ImageBatchItem>& source,
                           D3D11FrameTask& task) {
  task.imageItems.insert(task.imageItems.end(), source.begin(), source.end());
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
} // namespace

void D3D11SwapChainRenderer::RegisterMetrics() {
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

void D3D11SwapChainRenderer::UnregisterMetrics() {
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

HRESULT D3D11SwapChainRenderer::BeginSubmitFrame(void*& currentRtv) {
  if (state_->rtv0 == nullptr || width_ <= 0 || height_ <= 0) {
    return E_UNEXPECTED;
  }

  currentRtv = state_->rtv0.Get();
  return S_OK;
}

HRESULT D3D11SwapChainRenderer::CollectFrameTask(
    const LayeredFramePacket* framePacket, D3D11FrameTask& task,
    SubmitFrameDiagnostics& diagnostics) {
  if (framePacket == nullptr) {
    return E_UNEXPECTED;
  }

  auto& sharedManager = *state_->deviceManager;
  const int estimatedCommandCount = EstimateFrameCommandCount(framePacket);
  const UINT requiredInstanceBytes =
      estimatedCommandCount > 0
          ? static_cast<UINT>(estimatedCommandCount) *
                static_cast<UINT>(sizeof(batch::ShapeInstance))
          : 0u;

  const HRESULT bufferHr = EnsureReusableInstanceBuffer(
      sharedManager, requiredInstanceBytes, state_->instanceBuffer,
      state_->instanceBufferCapacityBytes);
  if (FAILED(bufferHr)) {
    return bufferHr;
  }

  task.SetInstanceBuffer(requiredInstanceBytes > 0 ? state_->instanceBuffer.Get()
                                                   : nullptr);
  task.shapeInstanceCount = 0;
  task.textItems.clear();
  task.imageItems.clear();
  task.hasClear = false;
  std::fill(std::begin(task.clearColor), std::end(task.clearColor), 0.0f);
  UINT instanceBufferUsedBytes = 0;

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  std::uint8_t* mappedBytes = nullptr;
  ComPtr<ID3D11DeviceContext> uploadContext;
  bool instanceBufferMapped = false;
  std::unique_ptr<ExecutionLockGuard<D3D11DeviceManager>> executionLock;
  if (task.instanceBuffer != nullptr) {
    executionLock =
        std::make_unique<ExecutionLockGuard<D3D11DeviceManager>>(
            sharedManager);
    uploadContext = sharedManager.GetImmediateContext();
    if (uploadContext == nullptr) {
      return E_UNEXPECTED;
    }

    const HRESULT mapHr =
        uploadContext->Map(task.instanceBuffer, 0, D3D11_MAP_WRITE_DISCARD,
                           0, &mapped);
    if (FAILED(mapHr) || mapped.pData == nullptr) {
      return FAILED(mapHr) ? mapHr : E_FAIL;
    }
    mappedBytes = static_cast<std::uint8_t*>(mapped.pData);
    instanceBufferMapped = true;
  }

  for (int layerIndex = 0; layerIndex < LayeredFramePacket::kMaxLayerCount;
       ++layerIndex) {
    const auto& layer = framePacket->layers[layerIndex];
    if (layer.commandData == nullptr || layer.commandBytes <= 0) {
      continue;
    }

    ++diagnostics.layerCount;
    state_->batchCompiler.Reset(width_, height_, layer.commandData,
                                layer.commandBytes, layer.blobData,
                                layer.blobBytes);

    batch::CompiledBatchView batch{};
    HRESULT batchHr = S_OK;
    while (true) {
      const auto compileStart = std::chrono::steady_clock::now();
      batchHr = state_->batchCompiler.TryGetNextBatch(batch);
      const auto compileEnd = std::chrono::steady_clock::now();
      diagnostics.compileMs += DurationMs(compileStart, compileEnd);
      AccumulateCompileStats(diagnostics, state_->batchCompiler.lastBatchStats());
      if (batchHr != S_OK) {
        break;
      }

      switch (batch.kind) {
      case batch::BatchKind::Clear:
        ++diagnostics.clearBatchCount;
        task.hasClear = true;
        std::copy(std::begin(batch.clearColor), std::end(batch.clearColor),
                  std::begin(task.clearColor));
        break;

      case batch::BatchKind::Triangles:
        if (instanceBufferMapped) {
          uploadContext->Unmap(task.instanceBuffer, 0);
        }
        return E_NOTIMPL;

      case batch::BatchKind::ShapeInstances: {
        ++diagnostics.shapeBatchCount;
        const auto& shapeInstances = state_->batchCompiler.GetShapeInstances();
        const UINT batchBytes =
            static_cast<UINT>(shapeInstances.size()) *
            static_cast<UINT>(sizeof(batch::ShapeInstance));
        if (batchBytes > 0 &&
            (mappedBytes == nullptr ||
             instanceBufferUsedBytes + batchBytes >
                 state_->instanceBufferCapacityBytes)) {
          if (instanceBufferMapped) {
            uploadContext->Unmap(task.instanceBuffer, 0);
          }
          return mappedBytes == nullptr ? E_UNEXPECTED : E_BOUNDS;
        }

        if (batchBytes > 0) {
          std::memcpy(mappedBytes + instanceBufferUsedBytes,
                       shapeInstances.data(), batchBytes);
          instanceBufferUsedBytes += batchBytes;
          task.shapeInstanceCount += static_cast<int>(shapeInstances.size());
        }
        diagnostics.maxVertexBufferCapacityBytes =
            (std::max)(diagnostics.maxVertexBufferCapacityBytes,
                       state_->instanceBufferCapacityBytes);
        diagnostics.vertexBytesUploaded = instanceBufferUsedBytes;
        break;
      }

      case batch::BatchKind::Text: {
        ++diagnostics.textBatchCount;
        const auto& textItems = state_->batchCompiler.GetTextItems();
        diagnostics.maxTextBatchItemCount =
            (std::max)(diagnostics.maxTextBatchItemCount,
                       static_cast<int>(textItems.size()));
        task.textItems.insert(task.textItems.end(), textItems.begin(),
                              textItems.end());
        break;
      }

      case batch::BatchKind::Image: {
        ++diagnostics.imageBatchCount;
        const auto& imageItems = state_->batchCompiler.GetImageItems();
        diagnostics.maxImageBatchItemCount =
            (std::max)(diagnostics.maxImageBatchItemCount,
                       static_cast<int>(imageItems.size()));
        AppendImageBatchItems(imageItems, task);
        break;
      }

      default:
        if (instanceBufferMapped) {
          uploadContext->Unmap(task.instanceBuffer, 0);
        }
        return E_INVALIDARG;
      }
    }

    if (batchHr != S_FALSE) {
      if (instanceBufferMapped) {
        uploadContext->Unmap(task.instanceBuffer, 0);
      }
      const HRESULT hr = FAILED(batchHr) ? batchHr : E_INVALIDARG;
      wchar_t message[512]{};
      swprintf_s(
          message,
          L"batch compilation failed renderer=0x%p layer=%d hr=0x%08X commandBytes=%d blobBytes=%d compileMs=%.3f readMs=%.3f buildMs=%.3f.",
          static_cast<void*>(this), layerIndex, static_cast<unsigned int>(hr),
          layer.commandBytes, layer.blobBytes, diagnostics.compileMs,
          diagnostics.commandReadMs, diagnostics.commandBuildMs);
      FDVLOG_Log(FDVLOG_LevelError, kLogCategory, message, false);
      FDVLOG_WriteETW(FDVLOG_LevelError, kLogCategory, message, false);
      return hr;
    }
  }

  if (instanceBufferMapped) {
    uploadContext->Unmap(task.instanceBuffer, 0);
  }

  task.viewportWidth = static_cast<float>(width_);
  task.viewportHeight = static_cast<float>(height_);

  return S_OK;
}

void D3D11SwapChainRenderer::RecordFramePerformance(double drawDurationMs) {
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

bool D3D11SwapChainRenderer::ValidateFramePacket(const void* framePacket,
                                                 int framePacketBytes) const {
  return framePacket != nullptr &&
         framePacketBytes >= static_cast<int>(sizeof(LayeredFramePacket));
}

HRESULT D3D11SwapChainRenderer::SubmitLayeredCommands(const void* framePacket,
                                                      int framePacketBytes) {
  RendererLockGuard lock(&cs_);

  if (!ValidateFramePacket(framePacket, framePacketBytes)) {
    return E_INVALIDARG;
  }

  const auto parseSubmitStart = std::chrono::steady_clock::now();
  const HRESULT hr = SubmitLayeredCommandsAndPresent(
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

HRESULT D3D11SwapChainRenderer::SubmitLayeredCommandsAndPresent(
    const LayeredFramePacket* framePacket) {
  SubmitFrameDiagnostics diagnostics{};
  const auto drawStart = std::chrono::steady_clock::now();
  const std::uint64_t frameId = submittedFrameCount_ + 1;

  if (framePacket == nullptr) {
    return E_INVALIDARG;
  }

  void* currentRtv = nullptr;
  const auto beginStart = std::chrono::steady_clock::now();
  const HRESULT beginHr = BeginSubmitFrame(currentRtv);
  const auto beginEnd = std::chrono::steady_clock::now();
  diagnostics.beginFrameMs = DurationMs(beginStart, beginEnd);
  if (FAILED(beginHr)) {
    LogStageFailure(static_cast<void*>(this), frameId, L"begin_frame", beginHr,
                    diagnostics, width_, height_);
    return beginHr;
  }

  D3D11FrameTask task{};
  task.SetRenderTargetView(static_cast<ID3D11RenderTargetView*>(currentRtv));
  task.completion = std::make_shared<D3DFrameTaskCompletion>();
  if (task.completion == nullptr) {
    return E_OUTOFMEMORY;
  }

  const HRESULT collectHr = CollectFrameTask(framePacket, task, diagnostics);
  if (FAILED(collectHr)) {
    LogStageFailure(static_cast<void*>(this), frameId, L"collect_frame_task",
                    collectHr, diagnostics, width_, height_);
    return collectHr;
  }

  D3DFrameTaskResult taskResult{};
  task.completion->resultSink = &taskResult;
  task.completion->completed = false;
  const auto completion = task.completion;

  const HRESULT submitTaskHr =
      state_->deviceManager->SubmitFrame(std::move(task));
  if (FAILED(submitTaskHr)) {
    LogStageFailure(static_cast<void*>(this), frameId, L"submit_frame_task",
                    submitTaskHr, diagnostics, width_, height_);
    return submitTaskHr;
  }

  const HRESULT waitHr = WaitForTaskCompletion(completion, taskResult);
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
    LogStageFailure(static_cast<void*>(this), frameId, L"wait_frame_task",
                    waitHr, diagnostics, width_, height_);
    return waitHr;
  }

  if (state_->swapChain == nullptr) {
    return E_UNEXPECTED;
  }

  const auto presentStart = std::chrono::steady_clock::now();
  const HRESULT hr = state_->swapChain->Present(0, 0);
  const auto presentEnd = std::chrono::steady_clock::now();
  diagnostics.presentMs = DurationMs(presentStart, presentEnd);

  const auto drawEnd = std::chrono::steady_clock::now();
  const double drawDurationMs = DurationMs(drawStart, drawEnd);

  if (FAILED(hr)) {
    LogStageFailure(static_cast<void*>(this), frameId, L"present", hr,
                    diagnostics, width_, height_);
    return hr;
  }

  if (compileDurationMetricId_ > 0) {
    FDVLOG_LogMetric(compileDurationMetricId_, diagnostics.compileMs);
  }
  if (commandReadDurationMetricId_ > 0) {
    FDVLOG_LogMetric(commandReadDurationMetricId_, diagnostics.commandReadMs);
  }
  if (commandBuildDurationMetricId_ > 0) {
    FDVLOG_LogMetric(commandBuildDurationMetricId_, diagnostics.commandBuildMs);
  }
  if (triangleCpuDurationMetricId_ > 0) {
    FDVLOG_LogMetric(triangleCpuDurationMetricId_, diagnostics.triangleCpuMs);
  }
  if (triangleUploadDurationMetricId_ > 0) {
    FDVLOG_LogMetric(triangleUploadDurationMetricId_,
                     diagnostics.triangleUploadMs);
  }
  if (triangleDrawCallDurationMetricId_ > 0) {
    FDVLOG_LogMetric(triangleDrawCallDurationMetricId_,
                     diagnostics.triangleDrawCallMs);
  }
  if (textDurationMetricId_ > 0) {
    FDVLOG_LogMetric(textDurationMetricId_, diagnostics.textDrawMs);
  }
  if (presentDurationMetricId_ > 0) {
    FDVLOG_LogMetric(presentDurationMetricId_, diagnostics.presentMs);
  }

  RecordFramePerformance(drawDurationMs);
  if (drawDurationMs >= kSlowFrameThresholdMs &&
      (frameId % kSlowFrameLogEveryNFrames) == 0) {
    LogFrameBreakdown(static_cast<void*>(this), frameId, width_, height_,
                      drawDurationMs, diagnostics, FDVLOG_LevelWarn);
  } else if ((frameId % kDetailedFrameLogEveryNFrames) == 0) {
    LogFrameBreakdown(static_cast<void*>(this), frameId, width_, height_,
                      drawDurationMs, diagnostics, FDVLOG_LevelInfo);
  }
  return S_OK;
}

D3D11SwapChainRenderer::D3D11SwapChainRenderer(int width, int height)
    : width_(width), height_(height) {
  state_ = new D3D11SwapChainRendererState();
  state_->deviceManager = &D3D11DeviceManager::Instance();
  state_->deviceManager->RegisterClient();

  InitializeCriticalSectionAndSpinCount(&cs_, 1000);
  csInitialized_ = true;
}

D3D11SwapChainRenderer::~D3D11SwapChainRenderer() {
  ReleaseRendererResources();
  state_->deviceManager->ReleaseClient();

  delete state_;
  state_ = nullptr;

  if (csInitialized_) {
    DeleteCriticalSection(&cs_);
    csInitialized_ = false;
  }
}

void D3D11SwapChainRenderer::ReleaseRenderTargetResources() {
  state_->rtv0.Reset();
}

HRESULT D3D11SwapChainRenderer::CreateRenderTarget() {
  if (state_->swapChain == nullptr) {
    return E_UNEXPECTED;
  }

  ReleaseRenderTargetResources();

  ComPtr<ID3D11Device> device = state_->deviceManager->GetDevice();
  if (device == nullptr) {
    return E_UNEXPECTED;
  }

  ComPtr<ID3D11Texture2D> backBuffer;
  HRESULT hr = state_->swapChain->GetBuffer(
      0, __uuidof(ID3D11Texture2D),
      reinterpret_cast<void**>(backBuffer.GetAddressOf()));
  if (FAILED(hr) || backBuffer == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  hr = device->CreateRenderTargetView(
      backBuffer.Get(), nullptr, state_->rtv0.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state_->rtv0 == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }
  return S_OK;
}

HRESULT D3D11SwapChainRenderer::CreateSwapChain() {
  ComPtr<ID3D11Device> device = state_->deviceManager->GetDevice();
  ComPtr<IDXGIFactory2> dxgiFactory;
  if (device == nullptr) {
    return E_UNEXPECTED;
  }

  const HRESULT ensureFactoryHr =
      CreateDxgiFactoryFromDevice(device.Get(), dxgiFactory);
  if (FAILED(ensureFactoryHr)) {
    return ensureFactoryHr;
  }

  DXGI_SWAP_CHAIN_DESC1 swapDesc = {};
  swapDesc.Width = static_cast<UINT>(width_);
  swapDesc.Height = static_cast<UINT>(height_);
  swapDesc.Format = kSwapChainFormat;
  swapDesc.Stereo = FALSE;
  swapDesc.SampleDesc.Count = 1;
  swapDesc.SampleDesc.Quality = 0;
  swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swapDesc.BufferCount = kBufferCount;
  swapDesc.Scaling = DXGI_SCALING_STRETCH;
  swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swapDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  swapDesc.Flags = 0;

  HRESULT hr = dxgiFactory->CreateSwapChainForComposition(
      device.Get(), &swapDesc, nullptr,
      state_->swapChain.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state_->swapChain == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  return S_OK;
}

void D3D11SwapChainRenderer::ReleaseRendererResources() {
  state_->batchCompiler = batch::BatchCompiler();
  state_->instanceBuffer.Reset();
  state_->instanceBufferCapacityBytes = 0;

  ReleaseRenderTargetResources();
  state_->swapChain.Reset();
}

HRESULT D3D11SwapChainRenderer::Initialize() {
  RendererLockGuard lock(&cs_);
  if (width_ <= 0 || height_ <= 0) {
      return E_INVALIDARG;
  }

  if (state_->swapChain != nullptr && state_->rtv0 != nullptr) {
      return S_OK;
  }

  HRESULT hr = state_->deviceManager->EnsureReady();
  if (FAILED(hr)) {
      ReleaseRendererResources();
      return hr;
  }

  const HRESULT createSwapChainHr = CreateSwapChain();
  if (FAILED(createSwapChainHr)) {
      ReleaseRendererResources();
      return createSwapChainHr;
  }

  const HRESULT createRenderTargetHr = CreateRenderTarget();
  if (FAILED(createRenderTargetHr)) {
      ReleaseRendererResources();
      return createRenderTargetHr;
  }

  return S_OK;
}

HRESULT D3D11SwapChainRenderer::Resize(int width, int height) {
  RendererLockGuard lock(&cs_);
  return ResizeSwapChain(width, height);
}

HRESULT D3D11SwapChainRenderer::ResizeSwapChain(int width, int height) {
  if (state_->swapChain == nullptr ) {
    return E_UNEXPECTED;
  }

  if (width <= 0 || height <= 0) {
    return E_INVALIDARG;
  }

  if (width_ == width && height_ == height) {
    return S_OK;
  }

  ReleaseRenderTargetResources();

  HRESULT hr = state_->swapChain->ResizeBuffers(
      kBufferCount, static_cast<UINT>(width), static_cast<UINT>(height),
      kSwapChainFormat, 0);
  if (FAILED(hr)) {
    return hr;
  }

  width_ = width;
  height_ = height;
  return CreateRenderTarget();
}

HRESULT D3D11SwapChainRenderer::TryGetSwapChain(void** outSwapChain) {
  if (outSwapChain == nullptr) {
    return E_POINTER;
  }

  *outSwapChain = nullptr;

  RendererLockGuard lock(&cs_);
  if (state_->swapChain == nullptr) {
    return E_UNEXPECTED;
  }

  *outSwapChain = state_->swapChain.Get();
  return S_OK;
}

} // namespace fdv::d3d11

