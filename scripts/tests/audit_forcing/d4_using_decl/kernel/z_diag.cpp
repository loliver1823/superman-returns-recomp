// D4 fixture: the gates, defined in another file's namespace.
#include <cstdlib>
namespace sr { namespace diag {
bool hackOn() {
  const char* e = std::getenv("SR_FIXTURE_D4_HACK");
  return !e || e[0] != '0';
}
bool traceOn() {
  const char* e = std::getenv("SR_FIXTURE_D4_TRACE");
  return e && e[0] == '1';
}
} }
