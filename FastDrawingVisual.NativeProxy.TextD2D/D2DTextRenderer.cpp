#include "D2DTextRenderer.h"

#include <d2d1helper.h>
#include <chrono>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace fdv::textd2d {
namespace {

double DurationMs(const std::chrono::steady_clock::time_point& start,
                  const std::chrono::steady_clock::time_point& end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

D2D1_COLOR_F ToD2DColor(const fdv::protocol::ColorArgb8& color) {
  return {
      static_cast<float>(color.r) / 255.0f,
      static_cast<float>(color.g) / 255.0f,
      static_cast<float>(color.b) / 255.0f,
      static_cast<float>(color.a) / 255.0f,
  };
}

} // namespace

D2DTextRenderer::D2DTextRenderer()
    : textFormatCache_(nullptr) {}

D2DTextRenderer::~D2DTextRenderer() {
  ReleaseDeviceResources();
}

HRESULT D2DTextRenderer::EnsureDeviceResources(ID3D11Device* device) {
  if (device == nullptr) {
    return E_POINTER;
  }

  if (d3dDevice_.Get() == device && d2dFactory_ != nullptr &&
      d2dDevice_ != nullptr &&
      d2dContext_ != nullptr && dwriteFactory_ != nullptr) {
    return S_OK;
  }

  if (d3dDevice_ != nullptr && d3dDevice_.Get() != device) {
    ReleaseDeviceResources();
  }

  d3dDevice_ = device;

  if (d2dFactory_ == nullptr) {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   __uuidof(ID2D1Factory1), nullptr,
                                   reinterpret_cast<void**>(
                                       d2dFactory_.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || d2dFactory_ == nullptr) {
      return FAILED(hr) ? hr : E_FAIL;
    }
  }

  if (dwriteFactory_ == nullptr) {
    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwriteFactory_.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || dwriteFactory_ == nullptr) {
      return FAILED(hr) ? hr : E_FAIL;
    }

    textFormatCache_ = TextFormatCacheStore(dwriteFactory_.Get());
  }

  if (textRenderingParams_ == nullptr) {
    HRESULT hr = dwriteFactory_->CreateCustomRenderingParams(
        2.2f, 1.0f, 1.0f, DWRITE_PIXEL_GEOMETRY_RGB,
        DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC,
        textRenderingParams_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || textRenderingParams_ == nullptr) {
      return FAILED(hr) ? hr : E_FAIL;
    }
  }

  if (d2dDevice_ == nullptr || d2dContext_ == nullptr) {
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device->QueryInterface(
        __uuidof(IDXGIDevice),
        reinterpret_cast<void**>(dxgiDevice.GetAddressOf()));
    if (FAILED(hr) || dxgiDevice == nullptr) {
      return FAILED(hr) ? hr : E_FAIL;
    }

    if (d2dDevice_ == nullptr) {
      hr = d2dFactory_->CreateDevice(dxgiDevice.Get(),
                                     d2dDevice_.ReleaseAndGetAddressOf());
      if (FAILED(hr) || d2dDevice_ == nullptr) {
        return FAILED(hr) ? hr : E_FAIL;
      }
    }

    if (d2dContext_ == nullptr) {
      hr = d2dDevice_->CreateDeviceContext(
          D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
          d2dContext_.ReleaseAndGetAddressOf());
      if (FAILED(hr) || d2dContext_ == nullptr) {
        return FAILED(hr) ? hr : E_FAIL;
      }
    }
  }

  return S_OK;
}

