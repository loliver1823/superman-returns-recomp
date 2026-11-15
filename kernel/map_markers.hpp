// iter-466 map lane: the USEFUL PAUSE MAP (docs/MAP.md). Kernel-only header (never include from
// guest/, never add to guest_context.hpp).
//
// The pause map (Apt movie SCRMAP) is the game's 3D city seen through its map camera; retail marks
// only Superman. The `pause_map` port feature patches SCRMAP with a generic marker renderer
// (_global.SRMap) and this module feeds it through the FSCommand bridge (SR_Map, mod_bridge.cpp):
// what to mark comes from MAP DEFINITIONS (the port feature's map.json and every enabled mod's
// "map" data, one schema, docs/MAP.md 4.1); every status is READ from guest memory (DAS values,
// stats, objective triggers, Lua globals) and positions are projected with the game's own camera
// transform (PauseMap_WorldToStage_sub_822A6A28). Observe-only: no guest call, no guest write.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sr {
namespace kernel {

struct JsonValue;
struct ModReply;

namespace mapmk {

// ---------------------------------------------------------------- the definition schema
struct Value {
  enum class Kind { Const, Das, Bit, Stat, TriggerAvailable, TriggerEngaged, Lua, All, Any, Not, Ge };
  Kind kind = Kind::Const;
  std::string name, flow;   // Das / Bit (prefix) / Stat / Lua name; flow "" = BaseWorldFlow
  int64_t index = 0;        // Bit index, trigger objectiveIndex
  double num = 0;           // Const value, Ge threshold
  std::vector<Value> kids;  // All / Any / Not / Ge
};

struct Text {
  enum class Kind { None, Literal, Key, TriggerText };
  Kind kind = Kind::None;
  std::string s;            // literal text, a text.AST key, or the trigger DAS string attribute
};

struct Readout {            // "record" / "target"
  bool present = false;
  Value value;
  std::string format = "count";   // time_ms | score | count
  std::string label, elseLabel;
  int when = 0;                   // "when": 0 always | 1 completed (else_label otherwise) | 2 pending
};

struct EachTrigger {
  std::vector<std::string> types;  // triggerType values (TYPE_OBJECTIVE ...); empty = any
  std::string flow;                // objectiveWorldFlowName; "" = any
  std::string filter, notFilter;   // require / exclude membership of OBJ|TRIGGERS|<filter> (METROEVENT)
  int available = -1, engaged = -1;  // -1 any, 0 / 1 required; available 1 also keeps the engaged one
};

struct MarkerDef {
  std::string id, type, source;   // source = the layer (port feature / mod id) that defined it
  bool hasPos = false;
  std::array<float, 3> pos{};
  int64_t trigger = -1;           // position (and default name) of this live objective trigger
  bool each = false;
  EachTrigger eachTrigger;
  Text name, detail;
  bool hasShow = false, hasCompleted = false;
  Value show, completed;
  Readout record, target;
  std::string completedLabel, pendingLabel;
  bool keep = false;              // after_complete: "keep" (replayable) | "hide" (story beat)
  std::string becomes;            // replaced by this marker id once completed
  std::vector<std::array<float, 3>> route;   // a track drawn while the marker is selected (start .. finish)
};

struct TypeDef {
  std::string id, label, icon = "ring", source;   // icon: ring | reticle | superman | dot | pulse
  std::string glyph;              // up to 3 characters drawn on the icon (">>", "B", "!")
  uint32_t color = 0xFFFFFF;
};

struct ViewDef {
  std::string kind = "game";      // game | art
  std::string movie;
  std::array<float, 4> rect{};    // art: world X at the image's left edge, Z at its top, X right, Z bottom
  float y = 0;                    // art: the ground height the rectangle is projected at
  int w = 1024, h = 1024;         // art: the movie's own size in stage pixels
};

struct MapDef {
  std::string id, world, name, source;
  bool hasView = false;
  ViewDef view;
  std::vector<TypeDef> types;
  std::vector<MarkerDef> markers;
};

struct Definition {
  std::vector<MapDef> maps;
};

// Parse + validate one definition (the port feature's map.json, a mod's "map" object or its
// map.json). false with *err = "maps[0].markers[3] (race.96): ..." on the first problem.
bool parseDefinition(const JsonValue& root, const std::string& source, Definition* out, std::string* err);
bool parseDefinitionText(const std::string& text, const std::string& source, Definition* out, std::string* err);

// One definition layer: lowest priority first (the port feature, then mods low -> high).
struct Layer {
  std::string id;
  std::shared_ptr<const Definition> def;
};
// A maps[] entry naming an existing map id WITHOUT "view" adds its types and markers to it (a
// type or marker id it repeats replaces the earlier one); WITH "view" it replaces that map. notes
// gets one line per replacement ("map metropolis: <winner> replaces <loser>").
std::vector<MapDef> mergeLayers(const std::vector<Layer>& layers, std::vector<std::string>* notes);

// ---------------------------------------------------------------- evaluation
struct TriggerInfo {
  int64_t idx = -1;
  std::string flow, type, nameKey, textKey;
  bool available = false, engaged = false, metro = false, hasPos = false;   // metro: in OBJ|TRIGGERS|METROEVENT
  std::array<float, 3> pos{};
};

// What the evaluator reads from the game (the live implementation reads guest memory; tests fake it).
struct GameReader {
  virtual ~GameReader() = default;
  virtual std::optional<int64_t> das(const std::string& name, const std::string& flow) = 0;
  virtual std::optional<double> stat(const std::string& name) = 0;
  virtual std::optional<double> lua(const std::string& name) = 0;
  virtual const std::vector<TriggerInfo>& triggers() = 0;
  virtual std::vector<std::string> worlds() = 0;   // loaded world-flow names
  virtual bool objectiveRunning() = 0;             // objectiveActivated != 0
};

enum MarkerState : uint32_t {
  kCompleted = 1u << 0,
  kCurrent = 1u << 1,     // the engaged objective
};

struct Marker {
  std::string id, type;
  int typeIndex = 0;
  uint32_t state = 0;
  std::string title, desc, desc2;   // a leading '@' = a text.AST key the screen localizes
  std::string detail, status, readout;   // the selection box lines (detail may be '@key')
  std::vector<std::array<float, 3>> route;
  bool hasPos = false;
  std::array<float, 3> pos{};
};

struct Evaluation {
  bool available = false;     // a map matched the loaded world
  std::string mapId;
  ViewDef view;               // the matched map's view (kind "art": the mod's map art)
  std::vector<TypeDef> types;
  std::vector<std::string> typeLabels;   // legend text incl. counts ("KITTENS 37/100")
  std::vector<int> typeDone, typeTotal;  // the legend's count column (total -1 = the type has no count)
  std::vector<Marker> markers;
  std::vector<std::string> warnings;     // sources that could not be read
};
Evaluation evaluate(const std::vector<MapDef>& maps, GameReader& g);

std::string formatTimeMs(double ms);       // "1:02.35"
std::string formatScore(double v);         // "1,250,000"

// ---------------------------------------------------------------- the game's projection
// PauseMap_WorldToStage_sub_822A6A28: the live camera frame V = *(*0x8296D86C + 0x28).
struct Camera {
  float tanHalf = 1, aspect = 1;
  std::array<float, 3> pos{};
  std::array<std::array<float, 3>, 3> rows{};   // rotation rows (world matrix rows 0..2)
  float W = 1280, H = 720;
};
Camera cameraFromEuler(float fovOrTan, bool tanDirect, float aspect, const std::array<float, 3>& pos,
                       const std::array<float, 3>& euler, float W, float H);
bool project(const Camera& c, const std::array<float, 3>& p, float* sx, float* sy);  // false: behind
int32_t packXY(float sx, float sy);        // the host's change-detection key; -1 = no position
// SR_MapXY / SR_MapRtXY / SR_MapArt carry x + kWireOff, y + kWireOff (always > 0); kNoPos = none
constexpr int32_t kWireOff = 32768;
constexpr const char* kBuiltinLayer = "pause_map";   // the port feature's own map.json (its layer id)
constexpr int32_t kNoPos = 0;

// Lua 5.0.2 luaS_hash (the game's VM, docs/MAP.md 4.3).
uint32_t luaStringHash(const std::string& s);

// ---------------------------------------------------------------- the bridge
// SR_Map <verb> [args]: "open" (evaluate + describe the markers), "tick <sx> <sy>" (positions;
// sx/sy = what the game passed to ScrMap.SetSupermanIcon, for the self-check), "filter <type> 0|1",
// "close". Replies are FlowMgr.SR_Map* calls (docs/MAP.md 4.4).
void mapCommand(const std::string& args, std::vector<ModReply>* replies);

// Test seam: the live Lua-global reader over fake guest memory (page -> 4096 bytes or nullptr).
std::optional<double> luaGlobalOver(const std::function<const uint8_t*(uint32_t page)>& page, const std::string& name);

// Test seams: a reader + camera in place of guest memory; nullptr restores the live ones.
void mapSetTestReader(GameReader* reader, const Camera* cam, const std::vector<MapDef>* maps);

}  // namespace mapmk
}  // namespace kernel
}  // namespace sr
