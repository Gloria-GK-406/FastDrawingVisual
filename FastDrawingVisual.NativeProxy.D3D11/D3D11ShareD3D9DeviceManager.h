#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

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

class D3D11ShareD3D9DeviceManager final {
 public:
  static D3D11ShareD3D9DeviceManager& Instance();

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
  ComPtr<IDirect3D9Ex> GetD3D9() const;
  ComPtr<IDirect3DDevice9Ex> GetD3D9Device() const;
  HWND GetDeviceHwnd() const;

  void LockExecution();
  void UnlockExecution();

 private:
  struct FrameTaskQueueEntry;

  D3D11ShareD3D9DeviceManager();
  ~D3D11ShareD3D9DeviceManager();

  D3D11ShareD3D9DeviceManager(const D3D11ShareD3D9DeviceManager&) = delete;
  D3D11ShareD3D9DeviceManager& operator=(const D3D11ShareD3D9DeviceManager&) =
      delete;

  HRESULT EnsureHiddenWindowUnlocked();
  HRESULT CreateSharedInteropDevices();
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
  ComPtr<IDirect3D9Ex> d3d9_ = nullptr;
  ComPtr<IDirect3DDevice9Ex> d3d9Device_ = nullptr;
  HWND hiddenHwnd_ = nullptr;
  HANDLE queueSemaphore_ = nullptr;
  HANDLE stopEvent_ = nullptr;
  HANDLE workerThread_ = nullptr;
  std::vector<std::unique_ptr<FrameTaskQueueEntry>> queue_;
};

} // namespace fdv::d3d11
