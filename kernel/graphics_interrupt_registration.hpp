#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace sr::kernel::graphics_interrupt {

struct Registration {
  uint32_t callbackVA = 0;
  uint32_t contextVA = 0;
  bool nativeSource0 = false;
};

enum class PublishResult { Published, ReentrantUnregister, RetirementInProgress };
using PrepareCallback = void (*)(const Registration&, void*) noexcept;

class Service;

// A lease pins its immutable registration through all delivery-related device
// accesses. Keep it on the acquiring thread, including after moves: callback
// publication uses that thread's depth for this service to avoid waiting on
// itself. A lease for another service does not bypass retirement here.
class DeliveryLease {
 public:
  DeliveryLease() noexcept = default;
  ~DeliveryLease();
  DeliveryLease(const DeliveryLease&) = delete;
  DeliveryLease& operator=(const DeliveryLease&) = delete;
  DeliveryLease(DeliveryLease&& other) noexcept;
  DeliveryLease& operator=(DeliveryLease&& other) noexcept;

  explicit operator bool() const noexcept { return service_ != nullptr; }
  const Registration& registration() const noexcept { return registration_; }

 private:
  friend class Service;
  DeliveryLease(Service& service, Registration registration) noexcept;
  void release() noexcept;

  Service* service_ = nullptr;
  Registration registration_;
  std::thread::id owner_;
};

class Service {
 public:
  Service() = default;
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  // Nonblocking admission: an empty lease means unregistered or retiring.
  DeliveryLease begin();

  // External setters close admission and drain every active lease, including
  // snapshots retained across callback re-registration, before returning.
  // A callback may publish a nonzero replacement without draining its lease.
  // Reentrant unregister and publication during retirement reject unchanged;
  // the caller must stop before an SDK teardown can free the device.
  // Prepare runs under the publication mutex before exposing the tuple. It
  // must not throw or reenter this service. Guest calls never hold this mutex.
  PublishResult publish(Registration next, PrepareCallback prepare = nullptr,
                        void* prepareContext = nullptr);

 private:
  friend class DeliveryLease;
  void release();

  std::mutex mutex_;
  std::condition_variable changed_;
  Registration current_;
  std::size_t active_ = 0;
  bool retiring_ = false;
};

// Process-lifetime storage for detached VD and CP delivery threads. Isolated
// test services must outlive every lease and setter using them.
Service& registrationService();

}  // namespace sr::kernel::graphics_interrupt
