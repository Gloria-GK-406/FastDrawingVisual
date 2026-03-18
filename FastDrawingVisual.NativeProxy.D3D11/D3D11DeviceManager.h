#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include <d3d9.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "D3DTaskType.h"

namespace fdv::textd2d {
class D2DTextRenderer;
} // namespace fdv::textd2d

namespace fdv::d3d11 {

using Microsoft::WRL::ComPtr;

class D3D11DeviceManager final {
 public:
  static D3D11DeviceManager& Instance();

  HRESULT EnsureReady();
  void RegisterClient();
  void ReleaseClient();
  void Invalidate();
  HRESULT CreateDynamicInstanceBuffer(UINT requiredBytes,
                                      ComPtr<ID3D11Buffer>& bufferOut,
                                      UINT& capacityBytesOut);
  HRESULT SubmitFrame(D3D11FrameTask&& task);

  std::uint64_t generation() const;
  ComPtr<ID3D11Device> GetDevice() const;
  ComPtr<ID3D11DeviceContext> GetImmediateContext() const;

  void LockExecution();
  void UnlockExecution();

 private:
  D3D11DeviceManager();
  ~D3D11DeviceManager();

  D3D11DeviceManager(const D3D11DeviceManager&) = delete;
  D3D11DeviceManager& operator=(const D3D11DeviceManager&) = delete;

  struct FrameTaskQueueEntry;

  HRESULT CreateSharedDevice();
  HRESULT EnsurePipelineResourcesUnlocked();
  HRESULT EnsureWorkerReady();
  HRESULT ExecuteFrameTask(const D3D11FrameTask& task,
                           D3DFrameTaskResult& result);
  static DWORD WINAPI WorkerThreadProc(LPVOID parameter);
  void StopWorker();
  void WorkerLoop();
  void ResetUnlocked();
  void FailQueuedTasksUnlocked(HRESULT hr);

 private:
  mutable CRITICAL_SECTION stateCs_{};
  CRITICAL_SECTION executionCs_{};
  CRITICAL_SECTION queueCs_{};
  bool stateCsInitialized_ = false;
  bool executionCsInitialized_ = false;
  bool queueCsInitialized_ = false;
  std::uint32_t clientCount_ = 0;
  std::uint64_t generation_ = 0;
  ComPtr<ID3D11Device> device_ = nullptr;
  ComPtr<ID3D11DeviceContext> context_ = nullptr;
  ComPtr<ID3D11VertexShader> instanceVertexShader_ = nullptr;
  ComPtr<ID3D11PixelShader> instancePixelShader_ = nullptr;
  ComPtr<ID3D11InputLayout> instanceInputLayout_ = nullptr;
  ComPtr<ID3D11BlendState> blendState_ = nullptr;
  ComPtr<ID3D11RasterizerState> rasterizerState_ = nullptr;
  ComPtr<ID3D11Buffer> unitQuadVertexBuffer_ = nullptr;
  ComPtr<ID3D11Buffer> viewConstantsBuffer_ = nullptr;
  std::unique_ptr<fdv::textd2d::D2DTextRenderer> textRenderer_;
  HANDLE queueSemaphore_ = nullptr;
  HANDLE stopEvent_ = nullptr;
  HANDLE workerThread_ = nullptr;
  std::vector<std::unique_ptr<FrameTaskQueueEntry>> queue_;
};

class SharedD3D11D3D9InteropManager final {
 public:
  static SharedD3D11D3D9InteropManager& Instance();

  HRESULT EnsureReady();
  void Invalidate();

  std::uint64_t generation() const;
  ComPtr<ID3D11Device> GetDevice() const;
  ComPtr<ID3D11DeviceContext> GetImmediateContext() const;
  ComPtr<IDirect3D9Ex> GetD3D9() const;
  ComPtr<IDirect3DDevice9Ex> GetD3D9Device() const;
  HWND GetDeviceHwnd() const;

  void LockExecution();
  void UnlockExecution();

 private:
  SharedD3D11D3D9InteropManager();
  ~SharedD3D11D3D9InteropManager();

  SharedD3D11D3D9InteropManager(const SharedD3D11D3D9InteropManager&) = delete;
  SharedD3D11D3D9InteropManager& operator=(const SharedD3D11D3D9InteropManager&) = delete;

  HRESULT EnsureHiddenWindowUnlocked();
  HRESULT CreateSharedInteropDevices();
  void ResetUnlocked();

 private:
  mutable CRITICAL_SECTION stateCs_{};
  CRITICAL_SECTION executionCs_{};
  bool stateCsInitialized_ = false;
  bool executionCsInitialized_ = false;
  std::uint64_t generation_ = 0;
  ComPtr<ID3D11Device> device_ = nullptr;
  ComPtr<ID3D11DeviceContext> context_ = nullptr;
  ComPtr<IDirect3D9Ex> d3d9_ = nullptr;
  ComPtr<IDirect3DDevice9Ex> d3d9Device_ = nullptr;
  HWND hiddenHwnd_ = nullptr;
};

template <typename TManager>
class ExecutionLockGuard final {
 public:
  explicit ExecutionLockGuard(TManager& manager) : manager_(manager) {
    const auto waitStart = std::chrono::steady_clock::now();
    manager_.LockExecution();
    const auto waitEnd = std::chrono::steady_clock::now();
    waitDurationMs_ =
        std::chrono::duration<double, std::milli>(waitEnd - waitStart).count();
  }

  ~ExecutionLockGuard() { manager_.UnlockExecution(); }

  ExecutionLockGuard(const ExecutionLockGuard&) = delete;
  ExecutionLockGuard& operator=(const ExecutionLockGuard&) = delete;

  double waitDurationMs() const { return waitDurationMs_; }

 private:
  TManager& manager_;
  double waitDurationMs_ = 0.0;
};

} // namespace fdv::d3d11
