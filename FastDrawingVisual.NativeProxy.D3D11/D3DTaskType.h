#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <memory>
#include <utility>
#include <vector>

#include <d3d11.h>

#include "../FastDrawingVisual.NativeProxy.Shared/D3DFrameTaskTypes.h"
#include "D3DBatchTypes.h"

namespace fdv::d3d11 {

using D3DFrameTaskCompletion = fdv::nativeproxy::shared::D3DFrameTaskCompletion;
using D3DFrameExecuteStats = fdv::nativeproxy::shared::D3DFrameExecuteStats;
using D3DFrameTaskResult = fdv::nativeproxy::shared::D3DFrameTaskResult;

struct D3D11FrameTask {
  D3D11FrameTask() = default;
  ~D3D11FrameTask() { Reset(); }

  D3D11FrameTask(const D3D11FrameTask&) = delete;
  D3D11FrameTask& operator=(const D3D11FrameTask&) = delete;

  D3D11FrameTask(D3D11FrameTask&& other) noexcept { MoveFrom(other); }

  D3D11FrameTask& operator=(D3D11FrameTask&& other) noexcept {
    if (this != &other) {
      Reset();
      MoveFrom(other);
    }
    return *this;
  }

  void Reset() {
    ReleasePtr(instanceBuffer);
    ReleasePtr(renderDoneQuery);
    ReleasePtr(renderTargetView);
    viewportWidth = 0.0f;
    viewportHeight = 0.0f;
    hasClear = false;
    clearColor[0] = 0.0f;
    clearColor[1] = 0.0f;
    clearColor[2] = 0.0f;
    clearColor[3] = 0.0f;
    shapeInstanceCount = 0;
    textItems.clear();
    imagePixelBlobs.clear();
    imageItems.clear();
    completion.reset();
  }

  void SetRenderTargetView(ID3D11RenderTargetView* value) {
    AssignPtr(renderTargetView, value);
  }

  void SetRenderDoneQuery(ID3D11Query* value) { AssignPtr(renderDoneQuery, value); }

  void SetInstanceBuffer(ID3D11Buffer* value) { AssignPtr(instanceBuffer, value); }

  ID3D11RenderTargetView* renderTargetView = nullptr;
  ID3D11Query* renderDoneQuery = nullptr;
  float viewportWidth = 0.0f;
  float viewportHeight = 0.0f;
  bool hasClear = false;
  float clearColor[4] = {};

  ID3D11Buffer* instanceBuffer = nullptr;
  int shapeInstanceCount = 0;
  std::vector<batch::TextBatchItem> textItems;
  std::vector<std::vector<std::uint8_t>> imagePixelBlobs;
  std::vector<batch::ImageBatchItem> imageItems;
  std::shared_ptr<D3DFrameTaskCompletion> completion;

 private:
  template <typename TInterface>
  static void AssignPtr(TInterface*& target, TInterface* value) {
    if (value != nullptr) {
      value->AddRef();
    }
    ReleasePtr(target);
    target = value;
  }

  template <typename TInterface>
  static void ReleasePtr(TInterface*& value) {
    if (value != nullptr) {
      value->Release();
      value = nullptr;
    }
  }

  void MoveFrom(D3D11FrameTask& other) {
    renderTargetView = other.renderTargetView;
    renderDoneQuery = other.renderDoneQuery;
    viewportWidth = other.viewportWidth;
    viewportHeight = other.viewportHeight;
    hasClear = other.hasClear;
    clearColor[0] = other.clearColor[0];
    clearColor[1] = other.clearColor[1];
    clearColor[2] = other.clearColor[2];
    clearColor[3] = other.clearColor[3];
    instanceBuffer = other.instanceBuffer;
    shapeInstanceCount = other.shapeInstanceCount;
    textItems = std::move(other.textItems);
    imagePixelBlobs = std::move(other.imagePixelBlobs);
    imageItems = std::move(other.imageItems);
    completion = std::move(other.completion);

    other.renderTargetView = nullptr;
    other.renderDoneQuery = nullptr;
    other.viewportWidth = 0.0f;
    other.viewportHeight = 0.0f;
    other.hasClear = false;
    other.clearColor[0] = 0.0f;
    other.clearColor[1] = 0.0f;
    other.clearColor[2] = 0.0f;
    other.clearColor[3] = 0.0f;
    other.instanceBuffer = nullptr;
    other.shapeInstanceCount = 0;
  }
};

} // namespace fdv::d3d11
