// B1 fixture: txc::enabled(), a default-ON own gate. Scanned FIRST, so the old
// bare-name lookup tried it before diagOn() and matched the member call
// cfg.enabled() in a_site.cpp patchC.
#include <cstdlib>

namespace sr { namespace kernel { namespace txc {
bool enabled() {
  const char* e = std::getenv("SR_FIXTURE_B1_TXC");
  return !e || e[0] != '0';
}
} } }
