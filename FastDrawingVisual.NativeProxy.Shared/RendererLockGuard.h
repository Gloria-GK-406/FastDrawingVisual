#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace fdv::nativeproxy::shared {

class RendererLockGuard final {
 public:
  explicit RendererLockGuard(CRITICAL_SECTION* cs) : cs_(cs) {
    if (cs_ != nullptr) {
      EnterCriticalSection(cs_);
    }
  }

  ~RendererLockGuard() {
    if (cs_ != nullptr) {
      LeaveCriticalSection(cs_);
    }
  }

  RendererLockGuard(const RendererLockGuard&) = delete;
  RendererLockGuard& operator=(const RendererLockGuard&) = delete;

 private:
  CRITICAL_SECTION* cs_ = nullptr;
};

} // namespace fdv::nativeproxy::shared
