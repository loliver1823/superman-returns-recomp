#pragma once
// iter-463 phase 2 (docs/DEVTOOLS_DESIGN.md 5.4, 5.5, 10 steps 5-7): the level-skip
// executor's CORE in C++ - the command-language parser, the pure guest reads (World),
// the step machine (Executor) and the differential report. The third implementation of
// ONE semantics:
//   tools/devtools/skipexec.py   the REFERENCE (built on tools/objstate.py's ports)
//   tools/devtools/frida_skip.js the Frida executor (Xenia oracle side)
//   this file                    the kernel executor (kernel/devtools_levelskip.cpp)
// Every function mirrors skipexec.py / frida_skip.js line for line (retail addresses
// are cited there). The differential is tools/devtools/test_skipcore_cpp.py: this code,
// compiled into tools/devtools/skipcore_host.cpp, must give skipexec's report, parse
// and simulation results on the golden dumps, the shared corpus and a seeded fuzz.
//
// Pure C++: no Windows, no kernel headers, no guest calls. Guest memory is read through
// a PageSource (the exe's checks every page fault-free with hostRangeReadableCached;
// the test host serves golden pages) and written only through the Executor's write
// callback (the exe's runs on the game tick thread, at the drain point).
//
// Errors are C++ exceptions of the types below, caught exactly where the reference
// catches them. The harness VEHs claim only access violations and single-steps
// (image_write_watchdog, hw_watchpoint), so a C++ throw never reaches them as a fault;
// and no read here can fault: an unreadable page is a ReadErr, never a touch.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sr {
namespace devtools {
namespace skip {

// ============================================================== JSON value ====
// The differential object (and the exe's event / [OBJSTATE] lines). Objects keep
// insertion order; equality is the comparing side's job (Python parses the dump).
class J {
 public:
  enum class T : uint8_t { Null, Bool, Int, Dbl, Str, Arr, Obj };
  J() = default;
  static J null() { return J(); }
  static J boolean(bool b) { J j; j.t_ = T::Bool; j.b_ = b; return j; }
  static J num(int64_t v) { J j; j.t_ = T::Int; j.i_ = v; return j; }
  static J dbl(double v) { J j; j.t_ = T::Dbl; j.d_ = v; return j; }
  static J str(std::string s) { J j; j.t_ = T::Str; j.s_ = std::move(s); return j; }
  static J arr() { J j; j.t_ = T::Arr; return j; }
  static J obj() { J j; j.t_ = T::Obj; return j; }

  T type() const { return t_; }
  bool isNull() const { return t_ == T::Null; }
  bool isBool() const { return t_ == T::Bool; }
  bool isInt() const { return t_ == T::Int; }
  bool isNum() const { return t_ == T::Int || t_ == T::Dbl; }
  bool isStr() const { return t_ == T::Str; }
  bool isArr() const { return t_ == T::Arr; }
  bool isObj() const { return t_ == T::Obj; }
  bool b() const { return b_; }
  int64_t i() const { return t_ == T::Dbl ? static_cast<int64_t>(d_) : i_; }
  double d() const { return t_ == T::Int ? static_cast<double>(i_) : d_; }
  const std::string& s() const { return s_; }
  const std::vector<J>& a() const { return a_; }
  std::vector<J>& a() { return a_; }
  const std::vector<std::pair<std::string, J>>& o() const { return o_; }

  J& set(const std::string& k, J v);           // replaces an existing key in place
  const J* get(const std::string& k) const;    // nullptr when absent
  bool has(const std::string& k) const { return get(k) != nullptr; }
  void push(J v) { a_.push_back(std::move(v)); }
  size_t size() const { return t_ == T::Arr ? a_.size() : o_.size(); }
  std::string dump() const;

