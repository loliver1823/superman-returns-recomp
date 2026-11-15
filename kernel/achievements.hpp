// step5 E14a (lane B, 2026-10-23): the ACHIEVEMENTS CORE, no UI. The owner's E14 (approved 10-23 ~13:30; mock-up
// archive/s5_pcfeatures/mockups/q14_achievements.png; STEP5_PLAN section 9). E14b (lane E) draws the pause-menu page
// from the read API below; this unit only records, stores and answers.
//
// WHAT IT DOES
// - THE HOOK. The game's own achievement write is XMsgStartIORequest(app 0xFB XGI, msg 0x000B0008
//   XUserWriteAchievements, overlapped r5, buffer r6 = {be u32 count, be u32 ptr -> XUSER_ACHIEVEMENT {be u32 user,
//   be u32 id}[count]}, length r7 = 8) (wrapper sub_82467DC0; logic-audit row B3). Today that import is the generic
//   stub (kernel/stubs.cpp REG_GENERIC_STUB: returns 0, writes nothing). This handler, for 0xFB / 0xB0008 ONLY, reads
//   the ids fault-free, records those in the title's XACH table (others are logged and dropped), and then returns
//   EXACTLY what the previous registration returns (it calls it): the guest sees no change. Every other app / message
//   goes to the previous registration untouched. No trigger logic of ours: the game decides when to write
//   (Achievements_CheckUnlockConditions @0x822BB3E0 and its callers).
// - THE TABLE. The title's XDBF (SPA), copied from the player's default.xex at load (kernel/xex_loader.hpp
//   xexTitleDatabase; nothing ships): section 1 'XACH' (id, name / unlocked / locked string ids, image id,
//   gamerscore, flags), 'XSTC' (the default language), 'XTHD' (the title id), section 3 the XSTR string tables per
//   language. The layout follows Xenia (src/xenia/kernel/xam/xdbf/xdbf_io.h, spa_info.h; BSD-3-Clause, CREDITS.md).
//   LOCALE: the language the game's own UI uses. The game asks XGetLanguage, which is a generic stub here (answers 0,
//   not a language), so the game's text follows its default; this unit uses the XDBF's own default language (XSTC),
//   the same source. A future XGetLanguage answer with a string table would be used instead (languageForTest seam).
// - TIED TO EACH SAVE. One session set {id -> unlock time}. The events come from the current-save-slot tracker
//   (kernel/mod_bridge.hpp SR_SaveEvent; docs/CUSTOM_FRONTEND.md 5.6: the custom front end sends them at NEW GAME,
//   LOAD and SAVE with the slot the game's own save flow used, saveFlow+0x54):
//     boot                -> the empty set, no slot bound (SaveStatus::NoSave);
//     an unlock           -> added to the set (the first time only, with the host time);
//     SAVE of slot n      -> the sidecar's slot n := the CURRENT set (this session's unlocks included) + slot n's
//                            stored checksum from the package's save file; slot n is bound. It REPLACES whatever slot
//                            n had, as the save itself does;
//     LOAD of slot n      -> the set := the sidecar's slot n record if its stored checksum equals slot n's in the
//                            package (else empty: never guessed); slot n is bound. Unsaved unlocks are DROPPED, like
//                            unsaved progress;
//     NEW GAME            -> the empty set, no slot bound.
//   The sidecar: <TYPE dir>/<package>.achievements.json BESIDE the package folder (like `.devtool`, never inside it),
//   written atomically (a temporary file, then a rename) after the game's own write completed:
//     {"v": 1, "title_id": "454107ED", "slots": {"0": {"slot_checksum": "1A2B3C4D", "unlocked": {"11": 1729670000000}},
//      "1": null, "2": null}}   (unlock times: Unix ms, UTC)
//   A missing sidecar = empty records. A corrupt one, another title's, or a bad version = empty records and one
//   [ACH] line: never a crash, and it NEVER touches the save package (the sidecar is only ever replaced whole).
//   Achievement ids are canonical decimal keys ("11", never "011"), each at most once per slot; anything else makes
//   the file unreadable (so a later SAVE never re-writes an aliased key twice).
//   TIMING (the review, documented): SAVE reads slot n's checksum from the save file when the 'save' event arrives,
//   with no completion handshake of its own. The custom front end sends 'save' after the game's own
//   SAVE SUCCESSFUL (Main_Hud / the autosave popup hook), i.e. after the write completed, so the checksum is the one
//   just written (the E14a live roll: 40C3B900 both ways). A 'save' sent before the write completes would bind the
//   set to the OLD checksum; the next LOAD would then find a mismatch and show the slot empty (SlotUnreadable),
//   never a wrong set. An unlock that lands while SAVE's own file I/O runs stays in memory and goes into the next
//   SAVE. A repeated 'save' for the same slot rewrites the same record (harmless).
//   LOAD / SAVE act on the slot the EVENT names (mod_bridge modSlotEvent's eventSlot), never the tracker's
//   remembered slot: a LOAD whose +0x54 is out of range changes nothing (the review's P2).
// - CHEATS NEVER BLOCK. Nothing here reads any cheat state; every write the game makes is recorded.
//   (Whether the game's own CheckUnlockConditions withholds a write after a retail cheat is the game's rule, not
//   this unit's; STEP5_PLAN 9 "Open".) Never calls the game's own reset; never writes guest memory.
// - [ACH] log lines: the table at first use, each write, each SAVE / LOAD / NEW GAME, every ignored sidecar.
// GATE: SR_ACHIEVEMENTS (default ON, =0 = the old generic stub stays, nothing recorded or written).
// Tests: build/difftest.exe --achievements-test (fixtures, the real table when SR_ACH_XDBF=<file> is given).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sr::kernel {
class ExportRegistry;
}

