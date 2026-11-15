// Lane E (RELEASE_PLAN step 5, E3-E6 r3; coordinator 2026-10-23): the [ASSET] receipt - which bytes the game's
// Apt registered for the root movie MAIN.
//
// Since r3 the custom_frontend port feature patches classes inside MAIN (_level0: it registers its 40 __Packages
// classes first, tools/apt/patch_movie.py THE FIRST-REGISTRAR RULE). A run must therefore SHOW which MAIN ran:
// the movie parser sub_826AE6D0 (r5 = the raw .apt, r6 = the raw .const, docs/ENGINE_MAP.md) is compared, before
// it runs, with the known candidates - the disc's ui.AST:MAIN (retail) and the port feature's generated
// DATA/_workinprogress/custom_frontend/MAIN.ast (override) - and every registration that matches one logs
//   [ASSET] MAIN #<n> source=<retail|override> file="<where>" sha256=<the movie entry's sha256> const=match apt=<match|differs>
// The .const decides (the parser restores the two fields it relocates, so it is the file's bytes before and after
// every parse); the .apt is reported (the registrar relocates its char table in place, so a re-registration of a
// buffer already parsed once would differ). The first registration of a boot that matches no candidate logs
//   [ASSET] MAIN #1 source=unknown ...
// (the first movie a boot registers is MAIN, _level0). tools/main_receipt.py grades these lines.
// E3-E6 r4 (Astra r3 P1): every guest read goes through a span check (GuestSpan: the exe's is fault-free - the
// address range in the guest reservation AND every page committed and readable, the predicate the cursor's reader
// uses), first the 64-byte prefix, then the candidate's whole length; the parser's arguments carry no size, so a
// candidate matches only when its full length is readable at the registered address and equal byte for byte.
// The wrapper calling onParseGuest is compiled in every configuration, the release one (SR_ORACLE=0) included
// (kernel/apt_parse_trace.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sr {
namespace kernel {
namespace assetreceipt {

struct Candidate {
  std::string movie;            // "MAIN"
  std::string source;           // "retail" | "override"
  std::string file;             // "DATA/ui.AST:MAIN" | the generated file
  std::string sha256;           // the whole movie entry (the .ast container)
  std::vector<uint8_t> apt, cst;
};

// -> a host pointer to the n guest bytes at va, or nullptr unless all of them are readable.
using GuestSpan = const uint8_t* (*)(void* user, uint32_t va, uint32_t n);

// A candidate from one movie entry (a nested BGFA container holding the .apt and the .const, told apart by their
// magics "Apt Data:" / "Apt constant file"). False + *err when either part is missing.
bool candidateFromEntry(const std::string& movie, const std::string& source, const std::string& file,
                        const std::vector<uint8_t>& entry, Candidate* out, std::string* err);

// The candidate whose .const equals the guest bytes at cstVA (-1 = none); *aptMatch = its .apt also equals the guest
// bytes at aptVA. Reads only through `span`: the 64-byte prefix, then (on a prefix match) the whole part.
int match(const std::vector<Candidate>& cands, GuestSpan span, void* user, uint32_t aptVA, uint32_t cstVA,
          bool* aptMatch);

// Boot: the candidates of this run, and the expectation line ("[ASSET] MAIN expect ...").
void setCandidates(std::vector<Candidate> cands, const std::string& expectLine);

// The parser hook, BEFORE the parser runs. Cheap for every other movie: a 64-byte compare against each candidate.
void onParse(GuestSpan span, void* user, uint32_t aptVA, uint32_t cstVA);
// The exe's call (kernel/apt_parse_trace.cpp sub_826AE6D0): onParse through the fault-free guest span.
void onParseGuest(uint32_t aptVA, uint32_t cstVA);

// difftest: the lines onParse logged so far (also written to the log), and a reset.
std::vector<std::string> loggedForTest();
void resetForTest();

}  // namespace assetreceipt
}  // namespace kernel
}  // namespace sr
