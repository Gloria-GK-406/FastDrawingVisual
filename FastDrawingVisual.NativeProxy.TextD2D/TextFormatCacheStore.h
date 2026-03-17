#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <dwrite.h>
#include <wrl/client.h>

namespace fdv::textd2d {

using Microsoft::WRL::ComPtr;

class TextFormatCacheStore final {
 public:
  explicit TextFormatCacheStore(IDWriteFactory* dwriteFactory);
  ~TextFormatCacheStore();

  IDWriteTextFormat* Acquire(const std::wstring& fontFamily, float fontSize);

 private:
  struct CacheEntry {
    std::wstring fontFamily;
    float fontSize = 0.0f;
    ComPtr<IDWriteTextFormat> format = nullptr;
    std::uint64_t lastUseTick = 0;
  };

  std::vector<CacheEntry> cacheEntries_;
  std::uint64_t useTickCounter_ = 0;
  ComPtr<IDWriteFactory> dwriteFactory_ = nullptr;
};

} // namespace fdv::textd2d
