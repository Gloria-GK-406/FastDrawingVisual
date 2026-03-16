#include "D3D11SwapChainRenderer.h"
#include "../FastDrawingVisual.LogCore/FdvLogCoreExports.h"
#include "BatchComplier.h"
#include "D3DBatchDraw.h"
#include "TextFormatCacheStore.h"

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <d3dcompiler.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <memory>
#include <wrl/client.h>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace fdv::d3d11 {
using Microsoft::WRL::ComPtr;

struct D3D11SwapChainRendererState {
  ComPtr<ID3D11Device> device = nullptr;
  ComPtr<ID3D11DeviceContext> context = nullptr;
  ComPtr<IDXGIFactory2> dxgiFactory = nullptr;
  ComPtr<IDXGISwapChain1> swapChain = nullptr;
  ComPtr<ID3D11RenderTargetView> rtv0 = nullptr;
  ComPtr<ID3D11VertexShader> vertexShader = nullptr;
  ComPtr<ID3D11PixelShader> pixelShader = nullptr;
  ComPtr<ID3D11InputLayout> inputLayout = nullptr;
  ComPtr<ID3D11BlendState> blendState = nullptr;
  ComPtr<ID3D11RasterizerState> rasterizerState = nullptr;
  ComPtr<ID3D11Buffer> dynamicVertexBuffer = nullptr;
  UINT dynamicVertexCapacityBytes = 0;
  ComPtr<ID2D1Factory1> d2dFactory = nullptr;
  ComPtr<ID2D1Device> d2dDevice = nullptr;
  ComPtr<ID2D1DeviceContext> d2dContext = nullptr;
  ComPtr<ID2D1Bitmap1> d2dTargetBitmap = nullptr;
  ComPtr<ID2D1SolidColorBrush> d2dSolidBrush = nullptr;
  ComPtr<IDWriteFactory> dwriteFactory = nullptr;
  std::unique_ptr<TextFormatCacheStore> textFormatCacheStore;
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
  int textBatchCount = 0;
  int triangleVertexCount = 0;
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
constexpr UINT kCreationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr const wchar_t* kTriangleVertexShaderObject = L"TriangleBatchVS.cso";
constexpr const wchar_t* kTrianglePixelShaderObject = L"TriangleBatchPS.cso";

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
      L"batches(clear=%d tri=%d text=%d) triVerts=%d maxTriBatchVerts=%d textItems=%d maxTextBatchItems=%d textChars=%d "
      L"beginMs=%.3f compileMs=%.3f readMs=%.3f buildMs=%.3f triCpuMs=%.3f triEnsureMs=%.3f triUploadMs=%.3f triDrawMs=%.3f "
      L"textMs=%.3f textFlushMs=%.3f textRecordMs=%.3f textEndMs=%.3f presentMs=%.3f vbResizes=%d uploadedBytes=%u maxVbBytes=%u totalMs=%.3f.",
      renderer, static_cast<unsigned long long>(frameId), width, height,
      diagnostics.layerCount, diagnostics.commandCount,
      diagnostics.clearCommandCount, diagnostics.fillRectCommandCount,
      diagnostics.strokeRectCommandCount,
      diagnostics.fillEllipseCommandCount,
      diagnostics.strokeEllipseCommandCount, diagnostics.lineCommandCount,
      diagnostics.textRunCommandCount, diagnostics.clearBatchCount,
      diagnostics.triangleBatchCount, diagnostics.textBatchCount,
      diagnostics.triangleVertexCount, diagnostics.maxTriangleBatchVertexCount,
      diagnostics.textItemCount, diagnostics.maxTextBatchItemCount,
      diagnostics.textCharCount, diagnostics.beginFrameMs,
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
      L"layers=%d cmds=%d triVerts=%d textItems=%d compileMs=%.3f triCpuMs=%.3f textMs=%.3f presentMs=%.3f.",
      renderer, static_cast<unsigned long long>(frameId), stage,
      static_cast<unsigned int>(hr), width, height, diagnostics.layerCount,
      diagnostics.commandCount, diagnostics.triangleVertexCount,
      diagnostics.textItemCount, diagnostics.compileMs,
      diagnostics.triangleCpuMs, diagnostics.textDrawMs,
      diagnostics.presentMs);
  FDVLOG_Log(FDVLOG_LevelError, kLogCategory, message, false);
  FDVLOG_WriteETW(FDVLOG_LevelError, kLogCategory, message, false);
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
  if (state_ == nullptr || state_->context == nullptr ||
      state_->swapChain == nullptr || state_->rtv0 == nullptr ||
      width_ <= 0 || height_ <= 0) {
    return E_UNEXPECTED;
  }

  const HRESULT pipelineHr = CreateDrawPipeline();
  if (FAILED(pipelineHr)) {
    return pipelineHr;
  }

  ID3D11RenderTargetView* nativeRtv = state_->rtv0.Get();
  currentRtv = nativeRtv;
  state_->context->OMSetRenderTargets(1, &nativeRtv, nullptr);

  D3D11_VIEWPORT viewport = {};
  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = 0.0f;
  viewport.Width = static_cast<float>(width_);
  viewport.Height = static_cast<float>(height_);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  state_->context->RSSetViewports(1, &viewport);
  return S_OK;
}

