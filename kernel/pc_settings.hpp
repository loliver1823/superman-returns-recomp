#pragma once
// iter-466 gfx (PORT FEATURE, docs/GRAPHICS_SETTINGS.md): THE PC SETTINGS MODEL - the options the
// PC SETTINGS screen shows (gfx/settings_ui.cpp draws it), their stores and how each one applies.
//
// The screen replaces the Apt DISPLAY OPTIONS page behind the same entries: MAIN MENU > SETTINGS >
// DISPLAY OPTIONS and in game PAUSE > OPTIONS (lane E L-c; the open request comes from
// kernel/display_menu.cpp; the overlay routing is the MODS library's, kernel/mod_library.hpp).
//
// Every option is a HOST setting: it changes how the host draws, presents, sounds or reads input,
// never a game decision or guest state - except the GAME and CHEATS tabs (lane E L-b): the game's own
// options and cheats, changed only through the game's own functions on the game's thread
// (kernel/game_options.hpp, kernel/cheats_menu.hpp), never by this model or the screen. Stores (all universal: the front end, the pause menu and a
// hand edit see the same value):
//   <save root>/settings/display.ini   display, graphics, audio-host and world keys
//   <save root>/settings/input.ini     controls keys (the controls / input lanes' file)
// Keys owned by another lane (upscale_filter / sharpness: res4k;
// population_density / traffic_density: density, in game_options.ini) are drawn and stored here; the owning lane registers the feature as
// available (setAvailable) and, for a live option, an apply hook (onLive). Until then the row is
// not listed (no dead controls; a tab with no rows is not shown). A row that is unavailable for a
// reason of the moment (one monitor; refresh rate outside exclusive fullscreen) is shown disabled
// with that reason.
//
// Kernel-only header (never include from guest/, never add to guest_context.hpp).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sr {
namespace kernel {
namespace pcsettings {

// lane E L-b (step 5, STEP5_PLAN 10): the screen is OPTIONS; its tab strip in this order (the owner's q14d):
// GAME (the game's own options, kernel/game_options.hpp) first, CHEATS (the game's own cheats, in game only) last.
enum class Tab : uint8_t { Game = 0, Controls, Display, Graphics, World, Audio, Cheats, Count };
const char* tabName(Tab t);

enum class Kind : uint8_t { Choice = 0, Slider, Toggle, Action,
                            // iter-466 controls2: a key binding row ("bind_<Action>", the value is
                            // input.ini's "Mouse1, Tab" text; the screen captures a key / button)
                            Binding };
// How a change takes effect: at once (the screen previews it live), at the next start, or at once
// behind a keep-or-revert countdown (display mode, resolution, monitor, refresh rate).
enum class Apply : uint8_t { Live = 0, Restart, Confirm };

struct Choice {
  std::string value;   // the stored text
  std::string label;   // what the drop-down shows
};

struct Option {
  std::string id;          // the store key
  Tab tab = Tab::Display;
  std::string section;     // the header this row sits under
  Kind kind = Kind::Choice;
  Apply apply = Apply::Live;
  std::string label;
  std::string desc;        // what it does
  std::string perf;        // NONE | LOW | MEDIUM | HIGH (the performance cost)
  std::string xbox;        // what the Xbox 360 did
  std::vector<Choice> choices;                 // Kind::Choice
  float lo = 0, hi = 1, step = 1;              // Kind::Slider
  int decimals = 0;
  std::string unit;                            // "%", "" ...
  std::string defaultValue;
  bool available = true;
  std::string unavailableWhy;
  bool presetManaged = false;                  // a preset sets it
  bool hideUnavailable = false;                // not listed until its lane registers it
  std::string unavailableBadge;                // the row's short badge while unavailable
};

// The catalogue, in screen order (rebuilt per call: monitor modes, devices and availability are
// live). Pure apart from reading the registry and the platform's display list.
std::vector<Option> catalogue();
const Option* find(const std::vector<Option>& cat, const std::string& id);

// ── values ────────────────────────────────────────────────────────────────────────────────
// The value in effect now (the store's, or the live preview's while the screen previews it).
std::string value(const std::string& id);
// Apply a value for the screen's live preview WITHOUT storing it (Apply::Live / Confirm only).
bool preview(const std::string& id, const std::string& v, std::string* err = nullptr);
// Store a value (and apply it when the option is live). false + *err when refused.
bool commit(const std::string& id, const std::string& v, std::string* err = nullptr);
// The value the process started with (restart-only options: does a change need a restart?).
std::string bootValue(const std::string& id);

// ── presets ───────────────────────────────────────────────────────────────────────────────
// ORIGINAL (the Xbox 360 look), LOW, MEDIUM, HIGH, ULTRA. Values for the preset-managed options.
const std::vector<std::string>& presetNames();
std::vector<std::pair<std::string, std::string>> presetValues(const std::string& preset);
// Which preset the given values match ("DEFAULT" when every managed value is the default,
// "CUSTOM" when none matches).
std::string matchPreset(const std::function<std::string(const std::string&)>& valueOf);

// ── the registry other lanes use ──────────────────────────────────────────────────────────
// A lane that implements an option marks it available (call at init, any thread).
void setAvailable(const std::string& id, bool available);
// A live option's apply hook: called with the new value on preview and commit (render or UI
// thread; must be quick and thread-safe). Without one a live option of another lane is stored
// only.
void onLive(const std::string& id, std::function<void(const std::string&)> fn);
// An action row (Kind::Action): the owner registers what it does. Returns a message
// for the screen's toast.
void onAction(const std::string& id, std::function<std::string()> fn);
bool runAction(const std::string& id, std::string* msg);

// ── displays (the platform publishes them; the RESOLUTION / MONITOR / REFRESH rows read them) ─
struct DisplayMode {
  uint32_t w = 0, h = 0, hz = 0;
};
struct DisplayInfo {
  std::string name;
  DisplayMode desktop;
  std::vector<DisplayMode> modes;   // distinct, largest first
};
void noteDisplays(const std::vector<DisplayInfo>& displays, int current);
std::vector<DisplayInfo> displays();
int currentDisplay();

// ── state other threads read ──────────────────────────────────────────────────────────────
// The value the store holds (no live preview).
std::string storedValue(const std::string& id);
// Drop one / every live preview; revertPreviews re-applies the stored values of live options.
void clearPreview(const std::string& id);
void revertPreviews();
// Host audio (kernel/apu/audio_system.cpp): the output gain 0..1 (MASTER VOLUME, MUTE IN
// BACKGROUND), the output devices it found and the one the settings chose ("" = the default).
float audioGain(bool windowFocused);
void noteAudioDevices(const std::vector<std::string>& names);
std::vector<std::string> audioDevices();
// The stored OUTPUT DEVICE for a player run; "" for an automated run (SR_INSTANCE_ID, unless SR_PORT_FEATURES=1)
// - lane E E0, the harness audio safety rule (kernel/apu/audio_system.hpp chooseAudioDevice).
std::string audioDeviceSetting();
// The stored value itself, whatever the run (difftest mutants only).
std::string audioDeviceStored();
// The performance overlay: 0 off, 1 fps, 2 full.
int perfOverlayMode();
// The density sliders' maximum (the density lane may lower it after its stability soak).
constexpr int kDensityMax = 200;

// iter-466 controls2: the binding rows. The action of a "bind_<Action>" id (-1 = not one).
int bindingAction(const std::string& id);

std::string upper(std::string s);
// Pure helpers (unit-tested by selfTest).
// The render resolution label for an internal scale against the output height.
std::string renderResolutionLabel(uint32_t scale, uint32_t outW, uint32_t outH);
int selfTest();

}  // namespace pcsettings
}  // namespace kernel
}  // namespace sr
