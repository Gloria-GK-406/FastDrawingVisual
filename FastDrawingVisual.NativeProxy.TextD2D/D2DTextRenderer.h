#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "../FastDrawingVisual.NativeProxy.Shared/BatchTypes.h"
#include "TextFormatCacheStore.h"

namespace fdv::textd2d {

using Microsoft::WRL::ComPtr;
using TextBatchItem = fdv::nativeproxy::shared::batch::TextBatchItem;

struct TextBatchDrawStats {
  double flushMs = 0.0;
  double recordTextMs = 0.0;
  double endDrawMs = 0.0;
};

class D2DTextRenderer final {
 public:
  D2DTextRenderer();
  ~D2DTextRenderer();

  D2DTextRenderer(const D2DTextRenderer&) = delete;
  D2DTextRenderer& operator=(const D2DTextRenderer&) = delete;

  HRESULT EnsureDeviceResources(ID3D11Device* device);
  HRESULT CreateTargetFromTexture(ID3D11Texture2D* targetTexture,
                                  DXGI_FORMAT format);
  HRESULT DrawTextBatch(ID3D11DeviceContext* d3dContext,
                        const TextBatchItem* items, int count,
                        TextBatchDrawStats* stats = nullptr);

  void ReleaseRenderTargetResources();
  void ReleaseDeviceResources();

 private:
  ComPtr<ID2D1Factory1> d2dFactory_ = nullptr;
  ComPtr<ID2D1Device> d2dDevice_ = nullptr;
  ComPtr<ID2D1DeviceContext> d2dContext_ = nullptr;
  ComPtr<ID2D1Bitmap1> d2dTargetBitmap_ = nullptr;
  ComPtr<ID2D1SolidColorBrush> solidBrush_ = nullptr;
  ComPtr<IDWriteFactory> dwriteFactory_ = nullptr;
  ComPtr<IDWriteRenderingParams> textRenderingParams_ = nullptr;
  ComPtr<ID3D11Device> d3dDevice_ = nullptr;
  TextFormatCacheStore textFormatCache_;
};

} // namespace fdv::textd2d