namespace sr::ach {

// ------------------------------------------------------------------------------------------- the read API (E14b)
enum class TableStatus : uint8_t {
  Ok,            // the list is the title's XACH table
  Unavailable    // no XDBF, or it could not be read: show nothing
};
enum class SaveStatus : uint8_t {
  NoSave,          // no slot bound yet (title, before the first SAVE or LOAD, after NEW GAME): `unlocked` = this
                   // session's unlocks (they go into the slot at its first SAVE)
  Slot,            // bound to save slot `slot`: its record (empty if the slot had none)
  SlotUnreadable   // bound to `slot`, but its sidecar record was corrupt / foreign / for another save: ignored
                   // (empty set), logged; the save itself is untouched
};
struct Achievement {          // one XACH row, in the table's own (stable) order
  uint32_t id = 0;            // the id the guest writes
  std::string name;           // UTF-8 display name (XSTR, the language above)
  std::string unlockedText;   // the description shown once unlocked
  std::string lockedText;     // the game's own how-to text, shown while locked
  uint32_t gamerscore = 0;
  bool secret = false;        // the XACH "shown while locked" flag (0x8) is CLEAR: the console showed
                              // SECRET ACHIEVEMENT until unlocked. The table carries no placeholder text.
  uint32_t flags = 0;         // the raw XACH flags
  uint32_t imageId = 0;       // the XIMG id (section 2); the decoded icon accessor comes with E14b
};
struct Unlock {
  uint32_t id = 0;
  int64_t unixMs = 0;         // the host time (UTC) the guest first wrote it
};
struct Snapshot {
  TableStatus table = TableStatus::Unavailable;
  SaveStatus save = SaveStatus::NoSave;
  int slot = -1;                     // the bound save slot 0..2; -1 for NoSave
  uint64_t generation = 0;           // the value generation() had when this snapshot was taken
  std::vector<Achievement> list;     // the full list, stable order
  std::vector<Unlock> unlocked;      // the current save's set, in unlock order
};
// Any thread: a copy taken under the unit's lock; never a guest call, never a guest read.
Snapshot snapshot();
// lane E L-a: an achievement's icon - the title's own image (XDBF section 2, the entry `imageId` names; a PNG on the
// 360's discs), copied out of the title database. False when there is no table, no such image or it is not a PNG.
// Any thread; no guest read beyond the loader's copy of the XDBF.
bool iconPng(uint32_t imageId, std::vector<uint8_t>* png);
// Bumps on every change (an unlock, SAVE, LOAD, NEW GAME, the table becoming available). A lock-free atomic read:
// poll it once a frame, call snapshot() only when it changed.
uint64_t generation();

// ------------------------------------------------------------------------------------------- the kernel wiring
// Registers the XMsgStartIORequest handler (after the stubs, so the previous registration is the generic stub; never
// chains to itself when registerStubs runs twice). A no-op with SR_ACHIEVEMENTS=0.
void registerAchievementExports(sr::kernel::ExportRegistry& reg);
// The save-slot tracker's events (mod_bridge SR_SaveEvent, after its state machine): kind = new / load / save /
// overwrite / query, slot = the slot THIS event names (modSlotEvent's eventSlot: LOAD +0x54 if 0..2, SAVE +0x54 if
// 0..2 else the latched overwrite slot; -1 = none, and then nothing changes).
void onSaveEvent(const std::string& kind, int slot);
// The game's save package (xam_content XamContentCreateEx: created or opened, rc success): its host folder.
void notePackage(const std::filesystem::path& pkg);

// ------------------------------------------------------------------------------------------- pure parts (tested)
struct Table {
  bool ok = false;
  uint32_t titleId = 0;       // XTHD; 0 when absent
  uint32_t language = 0;      // the XSTR language the text came from
  std::vector<Achievement> list;
  std::string why;            // when !ok
};
// wantLanguage 0 = the XSTC default; a language without a string table falls back to the default, then to the first.
Table parseTable(const std::vector<uint8_t>& xdbf, uint32_t wantLanguage);
// lane E L-a: the bytes of image `imageId` (section 2) in an XDBF; false unless present and a PNG.
bool imageFromXdbf(const std::vector<uint8_t>& xdbf, uint32_t imageId, std::vector<uint8_t>* png);

struct SlotRecord {
  bool present = false;
  uint32_t slotChecksum = 0;
  std::vector<Unlock> unlocked;
};
struct Sidecar {
  uint32_t titleId = 0;
  SlotRecord slots[3];
};
// False (and *why) for anything but a well-formed version-1 sidecar of `titleId`.
bool parseSidecar(const std::string& text, uint32_t titleId, Sidecar* out, std::string* why);
std::string sidecarJson(const Sidecar& s);
std::filesystem::path sidecarPath(const std::filesystem::path& pkg);
// The package's save file (<pkg>/Superman Returns, the 'MC02' container; docs/SAVES.md): slot n's stored checksum
// (region n, record 0, +8). False unless the container is well formed and the slot is not empty (progress > -1).
bool slotChecksumFromSave(const std::vector<uint8_t>& file, int slot, uint32_t* out);

// ------------------------------------------------------------------------------------------- test seams
void resetForTest();
void setTableForTest(const std::vector<uint8_t>& xdbf);   // instead of xexTitleDatabase
void setLanguageForTest(uint32_t language);               // instead of the XGetLanguage answer (0)
void setClockForTest(int64_t unixMs);                     // a fixed clock (0 = the real one)
bool guestSpanForTest(uint32_t va, uint32_t len);         // the hook's own fault-free span check

}  // namespace sr::ach