HRESULT D3D11SwapChainRenderer::SubmitCompiledBatches(
    const LayerPacket& layer, int layerIndex, void* currentRtv,
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
    const bool hasBatch = state_->batchCompiler.TryGetNextBatch(batch, batchHr);
    const auto compileEnd = std::chrono::steady_clock::now();
    diagnostics.compileMs += DurationMs(compileStart, compileEnd);
    AccumulateCompileStats(diagnostics, state_->batchCompiler.lastBatchStats());
    if (!hasBatch) {
      break;
    }

    ++batchIndex;
    switch (batch.kind) {
    case batch::BatchKind::Clear:
      ++diagnostics.clearBatchCount;
      state_->context->ClearRenderTargetView(nativeRtv, batch.clearColor);
      break;

    case batch::BatchKind::Triangles: {
      ++diagnostics.triangleBatchCount;
      diagnostics.maxTriangleBatchVertexCount =
          (std::max)(diagnostics.maxTriangleBatchVertexCount,
                     batch.triangleVertexCount);
      draw::TriangleBatchDrawContext triangleContext{};
      triangleContext.context = state_->context;
      triangleContext.inputLayout = state_->inputLayout;
      triangleContext.vertexShader = state_->vertexShader;
      triangleContext.pixelShader = state_->pixelShader;
      triangleContext.blendState = state_->blendState;
      triangleContext.rasterizerState = state_->rasterizerState;
      triangleContext.vertexBuffer = state_->dynamicVertexBuffer;
      triangleContext.vertexBufferCapacityBytes =
          state_->dynamicVertexCapacityBytes;

      const draw::TriangleVertexData vertexData{batch.triangleVertices,
                                                batch.triangleVertexCount};
      draw::TriangleBatchDrawStats triangleStats{};
      const auto triangleStart = std::chrono::steady_clock::now();
      const HRESULT drawHr = draw::DrawTriangleBatch(triangleContext, vertexData,
                                                     &triangleStats);
      const auto triangleEnd = std::chrono::steady_clock::now();
      diagnostics.triangleCpuMs += DurationMs(triangleStart, triangleEnd);
      diagnostics.triangleEnsureVertexBufferMs +=
          triangleStats.ensureVertexBufferMs;
      diagnostics.triangleUploadMs += triangleStats.uploadVertexDataMs;
      diagnostics.triangleDrawCallMs += triangleStats.issueDrawMs;
      diagnostics.vertexBytesUploaded += triangleStats.uploadedBytes;
      diagnostics.maxVertexBufferCapacityBytes =
          (std::max)(diagnostics.maxVertexBufferCapacityBytes,
                     triangleStats.vertexBufferCapacityBytes);
      if (triangleStats.resizedVertexBuffer) {
        ++diagnostics.vertexBufferResizeCount;
      }
      state_->dynamicVertexBuffer = triangleContext.vertexBuffer;
      state_->dynamicVertexCapacityBytes =
          triangleContext.vertexBufferCapacityBytes;
      if (FAILED(drawHr)) {
        wchar_t message[512]{};
        swprintf_s(
            message,
            L"triangle batch failed renderer=0x%p layer=%d batch=%d hr=0x%08X vertices=%d uploadedBytes=%u vbBytes=%u.",
            static_cast<void*>(this), layerIndex, batchIndex,
            static_cast<unsigned int>(drawHr), batch.triangleVertexCount,
            triangleStats.uploadedBytes,
            triangleStats.vertexBufferCapacityBytes);
        FDVLOG_Log(FDVLOG_LevelError, kLogCategory, message, false);
        FDVLOG_WriteETW(FDVLOG_LevelError, kLogCategory, message, false);
        return drawHr;
      }
      break;
    }

    case batch::BatchKind::Text: {
      ++diagnostics.textBatchCount;
      diagnostics.maxTextBatchItemCount =
          (std::max)(diagnostics.maxTextBatchItemCount, batch.textItemCount);
      if (state_->textFormatCacheStore == nullptr || state_->d2dContext == nullptr ||
          state_->d2dSolidBrush == nullptr) {
        return E_UNEXPECTED;
      }

      draw::TextBatchDrawContext textContext{};
      textContext.d3dContext = state_->context;
      textContext.d2dContext = state_->d2dContext;
      textContext.solidBrush = state_->d2dSolidBrush;

      const draw::DrawTextData textData{batch.textItems, batch.textItemCount};
      draw::TextBatchDrawStats textStats{};
      const auto textStart = std::chrono::steady_clock::now();
      const HRESULT drawHr = draw::DrawTextBatch(
          textContext, *state_->textFormatCacheStore, textData, &textStats);
      const auto textEnd = std::chrono::steady_clock::now();
      diagnostics.textDrawMs += DurationMs(textStart, textEnd);
      diagnostics.textFlushMs += textStats.flushMs;
      diagnostics.textRecordMs += textStats.recordTextMs;
      diagnostics.textEndDrawMs += textStats.endDrawMs;
      if (FAILED(drawHr)) {
        wchar_t message[512]{};
        swprintf_s(
            message,
            L"text batch failed renderer=0x%p layer=%d batch=%d hr=0x%08X items=%d.",
            static_cast<void*>(this), layerIndex, batchIndex,
            static_cast<unsigned int>(drawHr), batch.textItemCount);
        FDVLOG_Log(FDVLOG_LevelError, kLogCategory, message, false);
        FDVLOG_WriteETW(FDVLOG_LevelError, kLogCategory, message, false);
        return drawHr;
      }
      break;
    }
    }
  }

  if (batchHr != S_FALSE) {
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

  for (int layerIndex = 0; layerIndex < LayeredFramePacket::kMaxLayerCount;
       ++layerIndex) {
    const auto& layer = framePacket->layers[layerIndex];
    if (layer.commandBytes <= 0 || layer.commandData == nullptr) {
      continue;
    }

    const HRESULT submitHr =
        SubmitCompiledBatches(layer, layerIndex, currentRtv, diagnostics);
    if (FAILED(submitHr)) {
      LogStageFailure(static_cast<void*>(this), frameId, L"submit_batches",
                      submitHr, diagnostics, width_, height_);
      return submitHr;
    }
  }

  if (state_ == nullptr || state_->swapChain == nullptr) {
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
  state_ = new (std::nothrow) D3D11SwapChainRendererState();
  InitializeCriticalSectionAndSpinCount(&cs_, 1000);
  csInitialized_ = true;
}

D3D11SwapChainRenderer::~D3D11SwapChainRenderer() {
  ReleaseRendererResources();
  delete state_;
  state_ = nullptr;

  if (csInitialized_) {
    DeleteCriticalSection(&cs_);
    csInitialized_ = false;
  }
}

void D3D11SwapChainRenderer::ReleaseRenderTargetResources() {
  if (state_ == nullptr) {
    return;
  }

  if (state_->d2dContext != nullptr) {
    state_->d2dContext->SetTarget(nullptr);
  }

  state_->d2dSolidBrush.Reset();
  state_->d2dTargetBitmap.Reset();
  state_->rtv0.Reset();
}

HRESULT D3D11SwapChainRenderer::EnsureD2DAndDWrite() {
  if (state_ == nullptr || state_->device == nullptr) {
    return E_UNEXPECTED;
  }

  if (state_->d2dFactory != nullptr && state_->d2dDevice != nullptr &&
      state_->d2dContext != nullptr && state_->dwriteFactory != nullptr) {
    return S_OK;
  }

  if (state_->d2dFactory == nullptr) {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   __uuidof(ID2D1Factory1), nullptr,
                                   reinterpret_cast<void**>(
                                       state_->d2dFactory.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || state_->d2dFactory == nullptr) {
      return FAILED(hr) ? hr : E_FAIL;
    }
  }

  if (state_->dwriteFactory == nullptr) {
    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(state_->dwriteFactory.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || state_->dwriteFactory == nullptr) {
      return FAILED(hr) ? hr : E_FAIL;
    }
  }

  if (state_->d2dDevice == nullptr || state_->d2dContext == nullptr) {
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = state_->device->QueryInterface(__uuidof(IDXGIDevice),
                                         reinterpret_cast<void**>(
                                             dxgiDevice.GetAddressOf()));
    if (FAILED(hr) || dxgiDevice == nullptr) {
      return FAILED(hr) ? hr : E_FAIL;
    }

    if (state_->d2dDevice == nullptr) {
      hr = state_->d2dFactory->CreateDevice(
          dxgiDevice.Get(), state_->d2dDevice.ReleaseAndGetAddressOf());
      if (FAILED(hr) || state_->d2dDevice == nullptr) {
        return FAILED(hr) ? hr : E_FAIL;
      }
    }

    if (state_->d2dContext == nullptr) {
      hr = state_->d2dDevice->CreateDeviceContext(
          D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
          state_->d2dContext.ReleaseAndGetAddressOf());
      if (FAILED(hr) || state_->d2dContext == nullptr) {
        return FAILED(hr) ? hr : E_FAIL;
      }
    }
  }

  if (state_->textFormatCacheStore == nullptr) {
    state_->textFormatCacheStore =
        std::make_unique<TextFormatCacheStore>(state_->dwriteFactory.Get());
  }

  return S_OK;
}

HRESULT D3D11SwapChainRenderer::CreateRenderTarget() {
  if (state_ == nullptr || state_->device == nullptr ||
      state_->swapChain == nullptr) {
    return E_UNEXPECTED;
  }

  const HRESULT ensureHr = EnsureD2DAndDWrite();
  if (FAILED(ensureHr)) {
    return ensureHr;
  }

  ReleaseRenderTargetResources();

  ComPtr<ID3D11Texture2D> backBuffer;
  HRESULT hr = state_->swapChain->GetBuffer(
      0, __uuidof(ID3D11Texture2D),
      reinterpret_cast<void**>(backBuffer.GetAddressOf()));
  if (FAILED(hr) || backBuffer == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  hr = state_->device->CreateRenderTargetView(
      backBuffer.Get(), nullptr, state_->rtv0.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state_->rtv0 == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  ComPtr<IDXGISurface> dxgiSurface;
  hr = backBuffer->QueryInterface(__uuidof(IDXGISurface),
                                  reinterpret_cast<void**>(
                                      dxgiSurface.GetAddressOf()));
  if (FAILED(hr) || dxgiSurface == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  D2D1_BITMAP_PROPERTIES1 bitmapProps = {};
  bitmapProps.pixelFormat.format = kSwapChainFormat;
  bitmapProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
  bitmapProps.dpiX = 96.0f;
  bitmapProps.dpiY = 96.0f;
  bitmapProps.bitmapOptions =
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

  hr = state_->d2dContext->CreateBitmapFromDxgiSurface(
      dxgiSurface.Get(), &bitmapProps,
      state_->d2dTargetBitmap.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state_->d2dTargetBitmap == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  state_->d2dContext->SetTarget(state_->d2dTargetBitmap.Get());
  state_->d2dContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

  D2D1_COLOR_F white = {1.0f, 1.0f, 1.0f, 1.0f};
  hr = state_->d2dContext->CreateSolidColorBrush(
      white, state_->d2dSolidBrush.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state_->d2dSolidBrush == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  return S_OK;
}

HRESULT D3D11SwapChainRenderer::EnsureFactory() {
  if (state_ == nullptr || state_->device == nullptr) {
    return E_UNEXPECTED;
  }

  if (state_->dxgiFactory != nullptr) {
    return S_OK;
  }

  ComPtr<IDXGIDevice> dxgiDevice;
  ComPtr<IDXGIAdapter> adapter;

  HRESULT hr = state_->device->QueryInterface(__uuidof(IDXGIDevice),
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
                              state_->dxgiFactory.ReleaseAndGetAddressOf()));
  if (FAILED(hr) || state_->dxgiFactory == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  return S_OK;
}

HRESULT D3D11SwapChainRenderer::CreateSwapChain() {
  if (state_ == nullptr || state_->device == nullptr ||
      state_->dxgiFactory == nullptr) {
    return E_UNEXPECTED;
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

  HRESULT hr = state_->dxgiFactory->CreateSwapChainForComposition(
      state_->device.Get(), &swapDesc, nullptr,
      state_->swapChain.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state_->swapChain == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  return S_OK;
}

HRESULT D3D11SwapChainRenderer::CreateDrawPipeline() {
  if (state_ == nullptr || state_->device == nullptr) {
    return E_UNEXPECTED;
  }

  if (state_->vertexShader != nullptr && state_->pixelShader != nullptr &&
      state_->inputLayout != nullptr && state_->blendState != nullptr &&
      state_->rasterizerState != nullptr) {
    return S_OK;
  }

  ComPtr<ID3DBlob> vsBlob;
  ComPtr<ID3DBlob> psBlob;
  HRESULT hr = LoadShaderBlob(kTriangleVertexShaderObject, vsBlob);
  if (FAILED(hr)) {
    return hr;
  }

  hr = LoadShaderBlob(kTrianglePixelShaderObject, psBlob);
  if (FAILED(hr)) {
    return hr;
  }

  hr = state_->device->CreateVertexShader(
      vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
      state_->vertexShader.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state_->vertexShader == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  hr = state_->device->CreatePixelShader(
      psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
      state_->pixelShader.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state_->pixelShader == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  D3D11_INPUT_ELEMENT_DESC inputLayout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  hr = state_->device->CreateInputLayout(
      inputLayout, ARRAYSIZE(inputLayout), vsBlob->GetBufferPointer(),
      vsBlob->GetBufferSize(), state_->inputLayout.ReleaseAndGetAddressOf());
  if (FAILED(hr) || state_->inputLayout == nullptr) {
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

  return S_OK;
}

void D3D11SwapChainRenderer::ReleaseRendererResources() {
  if (state_ == nullptr) {
    return;
  }

  state_->textFormatCacheStore.reset();
  state_->batchCompiler = batch::BatchCompiler();

  ReleaseRenderTargetResources();
  state_->dynamicVertexBuffer.Reset();
  state_->dynamicVertexCapacityBytes = 0;
  state_->rasterizerState.Reset();
  state_->blendState.Reset();
  state_->inputLayout.Reset();
  state_->pixelShader.Reset();
  state_->vertexShader.Reset();
  state_->swapChain.Reset();
  state_->dxgiFactory.Reset();
  state_->d2dContext.Reset();
  state_->d2dDevice.Reset();
  state_->d2dFactory.Reset();
  state_->dwriteFactory.Reset();
  state_->context.Reset();
  state_->device.Reset();
}

HRESULT D3D11SwapChainRenderer::Initialize() {
  RendererLockGuard lock(&cs_);
  return CreateDeviceAndSwapChain();
}

HRESULT D3D11SwapChainRenderer::CreateDeviceAndSwapChain() {
  if (width_ <= 0 || height_ <= 0) {
    return E_INVALIDARG;
  }

  if (state_ == nullptr) {
    return E_OUTOFMEMORY;
  }

  if (state_->device != nullptr && state_->swapChain != nullptr &&
      state_->rtv0 != nullptr) {
    return S_OK;
  }

  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 kCreationFlags, nullptr, 0, D3D11_SDK_VERSION,
                                 state_->device.ReleaseAndGetAddressOf(), nullptr,
                                 state_->context.ReleaseAndGetAddressOf());
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                           kCreationFlags, nullptr, 0, D3D11_SDK_VERSION,
                           state_->device.ReleaseAndGetAddressOf(), nullptr,
                           state_->context.ReleaseAndGetAddressOf());
  }

  if (FAILED(hr) || state_->device == nullptr || state_->context == nullptr) {
    ReleaseRendererResources();
    return FAILED(hr) ? hr : E_FAIL;
  }

  const HRESULT ensureFactoryHr = EnsureFactory();
  if (FAILED(ensureFactoryHr)) {
    ReleaseRendererResources();
    return ensureFactoryHr;
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

  const HRESULT createPipelineHr = CreateDrawPipeline();
  if (FAILED(createPipelineHr)) {
    ReleaseRendererResources();
    return createPipelineHr;
  }

  return S_OK;
}

HRESULT D3D11SwapChainRenderer::Resize(int width, int height) {
  RendererLockGuard lock(&cs_);
  return ResizeSwapChain(width, height);
}

HRESULT D3D11SwapChainRenderer::ResizeSwapChain(int width, int height) {
  if (state_ == nullptr || state_->swapChain == nullptr ||
      state_->device == nullptr || state_->context == nullptr) {
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
  if (state_ == nullptr || state_->swapChain == nullptr) {
    return E_UNEXPECTED;
  }

  *outSwapChain = state_->swapChain.Get();
  return S_OK;
}

} // namespace fdv::d3d11
