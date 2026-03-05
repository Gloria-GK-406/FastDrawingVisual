#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace fdvlog {

// Non-thread-safe fixed-capacity ring buffer with overwrite-on-full semantics.
template <typename T> class RingBuffer {
public:
  RingBuffer() = default;
  explicit RingBuffer(size_t capacity) { Reset(capacity); }

  void Reset(size_t capacity) {
    items_.assign(capacity, T{});
    Clear();
  }

  void Clear() noexcept {
    head_ = 0;
    tail_ = 0;
    count_ = 0;
  }

  size_t Capacity() const noexcept { return items_.size(); }
  size_t Size() const noexcept { return count_; }
  bool Empty() const noexcept { return count_ == 0; }
  bool Full() const noexcept { return count_ == items_.size() && !items_.empty(); }

  bool PushOverwrite(const T &value) { return EmplaceOverwrite(value); }
  bool PushOverwrite(T &&value) { return EmplaceOverwrite(std::move(value)); }

  template <typename... Args> bool EmplaceOverwrite(Args &&...args) {
    if (items_.empty())
      return false;

    bool overwritten = false;
    if (count_ == items_.size()) {
      tail_ = (tail_ + 1) % items_.size();
      --count_;
      overwritten = true;
    }

    items_[head_] = T(std::forward<Args>(args)...);
    head_ = (head_ + 1) % items_.size();
    ++count_;
    return overwritten;
  }

  bool TryPop(T *value) {
    if (!value || count_ == 0)
      return false;

    *value = std::move(items_[tail_]);
    tail_ = (tail_ + 1) % items_.size();
    --count_;
    return true;
  }

private:
  std::vector<T> items_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t count_ = 0;
};

} // namespace fdvlog
