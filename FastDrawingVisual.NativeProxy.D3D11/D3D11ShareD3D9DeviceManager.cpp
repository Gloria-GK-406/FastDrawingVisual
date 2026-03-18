#include "D3D11ShareD3D9DeviceManager.h"

#include "D3D11DeviceManager.h"
#include "D3D11DeviceManagerShared.h"
#include "../FastDrawingVisual.NativeProxy.TextD2D/D2DTextRenderer.h"

#include <algorithm>
#include <chrono>

#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace fdv::d3d11 {

D3D11ShareD3D9DeviceManager& D3D11ShareD3D9DeviceManager::Instance() {
  static D3D11ShareD3D9DeviceManager instance;
  return instance;
}

D3D11ShareD3D9DeviceManager::D3D11ShareD3D9DeviceManager() {
  InitializeCriticalSectionAndSpinCount(&stateCs_, 1000);
  InitializeCriticalSectionAndSpinCount(&executionCs_, 1000);
  InitializeCriticalSectionAndSpinCount(&queueCs_, 1000);
  stateCsInitialized_ = true;
  executionCsInitialized_ = true;
  queueCsInitialized_ = true;
}

void D3D11ShareD3D9DeviceManager::StopWorker() {
  HANDLE workerThread = nullptr;
  HANDLE stopEvent = nullptr;
  HANDLE queueSemaphore = nullptr;

  EnterCriticalSection(&stateCs_);
  workerThread = workerThread_;
  stopEvent = stopEvent_;
  queueSemaphore = queueSemaphore_;
  LeaveCriticalSection(&stateCs_);

  if (stopEvent != nullptr) {
    SetEvent(stopEvent);
  }
  if (queueSemaphore != nullptr) {
    ReleaseSemaphore(queueSemaphore, 1, nullptr);
  }
  if (workerThread != nullptr) {
    WaitForSingleObject(workerThread, INFINITE);
  }

  EnterCriticalSection(&stateCs_);
  if (workerThread_ != nullptr) {
    CloseHandle(workerThread_);
    workerThread_ = nullptr;
  }
  if (stopEvent_ != nullptr) {
    CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
  }
  if (queueSemaphore_ != nullptr) {
    CloseHandle(queueSemaphore_);
    queueSemaphore_ = nullptr;
  }
  LeaveCriticalSection(&stateCs_);
}

D3D11ShareD3D9DeviceManager::~D3D11ShareD3D9DeviceManager() {
  StopWorker();
  EnterCriticalSection(&stateCs_);
  ResetUnlocked();
  LeaveCriticalSection(&stateCs_);

  if (queueCsInitialized_) {
    DeleteCriticalSection(&queueCs_);
    queueCsInitialized_ = false;
  }
  if (executionCsInitialized_) {
    DeleteCriticalSection(&executionCs_);
    executionCsInitialized_ = false;
  }
  if (stateCsInitialized_) {
    DeleteCriticalSection(&stateCs_);
    stateCsInitialized_ = false;
  }
}

