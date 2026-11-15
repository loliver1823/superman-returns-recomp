// B1 fixture: same-named gate helpers in another file / namespace. Scanned
// LAST, so the old bare-name lookup let this watchEnabled() replace a_site.cpp's.
#include <cstdlib>

bool diagOn() {
  const char* e = std::getenv("SR_FIXTURE_B1_DIAG");
  return e && e[0] == '1';
}

namespace sr { namespace kernel {
bool watchEnabled() {
  const char* e = std::getenv("SR_FIXTURE_B1_OTHER");
  return !e || e[0] != '0';
}
} }
