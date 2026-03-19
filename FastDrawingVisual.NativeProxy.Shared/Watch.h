#pragma once

#include <chrono>

namespace fdv::nativeproxy::shared {

class Watch final {
 public:
  using clock = std::chrono::steady_clock;
  using duration = clock::duration;
  using time_point = clock::time_point;

  Watch() noexcept { Restart(); }

  explicit Watch(bool startImmediately) noexcept {
    if (startImmediately) {
      Restart();
    }
  }

  void Start() noexcept {
    if (isRunning_) {
      return;
    }

    startTick_ = clock::now();
    isRunning_ = true;
  }

  void start() noexcept { Start(); }

  void Stop() noexcept {
    if (!isRunning_) {
      return;
    }

    accumulated_ += clock::now() - startTick_;
    isRunning_ = false;
  }

  void stop() noexcept { Stop(); }

  void Restart() noexcept {
    accumulated_ = duration::zero();
    startTick_ = clock::now();
    isRunning_ = true;
  }

  void restart() noexcept { Restart(); }

  void Reset() noexcept {
    accumulated_ = duration::zero();
    startTick_ = time_point{};
    isRunning_ = false;
  }

  void reset() noexcept { Reset(); }

  [[nodiscard]] bool IsRunning() const noexcept { return isRunning_; }

  [[nodiscard]] bool is_running() const noexcept { return IsRunning(); }

  [[nodiscard]] duration Elapsed() const noexcept {
    if (!isRunning_) {
      return accumulated_;
    }

    return accumulated_ + (clock::now() - startTick_);
  }

  [[nodiscard]] duration GetElapsed() const noexcept { return Elapsed(); }

  [[nodiscard]] double ElapsedMilliseconds() const noexcept {
    return std::chrono::duration<double, std::milli>(Elapsed()).count();
  }

  [[nodiscard]] double ElapsedSeconds() const noexcept {
    return std::chrono::duration<double>(Elapsed()).count();
  }

 private:
  duration accumulated_ = duration::zero();
  time_point startTick_{};
  bool isRunning_ = false;
};

} // namespace fdv::nativeproxy::shared
