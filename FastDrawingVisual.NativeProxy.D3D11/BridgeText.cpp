#include "BridgeRendererInternal.h"

#include <algorithm>
#include <string>

namespace {
bool Utf8ToWide(const std::uint8_t* bytes, std::uint32_t count,
                std::wstring& out) {
  out.clear();
  if (count == 0) {
    return true;
  }

  const int wideCount = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<LPCCH>(bytes),
      static_cast<int>(count), nullptr, 0);
  if (wideCount <= 0) {
    return false;
  }

  out.resize(static_cast<std::size_t>(wideCount));
  const int converted = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<LPCCH>(bytes),
      static_cast<int>(count), out.data(), wideCount);
  if (converted <= 0) {
    return false;
  }

  return true;
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

bool BeginD2DDraw(BridgeRendererD3D11* s, bool& d2dDrawActive) {
  if (d2dDrawActive) {
    return true;
  }

  if (!s || !s->d2dContext || !s->d2dTargetBitmap || !s->d2dSolidBrush ||
      !s->dwriteFactory) {
    SetLastError(s, E_UNEXPECTED);
    return false;
  }

  s->context->Flush();
  s->d2dContext->BeginDraw();
  d2dDrawActive = true;
  return true;
}

bool EndD2DDraw(BridgeRendererD3D11* s, bool& d2dDrawActive) {
  if (!d2dDrawActive) {
    return true;
  }

  HRESULT hr = s->d2dContext->EndDraw();
  d2dDrawActive = false;
  if (FAILED(hr)) {
    SetLastError(s, hr);
    return false;
  }

  return true;
}

bool ExecuteDrawTextCommand(BridgeRendererD3D11* s,
                            const fdv::protocol::DrawTextPayload& payload,
                            const fdv::protocol::CommandReader& reader,
                            bool& d2dDrawActive) {
  fdv::protocol::BlobSpan textUtf8{};
  if (!reader.TryResolveBlob(payload.textUtf8, textUtf8)) {
    SetLastError(s, E_INVALIDARG);
    return false;
  }

  if (textUtf8.bytes == 0) {
    return true;
  }

  fdv::protocol::BlobSpan fontUtf8{};
  if (!reader.TryResolveBlob(payload.fontFamilyUtf8, fontUtf8)) {
    SetLastError(s, E_INVALIDARG);
    return false;
  }

  std::wstring textWide;
  if (!Utf8ToWide(textUtf8.data, textUtf8.bytes, textWide) || textWide.empty()) {
    SetLastError(s, E_INVALIDARG);
    return false;
  }

  std::wstring fontWide;
  if (!Utf8ToWide(fontUtf8.data, fontUtf8.bytes, fontWide)) {
    SetLastError(s, E_INVALIDARG);
    return false;
  }

  if (fontWide.empty()) {
    fontWide = L"Segoe UI";
  }

  if (!BeginD2DDraw(s, d2dDrawActive)) {
    return false;
  }

  const float fontSize = std::max(1.0f, payload.fontSize);
  IDWriteTextFormat* textFormat = nullptr;
  HRESULT hr = s->dwriteFactory->CreateTextFormat(
      fontWide.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"en-us",
      &textFormat);
  if (FAILED(hr) || textFormat == nullptr) {
    SetLastError(s, FAILED(hr) ? hr : E_FAIL);
    SafeRelease(&textFormat);
    return false;
  }

  textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

  D2D1_RECT_F layoutRect = {payload.x, payload.y, static_cast<float>(s->width),
                            static_cast<float>(s->height)};
  if (layoutRect.right <= layoutRect.left) {
    layoutRect.right = layoutRect.left + 1.0f;
  }
  if (layoutRect.bottom <= layoutRect.top) {
    layoutRect.bottom = layoutRect.top + 1.0f;
  }

  s->d2dSolidBrush->SetColor(ToD2DColor(payload.color));
  s->d2dContext->DrawText(textWide.c_str(), static_cast<UINT32>(textWide.size()),
                          textFormat, &layoutRect, s->d2dSolidBrush,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP,
                          DWRITE_MEASURING_MODE_NATURAL);
  textFormat->Release();
  return true;
}
