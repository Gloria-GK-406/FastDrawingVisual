#pragma once
#include <d2d1_1.h>
#include <dwrite.h>
#include <cstdint>
#include <vector>
#include <string>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class TextFormatCacheStore {
 public:
  TextFormatCacheStore(IDWriteFactory* dwriteFactory);
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
