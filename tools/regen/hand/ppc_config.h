#pragma once
#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED


#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0x9E0000ull
#define PPC_CODE_BASE 0x820F0000ull
#define PPC_CODE_SIZE 0x81F5B8ull

#ifdef PPC_INCLUDE_DETAIL
#include "ppc_detail.h"
#endif

// BANDAID: Global "init phase" timeout. The game loop runs inside the E630
// init traversal and never exits because animation stubs prevent proper scene
// loading. After kInitDeadline seconds the lock function returns -2 (timeout),
// which lets each caller skip its work. Once E630 depth returns to 0 the flag
// is cleared so normal gameplay is unaffected.
// Proper fix: native SIMD animation pipeline. See docs/BANDAIDS.md.
#include <atomic>
#include <chrono>
struct InitTimeoutState {
    std::chrono::steady_clock::time_point startTime{std::chrono::steady_clock::now()};
    std::atomic<bool> armed{true};
    static constexpr auto kDeadline = std::chrono::seconds(10);
    bool expired() const {
        return armed.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() - startTime > kDeadline;
    }
    void disarm() { armed.store(false, std::memory_order_release); }
};
inline InitTimeoutState& g_initTimeout() {
    static InitTimeoutState s;
    return s;
}


// iter-464 codegen: flag_transform.py localizes the guest registers of every generated
// function (see SR_PPC_LOCALIZED below when applied). A function that reads a localized
// register at entry but has no direct caller (XenonAnalyse chunk duplicates, SEH filters,
// vtable-only targets) loads it from ctx and reports once if it ever runs.
#include <cstdint>
extern "C" void sr_abi_unbridged_entry(uint32_t va, const char* regs);
#define SR_ABI_UNBRIDGED_ENTRY(va, regs) do { static bool _sr_abi_once = false; if (__builtin_expect(!_sr_abi_once, 0)) { _sr_abi_once = true; sr_abi_unbridged_entry((va), (regs)); } } while (0)

#endif
