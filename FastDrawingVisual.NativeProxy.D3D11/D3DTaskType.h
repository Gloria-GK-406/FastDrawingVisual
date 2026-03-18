#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <memory>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "../FastDrawingVisual.NativeProxy.Shared/D3DFrameTaskTypes.h"
#include "D3DBatchTypes.h"

namespace fdv::d3d11 {

using Microsoft::WRL::ComPtr;
using D3DFrameTaskCompletion = fdv::nativeproxy::shared::D3DFrameTaskCompletion;
using D3DFrameExecuteStats = fdv::nativeproxy::shared::D3DFrameExecuteStats;
using D3DFrameTaskResult = fdv::nativeproxy::shared::D3DFrameTaskResult;

struct D3D11FrameTask {
  ComPtr<ID3D11RenderTargetView> renderTargetView = nullptr;
  float viewportWidth = 0.0f;
  float viewportHeight = 0.0f;
  bool hasClear = false;
  float clearColor[4] = {};

  ComPtr<ID3D11Buffer> instanceBuffer = nullptr;
  int shapeInstanceCount = 0;
  std::vector<batch::TextBatchItem> textItems;
  std::vector<batch::ImageBatchItem> imageItems;
  std::shared_ptr<D3DFrameTaskCompletion> completion;
};

} // namespace fdv::d3d11
