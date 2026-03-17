#include "TextFormatCacheStore.h"

#include <algorithm>

namespace fdv::textd2d {
namespace {
constexpr std::size_t kMaxCachedTextFormats = 32;
}

TextFormatCacheStore::TextFormatCacheStore(IDWriteFactory* dwriteFactory)
    : dwriteFactory_(dwriteFactory) {}

TextFormatCacheStore::~TextFormatCacheStore() = default;

IDWriteTextFormat* TextFormatCacheStore::Acquire(const std::wstring& fontFamily,
                                                 float fontSize) {
  if (dwriteFactory_ == nullptr) {
    return nullptr;
  }

  const float normalizedSize = (std::max)(1.0f, fontSize);
  const std::wstring& family =
      fontFamily.empty() ? std::wstring(L"Segoe UI") : fontFamily;
  const std::uint64_t useTick = ++useTickCounter_;

  for (auto& entry : cacheEntries_) {
    if (entry.fontSize == normalizedSize && entry.fontFamily == family &&
        entry.format != nullptr) {
      entry.lastUseTick = useTick;
      return entry.format.Get();
    }
  }

  ComPtr<IDWriteTextFormat> format;
  HRESULT hr = dwriteFactory_->CreateTextFormat(
      family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, normalizedSize,
      L"en-us", format.GetAddressOf());
  if (FAILED(hr) || format == nullptr) {
    return nullptr;
  }

  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

  if (cacheEntries_.size() >= kMaxCachedTextFormats) {
    auto evictIt = std::min_element(
        cacheEntries_.begin(), cacheEntries_.end(),
        [](const CacheEntry& left, const CacheEntry& right) {
          return left.lastUseTick < right.lastUseTick;
        });
    if (evictIt != cacheEntries_.end()) {
      *evictIt = {family, normalizedSize, format, useTick};
      return format.Get();
    }
  }

  cacheEntries_.push_back({family, normalizedSize, format, useTick});
  return format.Get();
}

} // namespace fdv::textd2d
