#pragma once
// iter-466 tooling (input isolation, feedback_background_runs): which controller an automated
// roll may open. A roll must never read the owner's real controller (he may be playing while
// it runs). With SR_HARNESS_VPAD=1 an automated run (SR_INSTANCE_ID) opens ONLY a ViGEm virtual
// pad (tools/glyphs/vpad_hotplug.py): a device whose device-tree ancestry reaches the ViGEmBus
// bus driver (service "ViGEmBus"), with the VID/PID of a ViGEm target. A real pad hangs off a
// USB hub, a Bluetooth enumerator or a wireless receiver, so the same VID/PID (a wired 360 pad
// is 045E:028E too) is never enough on its own.
//
// Probe (2026-10-06, SDL 2.30 RAWINPUT, a vgamepad VX360Gamepad):
//   path   \\?\HID#VID_045E&PID_028E&IG_01#3&338b0514&0&0000#{4d1e55b2-...}
//   chain  HID\VID_045E&PID_028E&IG_01\... -> USB\VID_045E&PID_028E&IG_01\... [HidUsb]
//          -> USB\VID_045E&PID_028E\01 [xusb22] -> ROOT\SYSTEM\0007 [ViGEmBus] -> HTREE\ROOT\0
//
// Kernel-only header (never include from guest/, never add to guest_context.hpp).
#include <cstdint>
#include <string>

namespace sr {
namespace kernel {
namespace vpad {

// true = the SDL joystick device path (SDL3 SDL_GetJoystickPathForID) is a ViGEm virtual pad with
// a ViGEm target VID/PID (045E:028E Xbox 360, 054C:05C4 DualShock 4). *why gets the device
// chain (accepted) or the reason (rejected). Never faults (no device is opened). Windows only;
// false elsewhere.
bool isVigemPad(const char* sdlPath, uint16_t vid, uint16_t pid, std::string* why);

// Test (difftest --vpad-filter-test): run the filter over EVERY present HID device interface,
// each claimed to be 045E:028E (so VID/PID alone would pass), printing the verdicts. Returns the
// number of devices accepted that are not on the ViGEm bus (must be 0); *accepted gets the
// number accepted.
int scanForTest(int* accepted);

}  // namespace vpad
}  // namespace kernel
}  // namespace sr
