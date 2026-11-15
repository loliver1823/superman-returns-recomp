#pragma once
// iter-463 PLATFORM FIX (SR_XAM_PROFILE, default ON; =0 keeps the generic stub):
// XamUserReadProfileSettings, the console's read of the signed-in profile's settings.
//
// The title asks for 4 dashboard settings at sign-in and at every NEW GAME
// (ProfileSettings_initRead_sub_822A4238 size query, ProfileSettings_asyncRead_sub_822A4548,
// consumed by ProfileSettings_applyOptions_sub_822A45D0): 0x10040023 AUTO_CENTER,
// 0x10040024 MOVEMENT_CONTROL, 0x10040002 YAXIS_INVERSION, 0x10040003 CONTROLLER_VIBRATION.
// The generic stub returned 0 and filled nothing: the size query left the result buffer
// size at 0 and applyOptions read whatever the zero-size allocation held (so the natural
// game ran with vibration OFF, where Xenia's profile default is 3 = ON).
//
// Port of xenia-canary @0b4e6c4 xam_user.cc XamUserReadProfileSettingsEx (BSD-3, Copyright
// Ben Vanik and Xenia contributors): the same argument checks, size query
// (X_ERROR_INSUFFICIENT_BUFFER 0x7A + *pSize when it is 0), result layout
// (X_USER_READ_PROFILE_SETTINGS {count, ptr} + 40-byte X_USER_PROFILE_SETTING per valid id,
// unknown ids dropped from the count), user checks and overlapped completion (result 0,
// extended error 0, return 997). Values are Xenia's defaults for a fresh profile
// (user_settings.h default_setting_values + GetDefaultSetting): CONTROLLER_VIBRATION = 3,
// every other INT32 = 0, source DEFAULT (1). Not ported: a persistent dashboard profile
// (Xenia's GPD); the title imports no XamUserWriteProfileSettings, so nothing here is ever
// written by the game. The custom front end's universal options (docs/CUSTOM_FRONTEND.md
// section 0) are applied through the game's own SettingsAdapter, not through this call.
//
// The guest makes every decision; nothing here calls a guest function. Kernel-only header.
#include <cstdint>
#include <string>

namespace sr {
namespace kernel {

class ExportRegistry;

// SR_XAM_PROFILE (default ON).
bool xamProfileEnabled();
// Test seam: -1 = follow the env, 0 = force off (the generic stub's behaviour), 1 = on.
void xamProfileSetEnabledForTest(int mode);

// Called after registerStubs() registered the generic stub: replaces
// XamUserReadProfileSettings (a no-op when SR_XAM_PROFILE=0 at startup).
void registerXamProfileExports(ExportRegistry& reg);

// The export body (r3 title, r4 user, r5 xuidCount, r6 xuids, r7 settingCount,
// r8 settingIds, r9 pBufferSize, r10 buffer, [r1+0x54] pOverlapped), with the three
// register/stack arguments passed explicitly for tests.
uint32_t xamReadProfileSettings(uint32_t titleId, uint32_t userIndex, uint32_t xuidCount,
                                uint32_t xuidsVA, uint32_t settingCount, uint32_t idsVA,
                                uint32_t pSizeVA, uint32_t bufferVA, uint32_t overlappedVA);

// iter-464 PLATFORM FIX (SR_XAM_USERNAME, default ON; =0 keeps the old stub: "Player" for user 0,
// return 1 for any other index): XamUserGetName as Xenia implements it (xenia-canary
// xam_user.cc XamUserGetName_entry, BSD-3): index >= 4 -> X_ERROR_INVALID_PARAMETER (0x57);
// a user that is not signed in (every index but 0 here) -> byte 0 of the buffer cleared,
// X_ERROR_NO_SUCH_USER (0x525); user 0 -> the gamertag copied truncating to the buffer
// (copy_truncating: at most len-1 chars + NUL), X_ERROR_SUCCESS.
// The gamertag is the player's: <save root>/settings/profile.ini, line `gamertag=<name>`
// (1..15 printable ASCII characters; anything else, or no file, = "Player", the old value).
// A save stores the profile name it was made under (16 bytes at *0x8296F21C+1, sub_82164F08)
// and SAVE compares it with XamUserGetName (sub_822B4D08): an imported Xenia save made under
// another gamertag fails with "...another gamer profile's save file" until the gamertag
// matches (tools/import_xenia_saves.py --gamertag reads it from the save).
bool xamUserNameEnabled();
std::string xamProfileGamertag();
uint32_t xamUserGetName(uint32_t userIndex, uint32_t bufferVA, uint32_t bufferLen);

}  // namespace kernel
}  // namespace sr