HRESULT D3D11ShareD3D9DeviceManager::EnsureHiddenWindowUnlocked() {
  if (hiddenHwnd_ != nullptr) {
    return S_OK;
  }

  const HRESULT classHr = EnsureHiddenWindowClassRegistered();
  if (FAILED(classHr)) {
    return classHr;
  }

  hiddenHwnd_ = CreateWindowExW(0, kHiddenWindowClassName,
                                L"FastDrawingVisual.D3D11.SharedDeviceWindow",
                                WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
  if (hiddenHwnd_ == nullptr) {
    return HRESULT_FROM_WIN32(GetLastError());
  }

  return S_OK;
}

HRESULT D3D11ShareD3D9DeviceManager::CreateSharedInteropDevices() {
  HRESULT hr = EnsureHiddenWindowUnlocked();
  if (FAILED(hr)) {
    return hr;
  }

  hr = CreateHardwareD3D11Device(device_, context_, false, true);
  if (FAILED(hr)) {
    return hr;
  }

  hr = Direct3DCreate9Ex(D3D_SDK_VERSION, d3d9_.ReleaseAndGetAddressOf());
  if (FAILED(hr) || d3d9_ == nullptr) {
    ResetUnlocked();
    return FAILED(hr) ? hr : E_FAIL;
  }

  D3DPRESENT_PARAMETERS parameters = {};
  parameters.Windowed = TRUE;
  parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
  parameters.BackBufferFormat = D3DFMT_UNKNOWN;
  parameters.BackBufferWidth = 1;
  parameters.BackBufferHeight = 1;
  parameters.BackBufferCount = 1;
  parameters.hDeviceWindow = hiddenHwnd_;
  parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  parameters.EnableAutoDepthStencil = FALSE;

  hr = d3d9_->CreateDeviceEx(
      D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hiddenHwnd_,
      D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED |
          D3DCREATE_FPU_PRESERVE,
      &parameters, nullptr, d3d9Device_.ReleaseAndGetAddressOf());
  if (FAILED(hr)) {
    hr = d3d9_->CreateDeviceEx(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hiddenHwnd_,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED |
            D3DCREATE_FPU_PRESERVE,
        &parameters, nullptr, d3d9Device_.ReleaseAndGetAddressOf());
  }

  if (FAILED(hr) || d3d9Device_ == nullptr) {
    ResetUnlocked();
    return FAILED(hr) ? hr : E_FAIL;
  }

  return S_OK;
}

HRESULT D3D11ShareD3D9DeviceManager::EnsurePipelineResourcesUnlocked() {
  if (device_ == nullptr) {
    return E_UNEXPECTED;
  }

  if (instanceVertexShader_ != nullptr && instancePixelShader_ != nullptr &&
      instanceInputLayout_ != nullptr && unitQuadVertexBuffer_ != nullptr &&
      viewConstantsBuffer_ != nullptr && blendState_ != nullptr &&
      rasterizerState_ != nullptr) {
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

  hr = device_->CreateVertexShader(
      instanceVsBlob->GetBufferPointer(), instanceVsBlob->GetBufferSize(),
      nullptr, instanceVertexShader_.ReleaseAndGetAddressOf());
  if (FAILED(hr) || instanceVertexShader_ == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  hr = device_->CreatePixelShader(
      instancePsBlob->GetBufferPointer(), instancePsBlob->GetBufferSize(),
      nullptr, instancePixelShader_.ReleaseAndGetAddressOf());
  if (FAILED(hr) || instancePixelShader_ == nullptr) {
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
  hr = device_->CreateInputLayout(
      instanceInputLayout, ARRAYSIZE(instanceInputLayout),
      instanceVsBlob->GetBufferPointer(), instanceVsBlob->GetBufferSize(),
      instanceInputLayout_.ReleaseAndGetAddressOf());
  if (FAILED(hr) || instanceInputLayout_ == nullptr) {
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

  hr = device_->CreateBlendState(&blendDesc, blendState_.ReleaseAndGetAddressOf());
  if (FAILED(hr) || blendState_ == nullptr) {
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

  hr = device_->CreateRasterizerState(&rsDesc, rasterizerState_.ReleaseAndGetAddressOf());
  if (FAILED(hr) || rasterizerState_ == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  hr = EnsureDynamicBuffer(device_.Get(), 16u, D3D11_BIND_CONSTANT_BUFFER,
                           viewConstantsBuffer_);
  if (FAILED(hr)) {
    return hr;
  }

  hr = EnsureUnitQuadVertexBuffer(device_.Get(), unitQuadVertexBuffer_);
  if (FAILED(hr)) {
    return hr;
  }

  return S_OK;
}

HRESULT D3D11ShareD3D9DeviceManager::EnsureWorkerReady() {
  if (queueSemaphore_ != nullptr && stopEvent_ != nullptr &&
      workerThread_ != nullptr) {
    return S_OK;
  }

  if (queueSemaphore_ == nullptr) {
    queueSemaphore_ = CreateSemaphoreW(nullptr, 0, LONG_MAX, nullptr);
    if (queueSemaphore_ == nullptr) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
  }

  if (stopEvent_ == nullptr) {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent_ == nullptr) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
  }

  if (workerThread_ == nullptr) {
    DWORD threadId = 0;
    workerThread_ = CreateThread(nullptr, 0, &WorkerThreadProc, this, 0,
                                 &threadId);
    if (workerThread_ == nullptr) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
  }

  return S_OK;
}

void D3D11ShareD3D9DeviceManager::ResetUnlocked() {
  viewConstantsBuffer_.Reset();
  unitQuadVertexBuffer_.Reset();
  rasterizerState_.Reset();
  blendState_.Reset();
  instanceInputLayout_.Reset();
  instancePixelShader_.Reset();
  instanceVertexShader_.Reset();
  d3d9Device_.Reset();
  d3d9_.Reset();
  context_.Reset();
  device_.Reset();
}

HRESULT D3D11ShareD3D9DeviceManager::EnsureReady() {
  EnterCriticalSection(&stateCs_);
  if (device_ != nullptr && context_ != nullptr && d3d9_ != nullptr &&
      d3d9Device_ != nullptr && hiddenHwnd_ != nullptr) {
    const HRESULT pipelineHr = EnsurePipelineResourcesUnlocked();
    const HRESULT workerHr = EnsureWorkerReady();
    LeaveCriticalSection(&stateCs_);
    if (FAILED(pipelineHr)) {
      return pipelineHr;
    }
    return workerHr;
  }

  ResetUnlocked();
  const HRESULT hr = CreateSharedInteropDevices();
  HRESULT pipelineHr = S_OK;
  HRESULT workerHr = S_OK;
  if (SUCCEEDED(hr)) {
    ++generation_;
    pipelineHr = EnsurePipelineResourcesUnlocked();
    workerHr = EnsureWorkerReady();
  }
  LeaveCriticalSection(&stateCs_);
  if (FAILED(hr)) {
    return hr;
  }
  if (FAILED(pipelineHr)) {
    return pipelineHr;
  }
  return workerHr;
}

void D3D11ShareD3D9DeviceManager::RegisterClient() {
  EnterCriticalSection(&stateCs_);
  ++clientCount_;
  LeaveCriticalSection(&stateCs_);
}

void D3D11ShareD3D9DeviceManager::ReleaseClient() {
  bool shouldReset = false;

  EnterCriticalSection(&stateCs_);
  if (clientCount_ > 0) {
    --clientCount_;
    shouldReset = (clientCount_ == 0);
  }
  LeaveCriticalSection(&stateCs_);

  if (!shouldReset) {
    return;
  }

  StopWorker();
  EnterCriticalSection(&stateCs_);
  ResetUnlocked();
  LeaveCriticalSection(&stateCs_);
}

void D3D11ShareD3D9DeviceManager::Invalidate() {
  StopWorker();
  EnterCriticalSection(&stateCs_);
  ResetUnlocked();
  LeaveCriticalSection(&stateCs_);
}

std::uint64_t D3D11ShareD3D9DeviceManager::generation() const {
  EnterCriticalSection(&stateCs_);
  const std::uint64_t generation = generation_;
  LeaveCriticalSection(&stateCs_);
  return generation;
}

ComPtr<ID3D11Device> D3D11ShareD3D9DeviceManager::GetDevice() const {
  EnterCriticalSection(&stateCs_);
  ComPtr<ID3D11Device> device = device_;
  LeaveCriticalSection(&stateCs_);
  return device;
}

ComPtr<ID3D11DeviceContext>
D3D11ShareD3D9DeviceManager::GetImmediateContext() const {
  EnterCriticalSection(&stateCs_);
  ComPtr<ID3D11DeviceContext> context = context_;
  LeaveCriticalSection(&stateCs_);
  return context;
}

ComPtr<IDirect3D9Ex> D3D11ShareD3D9DeviceManager::GetD3D9() const {
  EnterCriticalSection(&stateCs_);
  ComPtr<IDirect3D9Ex> d3d9 = d3d9_;
  LeaveCriticalSection(&stateCs_);
  return d3d9;
}

ComPtr<IDirect3DDevice9Ex>
D3D11ShareD3D9DeviceManager::GetD3D9Device() const {
  EnterCriticalSection(&stateCs_);
  ComPtr<IDirect3DDevice9Ex> d3d9Device = d3d9Device_;
  LeaveCriticalSection(&stateCs_);
  return d3d9Device;
}

HWND D3D11ShareD3D9DeviceManager::GetDeviceHwnd() const {
  EnterCriticalSection(&stateCs_);
  const HWND hwnd = hiddenHwnd_;
  LeaveCriticalSection(&stateCs_);
  return hwnd;
}

void D3D11ShareD3D9DeviceManager::LockExecution() {
  EnterCriticalSection(&executionCs_);
}

void D3D11ShareD3D9DeviceManager::UnlockExecution() {
  LeaveCriticalSection(&executionCs_);
}

HRESULT D3D11ShareD3D9DeviceManager::CreateDynamicInstanceBuffer(
    UINT requiredBytes, ComPtr<ID3D11Buffer>& bufferOut,
    UINT& capacityBytesOut) {
  bufferOut.Reset();
  capacityBytesOut = 0;

  if (requiredBytes == 0) {
    return S_OK;
  }

  const HRESULT readyHr = EnsureReady();
  if (FAILED(readyHr)) {
    return readyHr;
  }

  const UINT minBytes = (std::max)(requiredBytes, 1024u);
  D3D11_BUFFER_DESC bufferDesc = {};
  bufferDesc.ByteWidth = minBytes;
  bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
  bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  EnterCriticalSection(&stateCs_);
  const HRESULT hr =
      device_->CreateBuffer(&bufferDesc, nullptr, bufferOut.GetAddressOf());
  LeaveCriticalSection(&stateCs_);
  if (FAILED(hr) || bufferOut == nullptr) {
    bufferOut.Reset();
    return FAILED(hr) ? hr : E_FAIL;
  }

  capacityBytesOut = minBytes;
  return S_OK;
}

struct D3D11ShareD3D9DeviceManager::FrameTaskQueueEntry {
  D3D11FrameTask task;
};

HRESULT D3D11ShareD3D9DeviceManager::SubmitFrame(D3D11FrameTask&& task) {
  const HRESULT readyHr = EnsureReady();
  if (FAILED(readyHr)) {
    return readyHr;
  }

  if (task.renderTargetView == nullptr || task.completion == nullptr) {
    return E_INVALIDARG;
  }

  auto entry = std::make_unique<FrameTaskQueueEntry>();
  if (entry == nullptr) {
    return E_OUTOFMEMORY;
  }

  FrameTaskQueueEntry* queuedEntry = entry.get();
  entry->task = std::move(task);

  EnterCriticalSection(&queueCs_);
  queue_.push_back(std::move(entry));
  LeaveCriticalSection(&queueCs_);

  if (!ReleaseSemaphore(queueSemaphore_, 1, nullptr)) {
    const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
    EnterCriticalSection(&queueCs_);
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
      if (it->get() == queuedEntry) {
        auto failedEntry = std::move(*it);
        queue_.erase(it);
        if (failedEntry != nullptr && failedEntry->task.completion != nullptr) {
          auto completion = failedEntry->task.completion;
          std::lock_guard<std::mutex> lock(completion->mutex);
          if (completion->resultSink != nullptr) {
            completion->resultSink->hr = hr;
          }
          completion->completed = true;
          completion->condition.notify_all();
        }
        break;
      }
    }
    LeaveCriticalSection(&queueCs_);
    return hr;
  }

  return S_OK;
}

DWORD WINAPI D3D11ShareD3D9DeviceManager::WorkerThreadProc(LPVOID parameter) {
  auto* manager = static_cast<D3D11ShareD3D9DeviceManager*>(parameter);
  if (manager == nullptr) {
    return 0;
  }

  manager->WorkerLoop();
  return 0;
}

void D3D11ShareD3D9DeviceManager::FailQueuedTasksUnlocked(HRESULT hr) {
  for (auto& entry : queue_) {
    if (entry == nullptr || entry->task.completion == nullptr) {
      continue;
    }
    auto completion = entry->task.completion;
    std::lock_guard<std::mutex> lock(completion->mutex);
    if (completion->resultSink != nullptr) {
      completion->resultSink->hr = hr;
    }
    completion->completed = true;
    completion->condition.notify_all();
  }
  queue_.clear();
}

void D3D11ShareD3D9DeviceManager::WorkerLoop() {
  HANDLE waitHandles[2] = {stopEvent_, queueSemaphore_};

  while (true) {
    const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE,
                                                    INFINITE);
    if (waitResult == WAIT_OBJECT_0) {
      if (textRenderer_ != nullptr) {
        textRenderer_->ReleaseDeviceResources();
        textRenderer_.reset();
      }
      EnterCriticalSection(&queueCs_);
      FailQueuedTasksUnlocked(E_ABORT);
      LeaveCriticalSection(&queueCs_);
      return;
    }
    if (waitResult != WAIT_OBJECT_0 + 1) {
      continue;
    }

    std::unique_ptr<FrameTaskQueueEntry> entry;
    EnterCriticalSection(&queueCs_);
    if (!queue_.empty()) {
      entry = std::move(queue_.front());
      queue_.erase(queue_.begin());
    }
    LeaveCriticalSection(&queueCs_);
    if (entry == nullptr) {
      continue;
    }

    D3DFrameTaskResult localResult{};
    localResult.hr = ExecuteFrameTask(entry->task, localResult);
    if (entry->task.completion != nullptr) {
      auto completion = entry->task.completion;
      std::lock_guard<std::mutex> lock(completion->mutex);
      if (completion->resultSink != nullptr) {
        *(completion->resultSink) = localResult;
      }
      completion->completed = true;
      completion->condition.notify_all();
    }
  }
}

HRESULT D3D11ShareD3D9DeviceManager::ExecuteFrameTask(
    const D3D11FrameTask& task, D3DFrameTaskResult& result) {
  if (task.renderTargetView == nullptr || task.viewportWidth <= 0.0f ||
      task.viewportHeight <= 0.0f) {
    return E_INVALIDARG;
  }

  ExecutionLockGuard<D3D11ShareD3D9DeviceManager> lock(*this);
  ComPtr<ID3D11DeviceContext> context = GetImmediateContext();
  if (context == nullptr) {
    return E_UNEXPECTED;
  }

  ID3D11RenderTargetView* rtv = task.renderTargetView;
  context->OMSetRenderTargets(1, &rtv, nullptr);

  D3D11_VIEWPORT viewport = {};
  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = 0.0f;
  viewport.Width = task.viewportWidth;
  viewport.Height = task.viewportHeight;
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  context->RSSetViewports(1, &viewport);

  if (task.hasClear) {
    context->ClearRenderTargetView(task.renderTargetView, task.clearColor);
  }

  if (task.shapeInstanceCount > 0) {
    const auto shapeStart = std::chrono::steady_clock::now();
    const HRESULT shapeHr =
        DrawShapePass(context.Get(), instanceInputLayout_.Get(),
                      instanceVertexShader_.Get(), instancePixelShader_.Get(),
                      blendState_.Get(), rasterizerState_.Get(),
                      unitQuadVertexBuffer_.Get(), viewConstantsBuffer_.Get(),
                      task);
    const auto shapeEnd = std::chrono::steady_clock::now();
    result.stats.shapeDrawMs = DurationMs(shapeStart, shapeEnd);
    if (FAILED(shapeHr)) {
      return shapeHr;
    }
  }

  if (!task.imageItems.empty() || !task.textItems.empty()) {
    if (textRenderer_ == nullptr) {
      textRenderer_ = std::make_unique<fdv::textd2d::D2DTextRenderer>();
      if (textRenderer_ == nullptr) {
        return E_OUTOFMEMORY;
      }
    }

    const HRESULT ensureTextHr = textRenderer_->EnsureDeviceResources(device_.Get());
    if (FAILED(ensureTextHr)) {
      return ensureTextHr;
    }

    ComPtr<ID3D11Resource> renderTargetResource;
    task.renderTargetView->GetResource(renderTargetResource.GetAddressOf());
    if (renderTargetResource == nullptr) {
      return E_FAIL;
    }

    ComPtr<ID3D11Texture2D> renderTargetTexture;
    const HRESULT textureHr = renderTargetResource.As(&renderTargetTexture);
    if (FAILED(textureHr) || renderTargetTexture == nullptr) {
      return FAILED(textureHr) ? textureHr : E_FAIL;
    }

    const HRESULT targetHr =
        textRenderer_->CreateTargetFromTexture(renderTargetTexture.Get(),
                                               kRenderTargetFormat);
    if (FAILED(targetHr)) {
      return targetHr;
    }
  }

  if (!task.imageItems.empty()) {
    fdv::textd2d::ImageBatchDrawStats imageStats{};
    const auto imageStart = std::chrono::steady_clock::now();
    const HRESULT imageHr = textRenderer_->DrawImageBatch(
        context.Get(), task.imageItems.data(),
        static_cast<int>(task.imageItems.size()), &imageStats);
    const auto imageEnd = std::chrono::steady_clock::now();
    result.stats.imageDrawMs = DurationMs(imageStart, imageEnd);
    result.stats.imageFlushMs = imageStats.flushMs;
    result.stats.imageRecordMs = imageStats.recordImageMs;
    result.stats.imageEndDrawMs = imageStats.endDrawMs;
    if (FAILED(imageHr)) {
      return imageHr;
    }
  }

  if (!task.textItems.empty()) {
    fdv::textd2d::TextBatchDrawStats textStats{};
    const auto textStart = std::chrono::steady_clock::now();
    const HRESULT textHr = textRenderer_->DrawTextBatch(
        context.Get(), task.textItems.data(),
        static_cast<int>(task.textItems.size()), &textStats);
    const auto textEnd = std::chrono::steady_clock::now();
    result.stats.textDrawMs = DurationMs(textStart, textEnd);
    result.stats.textFlushMs = textStats.flushMs;
    result.stats.textRecordMs = textStats.recordTextMs;
    result.stats.textEndDrawMs = textStats.endDrawMs;
    if (FAILED(textHr)) {
      return textHr;
    }
  }

  if (task.renderDoneQuery != nullptr) {
    context->End(task.renderDoneQuery);
  }

  context->Flush();
  return S_OK;
}

} // namespace fdv::d3d11