 private:
  T t_ = T::Null;
  bool b_ = false;
  int64_t i_ = 0;
  double d_ = 0;
  std::string s_;
  std::vector<J> a_;
  std::vector<std::pair<std::string, J>> o_;
};

// ============================================================== errors ====
struct ReadErr { std::string msg; };
struct UnknownErr { std::string msg; };
struct Refusal { std::string code; std::string why; };
struct Miss { uint32_t pg; };                  // test hosts only: the page was not supplied
struct PlanError { std::string msg; };

// ============================================================== constants ====
namespace C {
constexpr uint32_t G_WORLDFLOW_MGR = 0x8296DF74u, G_STATS = 0x8296DF70u, G_FILTER_REGISTRY = 0x8296DD08u,
                   G_ATOMS = 0x829A23B8u, OFF_NAME_OBJ_TRIGGERS = 0x82912090u;
constexpr uint32_t WFM_DEFS = 0x08, WFM_SLOTS = 0x20, WFM_CUR_DEF = 0x2C;
constexpr uint32_t FLOW_NAME = 0x0C, FLOW_NAME_HASH = 0x4C, FLOW_DAS = 0x68, FLOW_FSM = 0x6C;
constexpr uint32_t VT_FLOW_FSM = 0x8200E5E4u, FSM_CUR = 0x08, FSM_COUNT = 0x18, FSM_STATES = 0x1C;
constexpr uint32_t FSTATE_NTRANS = 0x08, FSTATE_TRANS = 0x0C, FTRANS_DEST = 0x04;
constexpr uint32_t DAS_CONTAINER = 0x04, NODE_VALUE = 0x04, NODE_FLAGS = 0x08;
constexpr uint32_t ACTOR_DAS = 0x80, ACTOR_GROUPS = 0x30, ACTOR_BRAIN = 0x84, BRAIN_ACTOR = 0x3C,
                   BRAIN_MACHINE = 0x1C;
constexpr uint32_t VT_TRIGGER_STATE = 0x82017790u, STATE_FLAGS = 0x04, STATE_DISTANCE = 0x14,
                   STATE_EVENT = 0x18;
constexpr uint32_t FOURCC_ACTR = 0x41435452u;
constexpr uint32_t STAT_NAME = 0x1C, STAT_VALUE_VTSLOT = 0x28, STAT_SETCAP_VTSLOT = 0x14;
constexpr uint32_t REACH_BITS = 0x4E6E6B28u, G_RUN_STATE_OBJ = 0x82911460u, RUN_STATE_OFF = 0x48,
                   RUN_STATE_POPUP = 4;
constexpr uint32_t STATE_IN_OBJECTIVE = 107, STATE_TRIGGERS = 1807139120u;
constexpr int64_t SETTLE_TICKS = 30, AWAIT_TIMEOUT = 5400;
constexpr uint32_t T_INT = 0, T_FLOAT = 1;
constexpr int NUM_BITS = 288;
// DT-2 dev-tool commands (iter-464; tools/devtools/skipexec.py has the citations)
constexpr uint32_t G_TIMER_MGR = 0x8296DF2Cu, MAP_ANCHOR = 0x04, MAP_BEGIN = 0x08, MAP_COUNT = 0x14;
constexpr uint32_t TN_RIGHT = 0x00, TN_LEFT = 0x04, TN_PARENT = 0x08, TN_OWNER = 0x10, TN_HASH = 0x14,
                   TN_FLOW = 0x18, TN_RUN = 0x25;
constexpr uint32_t MAX_TIMERS = 4096, RB_MAX_DEPTH = 64;
constexpr uint32_t G_OBJECTIVE_MGR = 0x8296DF5Cu, OBJ_MINIEVENTS = 0x04;
constexpr uint32_t G_METRO = 0x8296DF50u, METRO_HEALTH = 0x04, METRO_MAX = 0x08, METRO_FRAC = 0x10,
                   METRO_DAMAGE = 0x2C, METRO_LOCK = 0x2D, F32_ONE = 0x3F800000u;
}  // namespace C

// ============================================================== plan ====
struct Cond {
  std::string kind;                 // das | won | flow_loaded | trigger | stable | base_state |
                                    // pad_prompt | objective_failed
  std::optional<std::string> name;  // das name / flow name / trigger state
  std::optional<int64_t> value;     // das value / bit / trigger idx / stable ticks / state id
};
struct Step {
  std::string prim;
  std::optional<int64_t> idx;
  std::optional<std::string> flow;
  std::vector<std::string> buttons;
  std::optional<std::string> label;
  std::optional<std::vector<Cond>> when;   // nullopt = sugar (default guard)
  std::optional<int64_t> timeout;
  int line = 0;
  std::optional<int64_t> deadline;
  std::vector<std::vector<Cond>> abort;
  std::optional<std::string> arg;          // on|off of a DT-2 command (isArgPrim)
};
bool isArgPrim(const std::string& p);      // pause_timers | minievents | metro_lock | freecam
// skipplan.parse: UTF-8 text (a leading BOM dropped; Python's splitlines / split()
// whitespace sets). Throws PlanError.
std::vector<Step> parsePlan(const std::string& utf8);
std::vector<Cond> parseCondTokens(const std::vector<std::string>& toks, int line);  // one cond
std::vector<std::string> pySplit(const std::string& utf8);                          // str.split()
std::vector<Step> expandDefaults(const std::vector<Step>& steps);
std::vector<Cond> defaultGuard(const Step& st);
J condJson(const Cond& c);
J stepJson(const Step& s);
std::string condText(const Cond& c);

// ============================================================== memory ====
// One page: p == nullptr = unreadable; len 1..4096 (a short page ends where the source
// ends). The view only has to live until the source returns: Mem copies it.
struct PageView {
  const uint8_t* p = nullptr;
  uint32_t len = 0;
};
using PageSource = std::function<PageView(uint32_t pg)>;   // may throw Miss (test hosts)

class Mem {
 public:
  explicit Mem(PageSource src) : src_(std::move(src)) {}
  std::vector<uint8_t> raw(uint32_t va, uint32_t n);
  uint8_t u8(uint32_t va);
  uint32_t u32(uint32_t va);
  // objstate Mem.cstr: nullopt for a null pointer; a missing NUL within maxlen is ReadErr
  std::optional<std::string> cstr(uint32_t va, uint32_t maxlen = 256);
  // every page this Mem read, with the bytes it saw (the in-exe .snap writes exactly these)
  const std::map<uint32_t, std::optional<std::vector<uint8_t>>>& pages() const { return cache_; }