HRESULT D2DTextRenderer::CreateTargetFromTexture(ID3D11Texture2D* targetTexture,
                                                 DXGI_FORMAT format) {
  if (targetTexture == nullptr) {
    return E_POINTER;
  }

  if (d2dContext_ == nullptr) {
    return E_UNEXPECTED;
  }

  ReleaseRenderTargetResources();

  ComPtr<IDXGISurface> dxgiSurface;
  HRESULT hr = targetTexture->QueryInterface(
      __uuidof(IDXGISurface),
      reinterpret_cast<void**>(dxgiSurface.GetAddressOf()));
  if (FAILED(hr) || dxgiSurface == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  D2D1_BITMAP_PROPERTIES1 bitmapProps = {};
  bitmapProps.pixelFormat.format = format;
  bitmapProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
  bitmapProps.dpiX = 96.0f;
  bitmapProps.dpiY = 96.0f;
  bitmapProps.bitmapOptions =
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

  hr = d2dContext_->CreateBitmapFromDxgiSurface(
      dxgiSurface.Get(), &bitmapProps,
      d2dTargetBitmap_.ReleaseAndGetAddressOf());
  if (FAILED(hr) || d2dTargetBitmap_ == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  d2dContext_->SetTarget(d2dTargetBitmap_.Get());
  d2dContext_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
  d2dContext_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
  d2dContext_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  d2dContext_->SetTextRenderingParams(textRenderingParams_.Get());

  D2D1_COLOR_F white = {1.0f, 1.0f, 1.0f, 1.0f};
  hr = d2dContext_->CreateSolidColorBrush(
      white, solidBrush_.ReleaseAndGetAddressOf());
  if (FAILED(hr) || solidBrush_ == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  return S_OK;
}

HRESULT D2DTextRenderer::DrawTextBatch(ID3D11DeviceContext* d3dContext,
                                       const TextBatchItem* items, int count,
                                       TextBatchDrawStats* stats) {
  if (items == nullptr || count <= 0) {
    return S_OK;
  }

  if (d3dContext == nullptr || d2dContext_ == nullptr || solidBrush_ == nullptr) {
    return E_POINTER;
  }

  const auto flushStart = std::chrono::steady_clock::now();
  d3dContext->Flush();
  const auto flushEnd = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->flushMs += DurationMs(flushStart, flushEnd);
  }

  d2dContext_->BeginDraw();
  d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());

  HRESULT result = S_OK;
  const auto recordStart = std::chrono::steady_clock::now();
  for (int i = 0; i < count; ++i) {
    const auto& item = items[i];
    if (item.text.empty()) {
      continue;
    }

    IDWriteTextFormat* textFormat =
        textFormatCache_.Acquire(item.fontFamily, item.fontSize);
    if (textFormat == nullptr) {
      result = E_FAIL;
      break;
    }

    const D2D1_RECT_F layoutRect = {
        item.layoutLeft,
        item.layoutTop,
        item.layoutRight,
        item.layoutBottom,
    };

    solidBrush_->SetColor(ToD2DColor(item.color));
    d2dContext_->DrawTextW(
        item.text.c_str(), static_cast<UINT32>(item.text.size()), textFormat,
        &layoutRect, solidBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
        DWRITE_MEASURING_MODE_GDI_NATURAL);
  }
  const auto recordEnd = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->recordTextMs += DurationMs(recordStart, recordEnd);
  }

  const auto endDrawStart = std::chrono::steady_clock::now();
  const HRESULT endDrawHr = d2dContext_->EndDraw();
  const auto endDrawEnd = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->endDrawMs += DurationMs(endDrawStart, endDrawEnd);
  }
  if (FAILED(endDrawHr)) {
    return endDrawHr;
  }

  return result;
}

void D2DTextRenderer::ReleaseRenderTargetResources() {
  if (d2dContext_ != nullptr) {
    d2dContext_->SetTarget(nullptr);
  }

  solidBrush_.Reset();
  d2dTargetBitmap_.Reset();
}

void D2DTextRenderer::ReleaseDeviceResources() {
  ReleaseRenderTargetResources();
  d2dContext_.Reset();
  d2dDevice_.Reset();
  d2dFactory_.Reset();
  dwriteFactory_.Reset();
  textRenderingParams_.Reset();
  d3dDevice_.Reset();
  textFormatCache_ = TextFormatCacheStore(nullptr);
}

} // namespace fdv::textd2d
