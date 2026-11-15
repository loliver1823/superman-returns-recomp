// D13 fixture: sets the flags a_flags.cpp tests, from another file, at boot.
extern bool g_d13Fab;
extern bool g_d13Q7;
static bool s_d13Local = false;          // a DIFFERENT file-local flag of the same name
void bootFlags() {
  g_d13Fab = true;
  g_d13Q7 = true;
  s_d13Local = true;
}
struct Boot { Boot() { bootFlags(); } };
static Boot s_boot;
