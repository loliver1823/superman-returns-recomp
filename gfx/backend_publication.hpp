#pragma once
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>

namespace sr::gfx {

// The host serializes publication/retirement. Each caller holds a lease for its
// entire backend access; retirement stops admission and waits for prior users.
// A process-lifetime publication may outlive the backend and its worker loops.
template<class T>
class BackendPublication {
 public:
  class Lease {
   public:
    Lease() = default;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept : m_owner(std::exchange(other.m_owner, nullptr)), m_pointer(other.m_pointer) {}
    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        release();
        m_owner = std::exchange(other.m_owner, nullptr);
        m_pointer = other.m_pointer;
      }
      return *this;
    }
    ~Lease() { release(); }
    T* get() const { return m_owner ? m_pointer : nullptr; }
    T* operator->() const { return get(); }
    explicit operator bool() const { return get() != nullptr; }
   private:
    Lease(BackendPublication* owner, T* pointer) : m_owner(owner), m_pointer(pointer) {}
    void release() {
      if (!m_owner) return;
      std::lock_guard lock(m_owner->m_mutex);
      if (--m_owner->m_users == 0) m_owner->m_changed.notify_all();
      m_owner = nullptr;
    }
    BackendPublication* m_owner = nullptr;
    T* m_pointer = nullptr;
    friend class BackendPublication;
  };
  Lease acquire() {
    std::lock_guard lock(m_mutex);
    if (!m_pointer) return {};
    ++m_users;
    return Lease(this, m_pointer);
  }
  void publish(T* pointer) {
    std::unique_lock lock(m_mutex);
    if (m_pointer == pointer) return;
    m_pointer = nullptr;
    m_changed.wait(lock, [&] { return m_users == 0; });
    m_pointer = pointer;
  }
  bool retire(T* expected) {
    std::unique_lock lock(m_mutex);
    if (!expected || m_pointer != expected) return false;
    m_pointer = nullptr;
    m_changed.wait(lock, [&] { return m_users == 0; });
    return true;
  }
 private:
  std::mutex m_mutex;
  std::condition_variable m_changed;
  T* m_pointer = nullptr;
  size_t m_users = 0;
};

}  // namespace sr::gfx
