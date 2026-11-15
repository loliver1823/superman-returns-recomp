// Lane E (RELEASE_PLAN step 5, E3-E6 r3 / r4): the [ASSET] receipt for MAIN (kernel/asset_receipt.hpp).
#include "asset_receipt.hpp"

#include "costume_viewer_model.hpp"   // cview::astForEachNested: a nested BGFA container's payloads
#include "guest_context.hpp"          // the fault-free guest span (isGuestVAInRange, hostRangeReadableCached)
#include "mod_formats.hpp"            // sha256Hex
#include "../app/log.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

namespace sr {
namespace kernel {
namespace assetreceipt {
namespace {

constexpr char kAptMagic[] = "Apt Data:";
constexpr char kConstMagic[] = "Apt constant file";
constexpr uint32_t kPrefix = 64;

std::mutex g_mu;
std::vector<Candidate> g_cands;
bool g_sawFirst = false;
uint32_t g_matched = 0;
std::vector<std::string> g_logged;

bool startsWith(const std::vector<uint8_t>& v, const char* magic) {
  const size_t n = std::strlen(magic);
  return v.size() >= n && std::memcmp(v.data(), magic, n) == 0;
}

void emit(const std::string& line) {
  g_logged.push_back(line);
  SR_LOG_BOOT(sr::LogSeverity::Info, "%s", line.c_str());
}

// The exe's span: inside the guest reservation AND every page committed, readable and not a guard page (the cursor
// reader's predicate, kernel/mouse_ui.cpp guestMap). A null, low (first 64 KB) or wrapping range is refused.
const uint8_t* guestSpan(void*, uint32_t va, uint32_t n) {
  if (n == 0 || va < 0x10000u || va > 0xFFFFFFFFu - (n - 1)) return nullptr;
  if (!isGuestVAInRange(va) || !isGuestVAInRange(va + (n - 1))) return nullptr;
  const uint8_t* p = guestVAToHost(va);
  return (p && hostRangeReadableCached(p, n)) ? p : nullptr;
}

bool equalAt(GuestSpan span, void* user, uint32_t va, const std::vector<uint8_t>& want, uint32_t n) {
  const uint8_t* p = span(user, va, n);
  return p && std::memcmp(p, want.data(), n) == 0;
}

}  // namespace

bool candidateFromEntry(const std::string& movie, const std::string& source, const std::string& file,
                        const std::vector<uint8_t>& entry, Candidate* out, std::string* err) {
  Candidate c;
  c.movie = movie;
  c.source = source;
  c.file = file;
  c.sha256 = sha256Hex(entry.data(), entry.size());
  std::string e;
  const bool ok = cview::astForEachNested(entry, [&](const std::vector<uint8_t>& p) {
    if (startsWith(p, kConstMagic)) c.cst = p;
    else if (startsWith(p, kAptMagic)) c.apt = p;
    return true;
  }, &e);
  if (!ok) {
    *err = file + ": " + e;
    return false;
  }
  if (c.cst.size() < kPrefix || c.apt.size() < kPrefix || c.cst.size() > 0x10000000u || c.apt.size() > 0x10000000u) {
    *err = file + ": no .apt / .const part";
    return false;
  }
  *out = std::move(c);
  return true;
}

int match(const std::vector<Candidate>& cands, GuestSpan span, void* user, uint32_t aptVA, uint32_t cstVA,
          bool* aptMatch) {
  *aptMatch = false;
  if (!span || !cstVA) return -1;
  for (size_t i = 0; i < cands.size(); ++i) {
    const Candidate& c = cands[i];
    if (c.cst.size() < kPrefix) continue;
    if (!equalAt(span, user, cstVA, c.cst, kPrefix)) continue;                      // every other movie stops here
    if (!equalAt(span, user, cstVA, c.cst, uint32_t(c.cst.size()))) continue;      // the whole part, readable
    *aptMatch = aptVA && equalAt(span, user, aptVA, c.apt, uint32_t(c.apt.size()));
    return int(i);
  }
  return -1;
}

void setCandidates(std::vector<Candidate> cands, const std::string& expectLine) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_cands = std::move(cands);
  g_sawFirst = false;
  g_matched = 0;
  if (!expectLine.empty()) emit(expectLine);
}

void onParse(GuestSpan span, void* user, uint32_t aptVA, uint32_t cstVA) {
  std::lock_guard<std::mutex> lk(g_mu);
  const bool first = !g_sawFirst;
  g_sawFirst = true;
  if (g_cands.empty() || !span) return;
  bool aptOk = false;
  const int k = match(g_cands, span, user, aptVA, cstVA, &aptOk);
  char buf[512];
  if (k >= 0) {
    const Candidate& c = g_cands[size_t(k)];
    std::snprintf(buf, sizeof buf, "[ASSET] %s #%u source=%s file=\"%s\" sha256=%s const=match apt=%s", c.movie.c_str(),
                  ++g_matched, c.source.c_str(), c.file.c_str(), c.sha256.c_str(), aptOk ? "match" : "differs");
    emit(buf);
  } else if (first) {
    // #1 is the FIRST registration whatever it is (E3-E6 r6, Astra r5 P2): an unknown one takes the ordinal, so a
    // later match is #2 and a log cut before this line has no #1 at all (tools/main_receipt.py: NO-EVIDENCE)
    ++g_matched;
    std::snprintf(buf, sizeof buf,
                  "[ASSET] %s #1 source=unknown: the first movie this boot registered (_level0) matches no known %s "
                  "(neither the disc's nor the port feature's; a mod's?)",
                  g_cands[0].movie.c_str(), g_cands[0].movie.c_str());
    emit(buf);
  }
}

void onParseGuest(uint32_t aptVA, uint32_t cstVA) { onParse(&guestSpan, nullptr, aptVA, cstVA); }

std::vector<std::string> loggedForTest() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_logged;
}

void resetForTest() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_cands.clear();
  g_logged.clear();
  g_sawFirst = false;
  g_matched = 0;
}

}  // namespace assetreceipt
}  // namespace kernel
}  // namespace sr
