// boot.device_create (native-a, 2026-10-10): the device-create producers of the boot PM4 block as host code under
// SR_NATIVE_DEVCREATE (default ON since native/devcreate-fix, 2026-10-15; =0 keeps the guest bodies) on the native
// transport. See native_devcreate.cpp.
#pragma once

namespace sr::native::devcreate {
// The final counter line ([NATIVE_DEVCREATE] ...), gate ON only; `why` = the CP's exit / census reason.
void finalLine(const char* why);
}  // namespace sr::native::devcreate