 private:
  const std::optional<std::vector<uint8_t>>& page(uint32_t pg);
  PageSource src_;
  std::map<uint32_t, std::optional<std::vector<uint8_t>>> cache_;
};

// The BaseWorldFlow graph (skipexec.base_graph_spec): state ids by index, each state's
// transition destination indices (-1 = None), and the loss sets (absent = None).
struct Graph {
  std::vector<uint32_t> ids;
  std::vector<std::vector<int64_t>> trans;
  bool hasFail = false;
  std::vector<uint32_t> failObjective, failGameOver;
};

uint32_t fnv1(const std::string& bytes);
uint32_t hash31(const std::string& bytes);
std::string h8(uint32_t v);

// DT-2: the dev-tool commands' state, shared by every executor of one process (the plan's and
// the immediate command channel's): what each 'on' leaves for its 'off' (skipexec.Executor.cheats).
struct CheatState {
  J timers = J::arr();                    // [{node, owner, hash}] that pause_timers on paused
  std::optional<int64_t> metroDamage;     // the damage byte metro_lock on cleared
};

// ============================================================== World ====
class World {
 public:
  World(PageSource src, const Graph* graph) : m_(std::move(src)), graph_(graph) {}
  Mem& mem() { return m_; }

  std::pair<uint32_t, uint32_t> base();                                   // (flow, das handle)
  // (node, value, type) or nullopt when the node is missing
  std::optional<std::tuple<uint32_t, uint32_t, uint32_t>> das(const std::string& name);
  std::optional<uint32_t> dasVal(const std::string& name);
  bool dasSorted();
  std::vector<int64_t> wonBits(const std::string& prefix = "objectiveStatus");
  bool won(int64_t bit);
  std::vector<std::string> loaded();                                      // latin1 -> UTF-8
  const J& triggers();                                                    // array of records
  bool triggerCond(int64_t idx, const std::string& what);
  J baseFsm();
  J xp();
  std::optional<uint32_t> runState();
  J failure();
  bool pastPromptPoint();
  bool runningIndex(int64_t idx, const std::optional<std::string>& flow);
  J storyKey();                                                           // array or null
  bool cond(const Cond& c);
  J resolve(const std::string& prim, std::optional<int64_t> idx, const std::optional<std::string>& flow,
            bool strict = true, const std::optional<std::string>& arg = std::nullopt,
            const CheatState* cheats = nullptr);                          // array of writes; throws Refusal
  // DT-2 reads (skipexec.World.timers / minievents / metro / cheats_report)
  J timers();
  J minievents();
  J metro();
  J cheatsReport();
  J resolveRestore(const J& pend);
  bool restoreDue(const J& pend);

  // ported lookups (objstate GameReader)
  uint32_t dasNode(uint32_t handle, const std::string& name);
  std::optional<std::string> dasStr(uint32_t handle, const std::string& name);  // UTF-8 of latin1

  // iter-466 map lane (kernel/map_markers.cpp; all reads): the members of the entity filter
  // named `names` ("OBJ|TRIGGERS|METROEVENT", "SUPERMAN"; empty when the filter does not exist),
  // an entity's ACTR component, a stat's value (its getter's lfd offset, as xp() decodes it), and
  // the DAS handle of the LOADED world flow called `name` (any slot, not only the current flow).
  std::vector<uint32_t> filterMemberList(const std::string& names);
  uint32_t actorOf(uint32_t entity) { return getComponent(entity, C::FOURCC_ACTR); }
  std::optional<double> statValue(const std::string& name);
  std::optional<uint32_t> flowDasByName(const std::string& name);

