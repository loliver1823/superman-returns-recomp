// D11 fixture: every spelling of the guest base (round-3 verifier probe trees
// v3/p4 c_d7.cpp and v3/p7 copy.cpp, plus the real-tree shapes it listed:
// fm_module_walker `base[x] = 0`, kpcr lockWord(), fe_flow_watch `gb` thread
// parameter, image_write_watchdog getGlobalGuestMemory out-param). Round 3 saw
// none of the marked lines (UNSAFE direction: a write that is no site at all).
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <thread>
#include <windows.h>
struct PPCContext;
namespace sr { namespace kernel {
uint8_t* guestVAToHost(uint32_t);
uint8_t* getGuestBase();
extern uint8_t* g_guestBase;
extern thread_local uint8_t* g_guestBaseTls;
void getGlobalGuestMemory(uint8_t**, size_t*);
bool forcingAllowed();
} }
using sr::kernel::guestVAToHost;
using sr::kernel::g_guestBase;
// a generic byte writer: its body is NOT a site (param-fed); each CALL that
// hands it a guest pointer is, with that call's own gate
static inline void storeBE32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
static void setWord(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
struct Hdr { struct { uint32_t b; } a; uint32_t arr[4]; };
// a wrapper that RETURNS a guest pointer (kpcr lockWord)
static uint32_t* lockWordP(uint32_t va) { return reinterpret_cast<uint32_t*>(guestVAToHost(va)); }

void sub_82000200(PPCContext& ctx, uint8_t* base) {
  base[0x829A17BCu] = 1;  // EXPECT: UNGATED
  base[0x829A2BB0u] = base[0x829A2BB1u] = 0;  // EXPECT: UNGATED,UNGATED
  *reinterpret_cast<uint32_t*>(base + 0x829A2BB0u) = 0;  // EXPECT: UNGATED
  storeBE32(guestVAToHost(0x82000010u), 7u);  // EXPECT: UNGATED
  InterlockedExchange(reinterpret_cast<volatile LONG*>(guestVAToHost(0x82000014u)), 0);  // EXPECT: UNGATED
  guestVAToHost(0x82000018u)[0] = 1;  // EXPECT: UNGATED
  sr::kernel::g_guestBase[0x8200001Cu] = 1;  // EXPECT: UNGATED
  uint8_t* late = nullptr;
  late = guestVAToHost(0x82000020u);
  late[0] = 1;  // EXPECT: UNGATED
  Hdr* h = reinterpret_cast<Hdr*>(guestVAToHost(0x82000024u));
  h->a.b = 1;  // EXPECT: UNGATED
  h->arr[1] = 2;  // EXPECT: UNGATED
  FILE* f = std::fopen("x", "rb");
  std::fread(guestVAToHost(0x82000030u), 1, 16, f);  // EXPECT: UNGATED
  uint8_t* p = guestVAToHost(0x82000034u);
  ++*p;  // EXPECT: UNGATED
  p[1]++;  // EXPECT: UNGATED
  *p++ = 3;  // EXPECT: UNGATED
  uint8_t* q(guestVAToHost(0x82000038u));
  q[0] = 1;  // EXPECT: UNGATED
  std::fill_n(p, 4, uint8_t(0));  // EXPECT: UNGATED
  const uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 0};
  std::copy(src, src + 4, p);  // EXPECT: UNGATED
  Hdr& r = *reinterpret_cast<Hdr*>(guestVAToHost(0x8200003Cu));
  r.arr[0] = 1;  // EXPECT: UNGATED
  uint8_t* gb = sr::kernel::getGuestBase();
  gb[0x82000040u] = 1;  // EXPECT: UNGATED
  std::memset(sr::kernel::getGuestBase() + 0x82000044u, 0, 4);  // EXPECT: UNGATED
  uint8_t* tls = sr::kernel::g_guestBaseTls + 0x82000048u;
  tls[0] = 1;  // EXPECT: UNGATED
  *lockWordP(0x8200004Cu) = 0;  // EXPECT: UNGATED
  uint32_t* w = lockWordP(0x82000050u);
  *w = 0;  // EXPECT: UNGATED
  reinterpret_cast<std::atomic<uint32_t>*>(guestVAToHost(0x82000054u))->store(1);  // EXPECT: UNGATED
  std::memcpy((void*)(g_guestBase + 0x82000000u), src, 8);  // EXPECT: UNGATED
  std::memset(&g_guestBase[0x82000010u], 0, 8);  // EXPECT: UNGATED
  ZeroMemory(guestVAToHost(0x82000020u), 8);  // EXPECT: UNGATED
  CopyMemory(guestVAToHost(0x82000030u), src, 8);  // EXPECT: UNGATED
  memcpy_s(guestVAToHost(0x82000040u), 8, src, 8);  // EXPECT: UNGATED
  strcpy_s(reinterpret_cast<char*>(guestVAToHost(0x82000050u)), 8, "abc");  // EXPECT: UNGATED
  sprintf_s(reinterpret_cast<char*>(guestVAToHost(0x82000060u)), 8, "%d", 1);  // EXPECT: UNGATED
  setWord(guestVAToHost(0x82000070u), 5u);  // EXPECT: UNGATED
  if (sr::kernel::forcingAllowed()) setWord(guestVAToHost(0x82000074u), 6u);  // EXPECT: MASTER
  // ---- controls: READS and HOST memory are never sites
  const uint8_t* ro = guestVAToHost(0x82000080u);
  uint8_t x = ro[0];
  uint8_t c = *p++;
  uint8_t buf[16];
  std::memcpy(buf, guestVAToHost(0x82000084u), 16);
  uint8_t* host = buf;
  host[1] = x;
  setWord(buf, 7u);
  storeBE32(buf, c);
  if (h->arr[2] == 0) x = 1;
  bool flag = *guestVAToHost(0x82000088u) != 0;
  std::atomic<uint32_t> hostAtomic{0};
  hostAtomic.store(uint32_t(uintptr_t(guestVAToHost(0x8200008Cu))));
  (void)flag; (void)ctx; (void)x;
}

// A host thread entry taking the guest base as `gb` (fe_flow_watch convention).
void kickThread(uint8_t* gb) {
  uint8_t* hh = gb + 0x82000060u;
  hh[0] = 0; hh[3] = 4;  // EXPECT: UNGATED,UNGATED
}
// A `uint32_t* base` OUT-PARAMETER is not the guest base (d3d12_pm4_shmem shape).
static void ranges(uint32_t* base, uint32_t v) { *base = v * 4096u; }
void boot2() {
  uint8_t* g = nullptr; size_t sz = 0;
  sr::kernel::getGlobalGuestMemory(&g, &sz);
  uint8_t* s = g + 0x82000070u;
  s[0] = 1;  // EXPECT: UNGATED
  std::thread(kickThread, g).detach();
  uint32_t out = 0;
  ranges(&out, 3u);
}
int SDL_main(int, char**) { boot2(); return 0; }
