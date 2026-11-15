#pragma once

// iter-465 window (PORT FEATURE, harmless on every run): the window's title-bar and taskbar
// icon is the GAME'S OWN title icon, as Xenia shows it. Xenia (src/xenia/emulator.cc,
// GameInfoDatabase::GetIcon -> SpaInfo::title_icon) reads the title's XDBF resource from the
// XEX and takes image entry 0x8000 (a PNG). We read the same resource from the player's own
// default.xex at load time (kernel/xex_loader.hpp xexTitleDatabase - no game bytes ship) and
// set it here. Until then, or if it cannot be read, the window keeps the exe's own neutral
// icon (app/app_icon.rc), which SDL's window class takes as its default.

namespace sr {

// Call from the window's thread (the event pump). Cheap: one atomic load per call until the
// icon is applied or the attempt is given up, then nothing.
void windowIconTick(void* sdlWindow);

}  // namespace sr
