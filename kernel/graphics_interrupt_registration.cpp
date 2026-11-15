// Retail lifecycle verified against IDA and generated SDK source:
// destroy 8210651C unregisters, then 82106528 calls ring teardown, which frees
// *(dev+2A14) at 820FDF44 without clearing it. Release frees dev at 820F35EC.
// Unregister must therefore retire every admitted delivery before returning.
#include "graphics_interrupt_registration.hpp"

#include <exception>
#include <unordered_map>
#include <utility>

namespace sr::kernel::graphics_interrupt {
namespace {
thread_local std::unordered_map<Service*, std::size_t> deliveryDepths;
}

DeliveryLease::DeliveryLease(Service& service, Registration registration) noexcept
    : service_(&service), registration_(registration), owner_(std::this_thread::get_id()) {}

DeliveryLease::~DeliveryLease() { release(); }

DeliveryLease::DeliveryLease(DeliveryLease&& other) noexcept
    : service_(std::exchange(other.service_, nullptr)), registration_(other.registration_),
      owner_(other.owner_) {}

DeliveryLease& DeliveryLease::operator=(DeliveryLease&& other) noexcept {
  if (this != &other) {
    release();
    service_ = std::exchange(other.service_, nullptr);
    registration_ = other.registration_;
    owner_ = other.owner_;
  }
  return *this;
}

void DeliveryLease::release() noexcept {
  if (!service_) return;
  const auto depth = deliveryDepths.find(service_);
  if (owner_ != std::this_thread::get_id() || depth == deliveryDepths.end() || !depth->second)
    std::terminate();
  service_->release();
  if (--depth->second == 0) deliveryDepths.erase(depth);
  service_ = nullptr;
}

DeliveryLease Service::begin() {
  std::lock_guard lock(mutex_);
  if (retiring_ || !current_.callbackVA) return {};
  // Allocate TLS bookkeeping before admission commits. A failed allocation
  // leaves active_ unchanged; erased zero depths cannot alias a later service.
  const auto depth = deliveryDepths.try_emplace(this, 0).first;
  ++depth->second;
  ++active_;
  return DeliveryLease(*this, current_);
}

PublishResult Service::publish(Registration next, PrepareCallback prepare, void* prepareContext) {
  std::unique_lock lock(mutex_);
  const auto depth = deliveryDepths.find(this);
  if (depth != deliveryDepths.end() && depth->second) {
    if (!next.callbackVA) return PublishResult::ReentrantUnregister;
    if (retiring_) return PublishResult::RetirementInProgress;
    if (prepare) prepare(next, prepareContext);
    current_ = next;
    return PublishResult::Published;
  }

  // Serialize external setters over the entire retirement, while the CV
  // releases the publication mutex so active deliveries can finish.
  changed_.wait(lock, [this] { return !retiring_; });
  retiring_ = true;
  if (prepare) prepare(next, prepareContext);
  current_ = next;
  changed_.wait(lock, [this] { return active_ == 0; });
  retiring_ = false;
  lock.unlock();
  changed_.notify_all();
  return PublishResult::Published;
}

void Service::release() {
  {
    std::lock_guard lock(mutex_);
    if (!active_) std::terminate();
    --active_;
  }
  changed_.notify_all();
}

Service& registrationService() {
  static auto* service = new Service;
  return *service;
}

}  // namespace sr::kernel::graphics_interrupt
