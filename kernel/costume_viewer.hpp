#pragma once
// iter-465 viewer lane: THE COSTUME VIEWER (docs/MODS.md "Costume viewer"). OWNER 2026-10-05: "a new
// costume viewer ... a 3D model of the suit and cape doing an idle flight pose", laid out like the
// Spider-Man (2000, PS1) COSTUME VIEWER and built from the game's own UI pieces.
//
// The page is the game's own SCRCOSTUMES Apt screen, re-laid out by the custom front end's
// SRPATCH (list panel, info panel, legend - the game's own parts). Its centre is left open and
// this host overlay draws the selected suit there: the suit's own skinned body, textures and cape
// (kernel/costume_viewer_model.cpp, read from the player's own disc or the pack's files), posed in
// its flight bank's Hover clip, slowly turning. The Apt screen keeps the input and the game's own
// WEAR / BACK path; it tells the host which suit is selected (SR_CostumeViewer). The right stick
// and the mouse turn and zoom the suit (no buttons). While a popup is up the model is hidden (Apt draws under the overlay).
//
// Charter: a PORT FEATURE (player-facing host UI). It reads no guest memory, writes none and calls
// no guest code. Gate: SR_COSTUME_VIEWER (unset = the native costume screen's gate, so natural
// rolls keep the retail screen). Kernel-only header.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sr::kernel {
namespace cview { struct Model; }

bool costumeViewerEnabled();
// One SR_CostumeViewer command from the Apt page: "show <costume id> <locked 0|1>", "hide" (a popup
// is up), "close", "key <code>" (an input passed on to the retail page; logged).
void costumeViewerCommand(const std::string& args);

// The page's CHARACTER / COMMENTS lines per suit id (retail/1..3, mod and built-in suits): the
// pack's optional "character" (mod.json), Bizarro for the built-in, else SUPERMAN; the comment is
// the pack's whole description ("" = the record's own).
struct CostumeViewerInfo { std::string id, character, comment; };
std::vector<CostumeViewerInfo> costumeViewerInfo();

// The right stick the game read this poll (kernel/stubs.cpp XamInputGetState, after every merge):
// X turns the suit, Y zooms.
void costumeViewerNotePad(int16_t rx, int16_t ry);
// Mouse: a left-button drag turns (left / right) and zooms (up / down), the wheel zooms too
// (players only; inert while closed).
// Takes an SDL_Event* (void* keeps SDL out of this header).
void costumeViewerOnSdlEvent(const void* sdlEvent);

struct CostumeViewerFrame {
  bool visible = false;
  std::shared_ptr<const cview::Model> model;   // null while loading / on failure
  std::string id;
  bool silhouette = false;                     // a locked suit
  double seconds = 0;                          // clip time
  float yaw = 0, pitch = 0.06f, zoom = 1.0f;   // radians, radians, 1 = full body
  float rect[4] = {0.330f, 0.125f, 0.670f, 0.95f}; // the page's open centre (0..1 of the 16:9 image)
};
// Called by the renderer once per presented frame (advances time, input smoothing).
CostumeViewerFrame costumeViewerFrame();

}  // namespace sr::kernel
