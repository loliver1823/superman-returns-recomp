// iter-458 (H-REC): the cost of the trace recorder while it is ENABLED BUT NOT
// YET OPEN. The live renderer calls traceRecord() for every command whenever
// SR_HLE_TRACE_REC is set. idle1 (recorder path + hotkey, F9 never pressed)
// measured a render-queue lag ~180x the gate-off runs, with no trace open.
//
// This times the REAL traceRecord (through traceRecordForTest) in exactly that
// state: SR_HLE_TRACE_REC set in the environment (no test path override, so
// tracePath() does the production getenv), SR_HLE_TRACE_HOTKEY=1, never armed.
//
// Run: build/difftest.exe --trace-idle-bench [calls]
// Prints ns per traceRecord call and the implied render-thread share at a
// stated command rate, and FAILS (exit 1) above kMaxIdleNs per call. Before
// the fix it measured 17.5 us/call (seven getenv per command); the gate sits
// below the cost of ONE getenv, so any per-command environment lookup trips it.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "gfx/hle_video.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>

namespace H = sr::gfx::hle;

namespace {
using Clock = std::chrono::steady_clock;
double nsPer(Clock::time_point t0, Clock::time_point t1, long long n) {
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / (double)n;
}
}  // namespace

int runTraceIdleBench(long long calls) {
  if (calls <= 0) calls = 400000;
  int envCount = 0;
  for (char** e = _environ; e && *e; ++e) envCount++;

  // The idle1 configuration: recorder path set, operator hotkey arm, F9 never
  // pressed. Every other arm cleared so the conflict check sees exactly one.
  _putenv_s("SR_HLE_TRACE_REC", "archive/_trace_idle_bench_never_opened.srht");
  _putenv_s("SR_HLE_TRACE_HOTKEY", "1");
  _putenv_s("SR_HLE_TRACE_ARM_TEXKEY", "");
  _putenv_s("SR_HLE_TRACE_ARM_UNLOCKN", "");
  _putenv_s("SR_HLE_TRACE_ARM_RESOLVEN", "");
  _putenv_s("SR_HLE_TRACE_ARM_PRERESOLVE", "");
  _putenv_s("SR_HLE_TRACE_ARM_GEOM", "");
  H::resetRecorderForTest(nullptr);   // nullptr = production getenv path

  H::RenderCommand tex{};
  tex.type = H::RenderCommandType::SetTexture;
  tex.setTexture.sampler = 3;
  H::RenderCommand draw{};
  draw.type = H::RenderCommandType::DrawIndexedVertices;

  // Bare getenv, for scale.
  // (the name is read through a volatile pointer so the call cannot be hoisted)
  static const char* volatile s_name = "SR_HLE_TRACE_ARM_GEOM";
  uintptr_t sink = 0;
  auto g0 = Clock::now();
  for (long long i = 0; i < calls; i++) sink += (uintptr_t)std::getenv(s_name);
  auto g1 = Clock::now();
  if (sink == 1) std::puts("unreachable");

  auto t0 = Clock::now();
  for (long long i = 0; i < calls; i++)
    H::traceRecordForTest((i & 3) == 0 ? tex : draw);
  auto t1 = Clock::now();

  const double getenvNs = nsPer(g0, g1, calls);
  const double recNs = nsPer(t0, t1, calls);
  std::printf("[TRACE_IDLE_BENCH] env vars=%d calls=%lld | getenv=%.1f ns | "
              "traceRecord (enabled, not open, hotkey waiting)=%.1f ns/call\n",
              envCount, calls, getenvNs, recNs);
  for (double rate : {100000.0, 200000.0, 400000.0})
    std::printf("[TRACE_IDLE_BENCH]   at %.0f commands/s the recorder alone "
                "costs %.1f%% of one render thread\n",
                rate, recNs * rate / 1e7);
  H::closeRecorderForTest();
  _putenv_s("SR_HLE_TRACE_REC", "");
  _putenv_s("SR_HLE_TRACE_HOTKEY", "");
  constexpr double kMaxIdleNs = 1000.0;
  const bool ok = recNs <= kMaxIdleNs && recNs < getenvNs;
  std::printf("%s: armed-idle traceRecord %.1f ns/call (limit %.0f ns and below "
              "one getenv %.1f ns)\n",
              ok ? "PASS" : "FAIL", recNs, kMaxIdleNs, getenvNs);
  return ok ? 0 : 1;
}