 private:
  uint32_t vecFind(uint32_t vec, uint32_t key);
  uint32_t resolvePath(uint32_t container, const std::string& path);
  std::optional<std::string> atom(uint32_t key);
  uint32_t flowMgr();
  uint32_t currentFlow();
  std::vector<uint32_t> statList();
  uint32_t findStat(const std::string& name);
  std::optional<uint32_t> decodeStatCap(uint32_t e);
  uint32_t getComponent(uint32_t obj, uint32_t fourcc);
  uint32_t hashLookup(uint32_t tbl, const std::string& tok);
  std::vector<uint8_t> buildMask(uint32_t nametbl, const std::string& names);
  uint32_t findFilter(uint32_t filterset, const std::vector<uint8_t>& mask);
  std::vector<uint32_t> filterMembers(uint32_t filt);
  void triggerRec(uint32_t mem, J& rec, const std::vector<uint8_t>& emask);
  J resolveImpl(const std::string& prim, std::optional<int64_t> idx, const std::optional<std::string>& flow,
                bool strict);
  J resolveCheat(const std::string& prim, const std::string& arg, const CheatState* cheats);
  uint32_t rbNext(uint32_t n);

  Mem m_;
  const Graph* graph_;
  std::optional<J> trig_;
  std::unordered_map<uint32_t, std::optional<std::string>> atomCache_;
};

// ============================================================== Executor ====
using WorldFn = std::function<World()>;
using WriteFn = std::function<void(const J& write)>;          // may throw (the exe: unwritable)
using ClockFn = std::function<std::optional<double>()>;       // wall-clock seconds, or none
using EventFn = std::function<void(const J& event)>;
// A guest-call command (freecam): the kernel executes it on the tick thread and returns the event's
// extras ({"writes": [...], "calls": [...]}), or throws Refusal. Absent (the offline hosts, and the
// reference / Frida executors) -> the step refuses 'unsupported', as play_objective does.
using CallFn = std::function<J(const Step& st)>;

class Executor {
 public:
  Executor(const std::vector<Step>& steps, WorldFn world, WriteFn write, bool capsPad, ClockFn clock,
           EventFn onEvent = nullptr, std::shared_ptr<CheatState> cheats = nullptr, CallFn calls = nullptr);
  const CheatState& cheats() const { return *cheats_; }
  void tick(const std::string& variant);                       // "active" | "observe"
  void poll(std::optional<double> now = std::nullopt);         // host check: the deadline only
  // The idle-pad path (skipexec.Executor.idle_pad, frida_skip.js idlePad): the host calls it when
  // no hooked tick has run for 10 s while a pad step is current (a modal popup stops every
  // hooked tick). Pure reads, on the caller's thread. Returns {"pressed": bool, ...}.
  J idlePad();
  const std::string& status() const { return status_; }
  size_t k() const { return k_; }
  int64_t tickAll() const { return tickAll_; }
  int64_t tickActive() const { return tickActive_; }
  const std::vector<J>& events() const { return events_; }
  const std::vector<Step>& steps() const { return steps_; }
  const std::optional<J>& pendingRestore() const { return pendingRestore_; }
  std::optional<J> takePendingRestore() { auto p = pendingRestore_; pendingRestore_.reset(); return p; }
  void stopExternal(const std::string& variant, const std::string& code, const std::string& why) {
    stop(variant, code, why);
  }

 private:
  void newStep();
  J& event(const std::string& ev, const std::string& variant, J extra = J::obj());
  void apply(const J& writes);
  void advance();
  void stop(const std::string& variant, const std::string& code, const std::string& why);
  bool deadlineHit(std::optional<double> now);
  void tickPad(const Step& st, World& w, const std::vector<Cond>& conds, const std::string& variant,
               bool timedOut);
  void noteCheat(World& w, const Step& st, const J& wr);

  std::vector<Step> steps_;
  WorldFn world_;
  WriteFn write_;
  bool capsPad_;
  ClockFn clock_;
  EventFn onEvent_;
  size_t k_ = 0;
  int64_t tickAll_ = 0, tickActive_ = 0;
  std::vector<J> events_;
  std::string status_ = "running";
  std::optional<J> pendingRestore_;
  std::shared_ptr<CheatState> cheats_;
  CallFn calls_;
  // per step
  std::optional<int64_t> started_;
  std::optional<double> startedWall_;
  int64_t stableN_ = 0;
  std::optional<std::string> stableKey_;
  bool guardSeen_ = false, promptSeen_ = false;
  int64_t settleN_ = 0;
  std::optional<std::string> settleKey_;
};

// ============================================================== report ====
// skipexec.report: the differential object over one image.
struct StepKey {
  std::string prim;
  std::optional<int64_t> idx;
  std::optional<std::string> flow;
  std::optional<std::string> arg;        // DT-2 commands only (the report key then ends " <arg>")
};
extern const char* const kReportConds[];      // skipexec.REPORT_CONDS (nullptr-terminated)
J report(World& w, const std::vector<StepKey>& keys, const std::vector<std::string>& condTexts);

}  // namespace skip
}  // namespace devtools
}  // namespace sr
