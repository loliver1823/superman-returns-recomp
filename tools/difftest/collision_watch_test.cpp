// iter-463 --collision-watch-test: the fault-free read layer and helpers of kernel/collision_watch.cpp.
//
//   W1  inWindow boundaries (low edge, 0xA0000000 top, size overflow / wrap)
//   W2  Mem reads NEVER dereference outside the window: base is a reservation whose pages at and
//       above 0xA0000000 (and below 0x10000) are reserved-but-uncommitted, so any stray
//       dereference would fault the test process; in-window committed reads return the BE value
//   W3  flatModelReadable: true on the difftest's full-commit guest base, false (with a reason)
//       on a reservation with an uncommitted hole, false under g_physUnifyActive
//   W4  firstSighting de-dup keyed by (addr, translation)
//   W5  the per-second budget passes exactly N and counts the rest as dropped
//   B1  cost of one [COLL_PATH]-sized log line through sr::Log (INFO, not a gate) - the only
//       per-tick cost the harness bench (archive/v1_invest/collision/route/harness) cannot see,
//       because the budget caps the bench's lines
// Wiring (the hooks fire, and are read-only) is proven on the golden-call harness instead:
// fly/mv_midtown outputs byte-identical with SR_COLLISION_WATCH=0 and =1 (commit message).
#include "kernel/collision_watch.hpp"
#include "kernel/guest_context.hpp"
#include "app/log.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}
}  // namespace

int runCollisionWatchTest() {
  using namespace sr::kernel::collwatch;
  std::printf("[COLLISION_WATCH_TEST] kernel/collision_watch.cpp read layer + helpers\n");
  // W1
  check(!inWindow(0x0000FFFFu, 4) && inWindow(0x00010000u, 4), "W1a low edge 0x10000");
  check(inWindow(0x9FFFFFFCu, 4) && !inWindow(0x9FFFFFFDu, 4) && !inWindow(0xA0000000u, 1),
        "W1b top edge 0xA0000000 (last u32 at 0x9FFFFFFC)");
  check(!inWindow(0x9FFFFFF0u, 0xFFFFFFFFu) && !inWindow(0xFFFFFFFCu, 4) && !inWindow(0, 0),
        "W1c size overflow / wrap / null refused");
#ifdef _WIN32
  // W2: reservation of 0xA0010000 bytes, commit only [0x10000, 0x20000) and the last in-window page.
  uint8_t* r = (uint8_t*)VirtualAlloc(nullptr, 0xA0010000ull, MEM_RESERVE, PAGE_NOACCESS);
  if (!r) {
    check(false, "W2 reserve 2.5 GB address space");
  } else {
    bool ok = VirtualAlloc(r + 0x10000, 0x10000, MEM_COMMIT, PAGE_READWRITE) &&
              VirtualAlloc(r + 0x9FFFF000ull, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    check(ok, "W2a commit two in-window pages");
    if (ok) {
      const uint8_t be[4] = {0x12, 0x34, 0x56, 0x78};
      std::memcpy(r + 0x10000, be, 4);
      std::memcpy(r + 0x9FFFFFFCull, be, 4);
      const Mem m{r};
      check(m.u32(0x10000) == 0x12345678u && m.u32(0x9FFFFFFCu) == 0x12345678u, "W2b in-window BE reads");
      // Each of these would FAULT if dereferenced (uncommitted pages of the reservation).
      check(m.u32(0xA0000000u) == 0 && m.u32(0xA0000004u) == 0 && m.u8(0xA0000000u) == 0,
            "W2c reads at/above 0xA0000000 refused without touching memory");
      check(m.u32(0x0000FFFCu) == 0 && m.u8(0x0000FFFFu) == 0 && m.u32(0) == 0, "W2d reads below 0x10000 refused");
      check(m.u32(0x9FFFFFFEu) == 0, "W2e a read straddling the top refused");
      float v[3];
      m.vec3(0x9FFFFFF8u, v);
      check(std::isnan(v[0]) && std::isnan(v[2]), "W2f vec3 straddling the top -> NaN, no read");
    }
    // W3b: the same reservation has uncommitted holes -> refused with a reason.
    char why[160];
    check(!flatModelReadable(r, why, sizeof why) && std::strstr(why, "not committed"), "W3b hole refused");
    std::printf("      reason: %s\n", why);
    VirtualFree(r, 0, MEM_RELEASE);
  }
  // W3a: the difftest's own guest base (MEM_RESERVE|MEM_COMMIT over kSize, the game's model).
  {
    char why[160];
    uint8_t* gb = sr::kernel::getGuestBase();
    const bool ok = flatModelReadable(gb, why, sizeof why);
    check(ok, "W3a full-commit guest base accepted");
    std::printf("      reason: %s\n", why);
    const bool saved = sr::kernel::g_physUnifyActive;
    sr::kernel::g_physUnifyActive = true;
    check(!flatModelReadable(gb, why, sizeof why) && std::strstr(why, "SR_PHYS_UNIFY"), "W3c unified model refused");
    sr::kernel::g_physUnifyActive = saved;
  }
#endif
  // W4
  {
    const float a[3] = {1.f, 2.f, 3.f}, b[3] = {1.f, 2.f, 3.5f};
    const bool first = firstSighting(0x85D9CE80u, a);
    const bool again = firstSighting(0x85D9CE80u, a);
    const bool moved = firstSighting(0x85D9CE80u, b);
    const bool other = firstSighting(0x85D9CE90u, a);
    check(first && !again && moved && other, "W4 de-dup by (addr, translation); reuse re-described");
  }
  // W5
  check(budgetForTest(3, 10), "W5 budget passes exactly 3 of 10, 7 dropped");
  // B1
  {
    // Into a real log file, as in the game (sr::Log::init); stderr also gets each line (console
    // sink), so run the test with stderr redirected to a file, as rolls do.
    char tmp[MAX_PATH + 1] = {0};
    GetTempPathA(MAX_PATH, tmp);
    std::strncat(tmp, "sr_collision_watch_test.log", MAX_PATH - std::strlen(tmp));
    sr::Log::init(tmp);
    const int n = 20000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[COLL_PATH_BENCH] t=%u ms=%u loco=%s def=%u sub=%u path=%s L=%u in=(%.3f %.3f %.3f) "
                  "out=(%.3f %.3f %.3f) v=(%.2f %.2f %.2f) spd=%.1f man=%s ov=%s",
                  i, i, "FLY", 8u, 4u, "integrate", 2u, -2278.987, 1185.540, -754.917, -2278.785, 1185.326,
                  -754.221, 6.04, -6.44, 20.85, 22.6, "0[]", "6[85D921C0,85D8FD40,85C782E0,85C782E0,85C782E0,85C782E0]");
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
    sr::Log::shutdown();
    std::remove(tmp);
    std::printf("  info B1 one [COLL_PATH]-sized log line: %.2f us (x30/s of fast flight = %.0f us/s)\n", us, us * 30);
  }
  std::printf("[COLLISION_WATCH_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
