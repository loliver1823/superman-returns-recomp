// B6 fixture: the unrelated default-ON gate the registering function calls.
#include <cstdlib>
namespace sr { namespace kernel { namespace txc {
bool enabled() {
  const char* e = std::getenv("SR_FIXTURE_B6_TXC");
  return !e || e[0] != '0';
}
} } }
