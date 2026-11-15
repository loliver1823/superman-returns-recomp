// iter-466 map lane: the useful pause map's host model (kernel/map_markers.hpp, docs/MAP.md).
#include "map_markers.hpp"

#include "devtools_skip_core.hpp"
#include "guest_context.hpp"
#include "mod_bridge.hpp"
#include "mod_formats.hpp"
#include "mod_manager.hpp"
#include "../app/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace sr {
namespace kernel {
namespace mapmk {

namespace {

// ================================================================ parsing ====
struct Ctx {
  std::string where;
  std::string* err;
  bool fail(const std::string& why) {
    if (err && err->empty()) *err = where + ": " + why;
    return false;
  }
};

bool validId(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
      return false;
  return true;
}

bool isNum(const JsonValue* v) { return v && v->type == JsonValue::Type::Number; }

bool onlyKeys(const JsonValue& o, std::initializer_list<const char*> keys, Ctx& c) {
  for (const auto& kv : o.obj) {
    bool ok = false;
    for (const char* k : keys) ok = ok || kv.first == k;
    if (!ok) return c.fail("unknown key '" + kv.first + "'");
  }
  return true;
}

bool getStr(const JsonValue& o, const char* k, std::string* out, Ctx& c, bool required, size_t maxLen = 256) {
  const JsonValue* v = o.get(k);
  if (!v) return required ? c.fail(std::string("missing '") + k + "'") : true;
  if (!v->isString() || v->str.size() > maxLen)
    return c.fail(std::string("'") + k + "' must be a string of at most " + std::to_string(maxLen) + " characters");
  *out = v->str;
  return true;
}

const char* kValueKeys[] = {"const", "das", "bit", "stat", "trigger_available", "trigger_engaged", "lua", "all",
                            "any", "not", "ge"};

bool parseValue(const JsonValue& v, Value* out, Ctx& c, std::initializer_list<const char*> extra = {}, int depth = 0) {
  if (depth > 8) return c.fail("value nested too deeply");
  if (v.type == JsonValue::Type::Bool) { out->kind = Value::Kind::Const; out->num = v.b ? 1 : 0; return true; }
  if (v.type == JsonValue::Type::Number) { out->kind = Value::Kind::Const; out->num = v.num; return true; }
  if (!v.isObject()) return c.fail("a value must be an object, a number or true/false");
  const char* src = nullptr;
  for (const auto& kv : v.obj) {
    bool known = false;
    for (const char* k : kValueKeys)
      if (kv.first == k) {
        if (src) return c.fail(std::string("a value names one source, not '") + src + "' and '" + k + "'");
        src = k;
        known = true;
      }
    if (kv.first == "flow" || kv.first == "index") known = true;
    for (const char* k : extra) known = known || kv.first == k;
    if (!known) return c.fail("unknown value key '" + kv.first + "'");
  }
  if (!src) return c.fail("a value needs one of const, das, bit, stat, trigger_available, trigger_engaged, lua, all, any, not, ge");
  const JsonValue& s = *v.get(src);
  const std::string k = src;
  if (k == "const") {
    if (s.type == JsonValue::Type::Bool) out->num = s.b ? 1 : 0;
    else if (isNum(&s)) out->num = s.num;
    else return c.fail("'const' must be a number or true/false");
    out->kind = Value::Kind::Const;
    return true;
  }
  if (k == "das" || k == "bit" || k == "stat" || k == "lua") {
    if (!s.isString() || s.str.empty() || s.str.size() > 128) return c.fail("'" + k + "' must name a value (1..128 characters)");
    out->name = s.str;
    out->kind = k == "das" ? Value::Kind::Das : k == "bit" ? Value::Kind::Bit : k == "stat" ? Value::Kind::Stat : Value::Kind::Lua;
    if (const JsonValue* f = v.get("flow")) {
      if (out->kind != Value::Kind::Das && out->kind != Value::Kind::Bit) return c.fail("'flow' only goes with das / bit");
      if (!f->isString() || f->str.empty() || f->str.size() > 128) return c.fail("'flow' must name a world flow");
      out->flow = f->str;
    }
    if (out->kind == Value::Kind::Bit) {
      const JsonValue* ix = v.get("index");
      if (!ix || !ix->isInteger() || ix->i64 < 0 || ix->i64 >= 32 * 64) return c.fail("'bit' needs an integer 'index' (0..2047)");
      out->index = ix->i64;
    } else if (v.get("index")) {
      return c.fail("'index' only goes with bit");
    }
    return true;
  }
  if (v.get("flow") || v.get("index")) return c.fail("'flow' / 'index' only go with das / bit");
  if (k == "trigger_available" || k == "trigger_engaged") {
    if (!s.isInteger() || s.i64 < 0 || s.i64 > 4095) return c.fail("'" + k + "' must be an objectiveIndex (0..4095)");
    out->kind = k == "trigger_available" ? Value::Kind::TriggerAvailable : Value::Kind::TriggerEngaged;
    out->index = s.i64;
    return true;
  }
  if (k == "not") {
    out->kind = Value::Kind::Not;
    out->kids.resize(1);
    return parseValue(s, &out->kids[0], c, {}, depth + 1);
  }
  if (k == "all" || k == "any") {
    if (!s.isArray() || s.arr.empty() || s.arr.size() > 16) return c.fail("'" + k + "' must be an array of 1..16 values");
    out->kind = k == "all" ? Value::Kind::All : Value::Kind::Any;
    out->kids.resize(s.arr.size());
    for (size_t i = 0; i < s.arr.size(); ++i)
      if (!parseValue(s.arr[i], &out->kids[i], c, {}, depth + 1)) return false;
    return true;
  }
  // ge: [value, number]
  if (!s.isArray() || s.arr.size() != 2 || !isNum(&s.arr[1])) return c.fail("'ge' must be [value, number]");
  out->kind = Value::Kind::Ge;
  out->num = s.arr[1].num;
  out->kids.resize(1);
  return parseValue(s.arr[0], &out->kids[0], c, {}, depth + 1);
}

bool parseText(const JsonValue& v, Text* out, Ctx& c) {
  if (v.isString()) {
    if (v.str.size() > 256) return c.fail("text longer than 256 characters");
    out->kind = Text::Kind::Literal;
    out->s = v.str;
    return true;
  }
  if (!v.isObject() || v.obj.size() != 1) return c.fail("text must be a string or one of {text}, {text_key}, {trigger_text}");
  const auto& kv = v.obj[0];
  if (!kv.second.isString() || kv.second.str.empty() || kv.second.str.size() > 256) return c.fail("'" + kv.first + "' must be a string");
  out->s = kv.second.str;
  if (kv.first == "text") out->kind = Text::Kind::Literal;
  else if (kv.first == "text_key") out->kind = Text::Kind::Key;
  else if (kv.first == "trigger_text") out->kind = Text::Kind::TriggerText;
  else return c.fail("unknown text key '" + kv.first + "'");
  return true;
}

bool parseReadout(const JsonValue& v, Readout* out, Ctx& c) {
  if (!v.isObject()) return c.fail("must be an object (a value plus format / label / when / else_label)");
  if (!parseValue(v, &out->value, c, {"format", "label", "when", "else_label"})) return false;
  if (!getStr(v, "format", &out->format, c, false, 16) || !getStr(v, "label", &out->label, c, false, 48) ||
      !getStr(v, "else_label", &out->elseLabel, c, false, 48))
    return false;
  if (out->format != "count" && out->format != "score" && out->format != "time_ms") return c.fail("format must be count, score or time_ms");
  std::string when = "always";
  if (!getStr(v, "when", &when, c, false, 16)) return false;
  if (when != "always" && when != "completed" && when != "pending") return c.fail("when must be always, completed or pending");
  out->when = when == "completed" ? 1 : when == "pending" ? 2 : 0;
  out->present = true;
  return true;
}

bool parseColor(const std::string& s, uint32_t* out) {
  std::string h = s;
  if (!h.empty() && h[0] == '#') h = h.substr(1);
  if (h.size() != 6) return false;
  uint32_t v = 0;
  for (char ch : h) {
    int d = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
    if (d < 0) return false;
    v = (v << 4) | static_cast<uint32_t>(d);
  }
  *out = v;
  return true;
}

bool parseMarker(const JsonValue& v, const std::string& source, MarkerDef* m, Ctx& c) {
  if (!v.isObject()) return c.fail("a marker must be an object");
  if (!getStr(v, "id", &m->id, c, true, 64)) return false;
  if (!validId(m->id)) return c.fail("id '" + m->id + "' must be 1..64 of a-z A-Z 0-9 _ . -");
  c.where += " (" + m->id + ")";
  if (!onlyKeys(v, {"id", "type", "pos", "trigger", "each_trigger", "name", "detail", "show", "completed", "record",
                    "target", "completed_label", "pending_label", "after_complete", "becomes", "route"}, c) ||
      !getStr(v, "type", &m->type, c, true, 64))
    return false;
  m->source = source;
  int nPos = 0;
  if (const JsonValue* p = v.get("pos")) {
    if (!p->isArray() || p->arr.size() != 3 || !isNum(&p->arr[0]) || !isNum(&p->arr[1]) || !isNum(&p->arr[2]))
      return c.fail("pos must be [x, y, z] (world units, Y up)");
    for (int k = 0; k < 3; ++k) {
      const double d = p->arr[k].num;
      if (!std::isfinite(d) || std::fabs(d) > 1.0e6) return c.fail("pos is not finite / too far");
      m->pos[k] = static_cast<float>(d);
    }
    m->hasPos = true;
    ++nPos;
  }
  if (const JsonValue* t = v.get("trigger")) {
    if (!t->isInteger() || t->i64 < 0 || t->i64 > 4095) return c.fail("trigger must be an objectiveIndex (0..4095)");
    m->trigger = t->i64;
    ++nPos;
  }
  if (const JsonValue* e = v.get("each_trigger")) {
    if (!e->isObject()) return c.fail("each_trigger must be an object");
    Ctx ce{c.where + ".each_trigger", c.err};
    if (!onlyKeys(*e, {"trigger_type", "flow", "filter", "not_filter", "available", "engaged"}, ce)) return false;
    if (const JsonValue* tt = e->get("trigger_type")) {
      if (tt->isString()) m->eachTrigger.types.push_back(tt->str);
      else if (tt->isArray() && tt->arr.size() <= 8) {
        for (const auto& x : tt->arr) {
          if (!x.isString() || x.str.empty()) return ce.fail("trigger_type must be a string or an array of strings");
          m->eachTrigger.types.push_back(x.str);
        }
      } else return ce.fail("trigger_type must be a string or an array of at most 8 strings");
    }
    if (!getStr(*e, "flow", &m->eachTrigger.flow, ce, false, 128) || !getStr(*e, "filter", &m->eachTrigger.filter, ce, false, 64) ||
        !getStr(*e, "not_filter", &m->eachTrigger.notFilter, ce, false, 64))
      return false;
    for (const std::string* f : {&m->eachTrigger.filter, &m->eachTrigger.notFilter})
      if (!f->empty() && *f != "METROEVENT") return ce.fail("filter / not_filter: only METROEVENT is known");
    for (const char* k : {"available", "engaged"}) {
      if (const JsonValue* b = e->get(k)) {
        if (b->type != JsonValue::Type::Bool) return ce.fail(std::string(k) + " must be true/false");
        (std::strcmp(k, "available") == 0 ? m->eachTrigger.available : m->eachTrigger.engaged) = b->b ? 1 : 0;
      }
    }
    m->each = true;
    ++nPos;
  }
  if (nPos != 1) return c.fail("a marker needs exactly one of pos, trigger, each_trigger");
  if (const JsonValue* t = v.get("name")) { if (!parseText(*t, &m->name, c)) return false; }
  if (const JsonValue* t = v.get("detail")) { if (!parseText(*t, &m->detail, c)) return false; }
  if ((m->name.kind == Text::Kind::TriggerText || m->detail.kind == Text::Kind::TriggerText) && m->hasPos)
    return c.fail("trigger_text needs a trigger or each_trigger marker");
  if (const JsonValue* s = v.get("show")) { m->hasShow = true; if (!parseValue(*s, &m->show, c)) return false; }
  if (const JsonValue* s = v.get("completed")) { m->hasCompleted = true; if (!parseValue(*s, &m->completed, c)) return false; }
  if (const JsonValue* r = v.get("record")) {
    Ctx cr{c.where + ".record", c.err};
    if (!parseReadout(*r, &m->record, cr)) return false;
  }
  if (const JsonValue* r = v.get("target")) {
    Ctx cr{c.where + ".target", c.err};
    if (!parseReadout(*r, &m->target, cr)) return false;
  }
  if (!getStr(v, "completed_label", &m->completedLabel, c, false, 48) || !getStr(v, "pending_label", &m->pendingLabel, c, false, 48))
    return false;
  std::string ac = "hide";
  if (!getStr(v, "after_complete", &ac, c, false, 8)) return false;
  if (ac != "hide" && ac != "keep") return c.fail("after_complete must be keep or hide");
  m->keep = ac == "keep";
  if (!getStr(v, "becomes", &m->becomes, c, false, 64)) return false;
  if (!m->becomes.empty() && !validId(m->becomes)) return c.fail("becomes must name a marker id");
  if (!m->becomes.empty() && m->becomes == m->id) return c.fail("a marker cannot become itself");
  if (const JsonValue* r = v.get("route")) {
    if (!r->isArray() || r->arr.size() < 2 || r->arr.size() > 256) return c.fail("route must be 2..256 points [x, y, z] (start .. finish)");
    for (const auto& pt : r->arr) {
      if (!pt.isArray() || pt.arr.size() != 3 || !isNum(&pt.arr[0]) || !isNum(&pt.arr[1]) || !isNum(&pt.arr[2]))
        return c.fail("route points are [x, y, z]");
      std::array<float, 3> q{};
      for (int k = 0; k < 3; ++k) {
        if (!std::isfinite(pt.arr[k].num) || std::fabs(pt.arr[k].num) > 1.0e6) return c.fail("a route point is not finite / too far");
        q[k] = static_cast<float>(pt.arr[k].num);
      }
      m->route.push_back(q);
    }
  }
  return true;
}

bool parseMap(const JsonValue& v, const std::string& source, MapDef* m, Ctx& c) {
  if (!v.isObject()) return c.fail("a map must be an object");
  if (!getStr(v, "id", &m->id, c, true, 64)) return false;
  if (!validId(m->id)) return c.fail("id '" + m->id + "' must be 1..64 of a-z A-Z 0-9 _ . -");
  c.where += " (" + m->id + ")";
  if (!onlyKeys(v, {"id", "world", "name", "view", "types", "markers"}, c) || !getStr(v, "world", &m->world, c, false, 128) ||
      !getStr(v, "name", &m->name, c, false, 64))
    return false;
  m->source = source;
  if (const JsonValue* vw = v.get("view")) {
    Ctx cv{c.where + ".view", c.err};
    if (!vw->isObject()) return cv.fail("must be an object");
    if (!onlyKeys(*vw, {"kind", "movie", "rect", "y", "size"}, cv) || !getStr(*vw, "kind", &m->view.kind, cv, true, 16)) return false;
    if (m->view.kind == "art") {
      if (!getStr(*vw, "movie", &m->view.movie, cv, true, 256)) return false;
      if (m->view.movie.empty() || m->view.movie.find("..") != std::string::npos) return cv.fail("movie must be a path inside the pack");
      const JsonValue* r = vw->get("rect");
      if (!r || !r->isArray() || r->arr.size() != 4) return cv.fail("rect must be [x0, z0, x1, z1]");
      for (int k = 0; k < 4; ++k) {
        if (!isNum(&r->arr[k]) || !std::isfinite(r->arr[k].num)) return cv.fail("rect must be 4 numbers");
        m->view.rect[k] = static_cast<float>(r->arr[k].num);
      }
      if (m->view.rect[0] == m->view.rect[2] || m->view.rect[1] == m->view.rect[3]) return cv.fail("rect is empty");
      if (const JsonValue* y = vw->get("y")) {
        if (!isNum(y)) return cv.fail("y must be a number");
        m->view.y = static_cast<float>(y->num);
      }
      if (const JsonValue* sz = vw->get("size")) {
        if (!sz->isArray() || sz->arr.size() != 2 || !sz->arr[0].isInteger() || !sz->arr[1].isInteger() || sz->arr[0].i64 < 1 ||
            sz->arr[0].i64 > 8192 || sz->arr[1].i64 < 1 || sz->arr[1].i64 > 8192)
          return cv.fail("size must be [width, height] in pixels (1..8192)");
        m->view.w = static_cast<int>(sz->arr[0].i64);
        m->view.h = static_cast<int>(sz->arr[1].i64);
      }
    } else if (m->view.kind != "game") {
      return cv.fail("kind must be game or art");
    } else if (vw->get("movie") || vw->get("rect") || vw->get("y") || vw->get("size")) {
      return cv.fail("movie / rect / y / size only go with kind art");
    }
    m->hasView = true;
  }
  if (const JsonValue* ts = v.get("types")) {
    if (!ts->isArray() || ts->arr.size() > 32) return c.fail("types must be an array of at most 32");
    std::set<std::string> seen;
    for (size_t i = 0; i < ts->arr.size(); ++i) {
      const JsonValue& t = ts->arr[i];
      Ctx ct{c.where + ".types[" + std::to_string(i) + "]", c.err};
      if (!t.isObject()) return ct.fail("a type must be an object");
      if (!onlyKeys(t, {"id", "label", "icon", "color", "glyph"}, ct)) return false;
      TypeDef td;
      td.source = source;
      std::string color = "FFFFFF";
      if (!getStr(t, "id", &td.id, ct, true, 64) || !getStr(t, "label", &td.label, ct, true, 32) ||
          !getStr(t, "icon", &td.icon, ct, false, 16) || !getStr(t, "color", &color, ct, false, 8) ||
          !getStr(t, "glyph", &td.glyph, ct, false, 3))
        return false;
      if (!validId(td.id)) return ct.fail("id '" + td.id + "' must be 1..64 of a-z A-Z 0-9 _ . -");
      if (!seen.insert(td.id).second) return ct.fail("duplicate type id '" + td.id + "'");
      if (td.icon != "ring" && td.icon != "reticle" && td.icon != "superman" && td.icon != "dot" && td.icon != "pulse")
        return ct.fail("icon must be ring, reticle, pulse, superman or dot");
      if (!parseColor(color, &td.color)) return ct.fail("color must be RRGGBB");
      m->types.push_back(td);
    }
  }
  if (const JsonValue* ms = v.get("markers")) {
    if (!ms->isArray() || ms->arr.size() > 1024) return c.fail("markers must be an array of at most 1024");
    std::set<std::string> seen;
    for (size_t i = 0; i < ms->arr.size(); ++i) {
      MarkerDef md;
      Ctx cm{c.where + ".markers[" + std::to_string(i) + "]", c.err};
      if (!parseMarker(ms->arr[i], source, &md, cm)) return false;
      if (!seen.insert(md.id).second) return cm.fail("duplicate marker id '" + md.id + "'");
      m->markers.push_back(std::move(md));
    }
  }
  return true;
}

// ================================================================ evaluation ====
struct Eval {
  GameReader& g;
  std::set<std::string> unreadable;
  std::optional<double> value(const Value& v) {
    switch (v.kind) {
      case Value::Kind::Const: return v.num;
      case Value::Kind::Das: {
        const auto x = g.das(v.name, v.flow);
        if (!x) { unreadable.insert("das " + v.name); return std::nullopt; }
        return static_cast<double>(*x);
      }
      case Value::Kind::Bit: {
        const auto x = g.das(v.name + std::to_string(v.index >> 5), v.flow);
        if (!x) { unreadable.insert("bit " + v.name + std::to_string(v.index >> 5)); return std::nullopt; }
        return static_cast<double>((static_cast<uint64_t>(*x) >> (v.index & 31)) & 1u);
      }
      case Value::Kind::Stat: {
        const auto x = g.stat(v.name);
        if (!x) unreadable.insert("stat " + v.name);
        return x;
      }
      case Value::Kind::Lua: {
        const auto x = g.lua(v.name);
        if (!x) unreadable.insert("lua " + v.name);
        return x;
      }
      case Value::Kind::TriggerAvailable:
      case Value::Kind::TriggerEngaged: {
        for (const auto& t : g.triggers())
          if (t.idx == v.index) return (v.kind == Value::Kind::TriggerAvailable ? t.available : t.engaged) ? 1.0 : 0.0;
        return 0.0;   // no such live trigger: not available / not engaged
      }
      case Value::Kind::Not: {
        const auto x = value(v.kids[0]);
        if (!x) return std::nullopt;
        return *x != 0 ? 0.0 : 1.0;
      }
      case Value::Kind::All:
      case Value::Kind::Any: {
        // a decided answer wins over an unreadable kid: any(true, ?) = 1, all(false, ?) = 0
        bool any = false, all = true, unknown = false;
        for (const auto& k : v.kids) {
          const auto x = value(k);
          if (!x) { unknown = true; continue; }
          any = any || *x != 0;
          all = all && *x != 0;
        }
        if (v.kind == Value::Kind::Any) return any ? std::optional<double>(1.0) : unknown ? std::nullopt : std::optional<double>(0.0);
        return !all ? std::optional<double>(0.0) : unknown ? std::nullopt : std::optional<double>(1.0);
      }
      case Value::Kind::Ge: {
        const auto x = value(v.kids[0]);
        if (!x) return std::nullopt;
        return *x >= v.num ? 1.0 : 0.0;
      }
    }
    return std::nullopt;
  }
  bool truthy(const Value& v, bool dflt) {
    const auto x = value(v);
    return x ? *x != 0 : dflt;
  }
};

std::string formatReadout(const Readout& r, double v) {
  if (r.format == "time_ms") return formatTimeMs(v);
  if (r.format == "score") return formatScore(v);
  char b[32];
  std::snprintf(b, sizeof b, "%lld", static_cast<long long>(std::llround(v)));
  return b;
}

std::string readoutText(Eval& e, const Readout& r, bool completed) {
  if (!r.present) return "";
  const auto v = e.value(r.value);
  if (!v) return "";
  if (r.when == 2 && completed) return "";
  if (r.when == 1 && !completed) return r.elseLabel.empty() ? "" : r.elseLabel + " " + formatReadout(r, *v);
  return r.label.empty() ? formatReadout(r, *v) : r.label + " " + formatReadout(r, *v);
}

std::string textOf(const Text& t, const TriggerInfo* trig) {
  switch (t.kind) {
    case Text::Kind::None: return "";
    case Text::Kind::Literal: return t.s;
    case Text::Kind::Key: return "@" + t.s;
    case Text::Kind::TriggerText:
      if (!trig) return "";
      if (t.s == "objectiveNameLocalizationKey") return trig->nameKey.empty() ? "" : "@" + trig->nameKey;
      if (t.s == "objectiveTextLocalizationKey") return trig->textKey.empty() ? "" : "@" + trig->textKey;
      return "";
  }
  return "";
}

bool triggerMatches(const EachTrigger& f, const TriggerInfo& t) {
  if (!f.types.empty() && std::find(f.types.begin(), f.types.end(), t.type) == f.types.end()) return false;
  if (!f.flow.empty() && f.flow != t.flow) return false;
  if (f.notFilter == "METROEVENT" && t.metro) return false;
  if (f.filter == "METROEVENT" && !t.metro) return false;
  if (f.available == 1 && !t.available && !t.engaged) return false;   // the current objective stays
  if (f.available == 0 && t.available) return false;
  if (f.engaged >= 0 && (f.engaged == 1) != t.engaged) return false;
  return true;
}

bool atOrigin(const std::array<float, 3>& p) {
  return std::fabs(p[0]) < 0.5f && std::fabs(p[1]) < 0.5f && std::fabs(p[2]) < 0.5f;
}

}  // namespace

bool parseDefinition(const JsonValue& root, const std::string& source, Definition* out, std::string* err) {
  if (err) err->clear();
  Ctx c{"map", err};
  if (!root.isObject()) return c.fail("must be an object");
  if (!onlyKeys(root, {"format", "maps"}, c)) return false;
  const JsonValue* f = root.get("format");
  if (!f || !f->isInteger() || f->i64 != 1) return c.fail("format must be 1");
  const JsonValue* maps = root.get("maps");
  if (!maps || !maps->isArray() || maps->arr.empty() || maps->arr.size() > 16) return c.fail("maps must be an array of 1..16 maps");
  Definition d;
  std::set<std::string> seen;
  for (size_t i = 0; i < maps->arr.size(); ++i) {
    MapDef m;
    Ctx cm{"map: maps[" + std::to_string(i) + "]", err};
    if (!parseMap(maps->arr[i], source, &m, cm)) return false;
    if (!seen.insert(m.id).second) return cm.fail("duplicate map id '" + m.id + "'");
    d.maps.push_back(std::move(m));
  }
  *out = std::move(d);
  return true;
}

bool parseDefinitionText(const std::string& text, const std::string& source, Definition* out, std::string* err) {
  JsonValue j;
  std::string perr;
  if (!jsonParse(text, &j, &perr)) {
    if (err) *err = "map: " + perr;
    return false;
  }
  return parseDefinition(j, source, out, err);
}

std::vector<MapDef> mergeLayers(const std::vector<Layer>& layers, std::vector<std::string>* notes) {
  std::vector<MapDef> out;
  for (const auto& L : layers) {
    if (!L.def) continue;
    for (const auto& m : L.def->maps) {
      auto it = std::find_if(out.begin(), out.end(), [&](const MapDef& x) { return x.id == m.id; });
      if (it == out.end()) {
        out.push_back(m);
        continue;
      }
      if (m.hasView) {   // a replacement map
        if (notes) notes->push_back("map " + m.id + ": " + L.id + " replaces " + it->source);
        *it = m;
        continue;
      }
      // additions: types and markers by id (a repeated id replaces the earlier one)
      for (const auto& t : m.types) {
        auto ti = std::find_if(it->types.begin(), it->types.end(), [&](const TypeDef& x) { return x.id == t.id; });
        if (ti == it->types.end()) it->types.push_back(t);
        else {
          if (notes) notes->push_back("map " + m.id + " type " + t.id + ": " + L.id + " replaces " + ti->source);
          *ti = t;
        }
      }
      for (const auto& mk : m.markers) {
        auto mi = std::find_if(it->markers.begin(), it->markers.end(), [&](const MarkerDef& x) { return x.id == mk.id; });
        if (mi == it->markers.end()) it->markers.push_back(mk);
        else {
          if (notes) notes->push_back("map " + m.id + " marker " + mk.id + ": " + L.id + " replaces " + mi->source);
          *mi = mk;
        }
      }
      if (it->world.empty() && !m.world.empty()) it->world = m.world;
    }
  }
  // markers naming an unknown type are dropped with a note (their layer may rely on another)
  for (auto& m : out) {
    std::vector<MarkerDef> kept;
    for (auto& mk : m.markers) {
      const bool known = std::any_of(m.types.begin(), m.types.end(), [&](const TypeDef& t) { return t.id == mk.type; });
      if (known) kept.push_back(std::move(mk));
      else if (notes) notes->push_back("map " + m.id + " marker " + mk.id + " (" + mk.source + "): unknown type '" + mk.type + "', dropped");
    }
    m.markers = std::move(kept);
  }
  return out;
}

std::string formatTimeMs(double ms) {
  if (!std::isfinite(ms) || ms < 0) ms = 0;
  const long long cs = std::llround(ms / 10.0);
  char b[32];
  std::snprintf(b, sizeof b, "%lld:%02lld.%02lld", cs / 6000, (cs / 100) % 60, cs % 100);
  return b;
}

std::string formatScore(double v) {
  long long n = std::llround(std::isfinite(v) ? v : 0);
  const bool neg = n < 0;
  if (neg) n = -n;
  std::string s = std::to_string(n), o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i && (s.size() - i) % 3 == 0) o += ',';
    o += s[i];
  }
  return neg ? "-" + o : o;
}

Evaluation evaluate(const std::vector<MapDef>& maps, GameReader& g) {
  Evaluation ev;
  const std::vector<std::string> worlds = g.worlds();
  const MapDef* pick = nullptr;
  int best = -1;
  for (const auto& m : maps) {   // later = higher priority; a named world beats "any world"
    int score = -1;
    if (m.world.empty()) score = 0;
    else if (std::find(worlds.begin(), worlds.end(), m.world) != worlds.end()) score = 1;
    if (score >= 0 && score >= best) { best = score; pick = &m; }
  }
  if (!pick) return ev;
  ev.available = true;
  ev.mapId = pick->id;
  ev.view = pick->view;
  ev.types = pick->types;
  Eval e{g};
  std::vector<int> total(pick->types.size(), 0), done(pick->types.size(), 0);
  std::vector<bool> counted(pick->types.size(), false);
  std::set<std::string> replaced;   // markers whose "becomes" target took over
  for (const auto& md : pick->markers) {
    const int ti = static_cast<int>(std::find_if(pick->types.begin(), pick->types.end(),
                                                 [&](const TypeDef& t) { return t.id == md.type; }) - pick->types.begin());
    auto emit = [&](const std::string& id, const TriggerInfo* trig, bool hasPos, const std::array<float, 3>& pos) {
      if (md.hasShow && !e.truthy(md.show, false)) return;
      const bool completed = md.hasCompleted && e.truthy(md.completed, false);
      // the legend's done / total counts only the type's own layer: a mod marker of a built-in type is
      // drawn but never turns KITTENS 0/100 into 0/101 (a mod that wants a count defines its own type)
      if (md.hasCompleted && md.source == pick->types[ti].source) {
        counted[ti] = true;
        ++total[ti];
        if (completed) ++done[ti];
      }
      if (completed && !md.becomes.empty()) { replaced.insert(id); return; }
      if (completed && !md.keep) return;   // a finished story beat leaves the map
      Marker mk;
      mk.id = id;
      mk.type = md.type;
      mk.typeIndex = ti;
      if (completed) mk.state |= kCompleted;
      if (trig && trig->engaged) mk.state |= kCurrent;
      mk.title = textOf(md.name, trig);
      if (mk.title.empty() && trig && !trig->nameKey.empty()) mk.title = "@" + trig->nameKey;
      // desc: the detail text (or the status), then the record / target readouts
      std::vector<std::string> parts;
      const std::string detail = textOf(md.detail, trig);
      std::string status;
      if (mk.state & kCurrent) status = "CURRENT OBJECTIVE";
      else if (completed) status = md.completedLabel.empty() ? "COMPLETED" : md.completedLabel;
      else if (md.hasCompleted && !md.pendingLabel.empty()) status = md.pendingLabel;
      if (!detail.empty()) parts.push_back(detail);
      if (!status.empty()) parts.push_back(status);
      const std::string rec = readoutText(e, md.record, completed), tgt = readoutText(e, md.target, completed);
      std::string extra = tgt;
      if (!rec.empty()) extra = extra.empty() ? rec : extra + "  -  " + rec;
      if (!extra.empty()) parts.push_back(extra);
      // the wire carries two lines: a '@key' line stays alone, consecutive literal lines are joined
      {
        std::vector<std::string> merged;
        for (const auto& p : parts) {
          if (!merged.empty() && p[0] != '@' && merged.back()[0] != '@') merged.back() += "  -  " + p;
          else merged.push_back(p);
        }
        parts = merged;
        while (parts.size() > 2) parts.erase(parts.begin() + 1);   // two keys + a literal: first key + literal
      }
      mk.desc = parts.size() > 0 ? parts[0] : "";
      if (parts.size() > 1) mk.desc2 = parts[1];
      mk.detail = detail;
      mk.status = status;
      mk.readout = extra;
      // a trail is the built-in races' alone (owner 2026-10-06: "rampages and mods don't need a trail")
      if (md.source == kBuiltinLayer) mk.route = md.route;
      mk.hasPos = hasPos;
      mk.pos = pos;
      ev.markers.push_back(std::move(mk));
    };
    if (md.each) {
      for (const auto& t : g.triggers()) {
        if (!triggerMatches(md.eachTrigger, t)) continue;
        emit(md.id + "." + std::to_string(t.idx), &t, t.hasPos && !atOrigin(t.pos), t.pos);
      }
    } else if (md.trigger >= 0) {
      const TriggerInfo* found = nullptr;
      for (const auto& t : g.triggers())
        if (t.idx == md.trigger) found = &t;
      if (found) emit(md.id, found, found->hasPos && !atOrigin(found->pos), found->pos);
      else e.unreadable.insert("trigger " + std::to_string(md.trigger) + " (not in the world)");
    } else {
      emit(md.id, nullptr, true, md.pos);
    }
  }
  (void)replaced;
  for (size_t i = 0; i < ev.types.size(); ++i) {
    std::string l = ev.types[i].label;
    if (counted[i]) l += " " + std::to_string(done[i]) + "/" + std::to_string(total[i]);
    ev.typeLabels.push_back(l);
    ev.typeDone.push_back(counted[i] ? done[i] : 0);
    ev.typeTotal.push_back(counted[i] ? total[i] : -1);
  }
  ev.warnings.assign(e.unreadable.begin(), e.unreadable.end());
  return ev;
}

// ================================================================ projection ====
Camera cameraFromEuler(float fovOrTan, bool tanDirect, float aspect, const std::array<float, 3>& pos,
                       const std::array<float, 3>& euler, float W, float H) {
  // AptBuildTransformMatrix_sub_826CBF70: R = Rz(e2) * Ry(e1) * Rx(e0) (column vectors); the
  // camera frame's world matrix rows are R's columns (row-vector form), so view_i = dot(p - pos,
  // column i of R). For the map camera (e1 = e2 = 0) the rows are (1,0,0), (0,c,s), (0,-s,c).
  const double a = euler[0], b = euler[1], g = euler[2];
  const double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cg = std::cos(g), sg = std::sin(g);
  const double Rx[3][3] = {{1, 0, 0}, {0, ca, -sa}, {0, sa, ca}};
  const double Ry[3][3] = {{cb, 0, sb}, {0, 1, 0}, {-sb, 0, cb}};
  const double Rz[3][3] = {{cg, -sg, 0}, {sg, cg, 0}, {0, 0, 1}};
  double t[3][3], R[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      t[i][j] = 0;
      for (int k = 0; k < 3; ++k) t[i][j] += Ry[i][k] * Rx[k][j];
    }
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      R[i][j] = 0;
      for (int k = 0; k < 3; ++k) R[i][j] += Rz[i][k] * t[k][j];
    }
  Camera c;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) c.rows[i][j] = static_cast<float>(R[j][i]);
  c.tanHalf = tanDirect ? fovOrTan : static_cast<float>(std::tan(fovOrTan * 0.5));
  c.aspect = aspect;
  c.pos = pos;
  c.W = W;
  c.H = H;
  return c;
}

bool project(const Camera& c, const std::array<float, 3>& p, float* sx, float* sy) {
  const double d[3] = {double(p[0]) - c.pos[0], double(p[1]) - c.pos[1], double(p[2]) - c.pos[2]};
  double v[3];
  for (int i = 0; i < 3; ++i) v[i] = d[0] * c.rows[i][0] + d[1] * c.rows[i][1] + d[2] * c.rows[i][2];
  if (!(v[2] > 1e-3) || !(c.tanHalf > 0) || !(c.aspect > 0)) return false;   // behind (the game does not clip)
  const double xp = v[0] / (v[2] * c.tanHalf);
  const double yp = v[1] / (v[2] * c.tanHalf / c.aspect);
  *sx = static_cast<float>(c.W * 0.5 * (1.0 - xp));
  *sy = static_cast<float>(c.H * 0.5 * (1.0 - yp));
  return std::isfinite(*sx) && std::isfinite(*sy);
}

int32_t packXY(float sx, float sy) {
  if (!std::isfinite(sx) || !std::isfinite(sy)) return -1;
  const long x = std::lround(std::clamp(sx, -16384.0f, 16383.0f)) + 16384;
  const long y = std::lround(std::clamp(sy, -16384.0f, 16383.0f)) + 16384;
  return static_cast<int32_t>((x << 15) | y);
}

uint32_t luaStringHash(const std::string& s) {
  // Lua 5.0.2 luaS_hash: h = len; step = (len >> 5) + 1; for (l1 = len; l1 >= step; l1 -= step)
  // h ^= (h << 5) + (h >> 2) + byte[l1 - 1]
  const size_t l = s.size();
  uint32_t h = static_cast<uint32_t>(l);
  const size_t step = (l >> 5) + 1;
  for (size_t l1 = l; l1 >= step; l1 -= step)
    h = h ^ ((h << 5) + (h >> 2) + static_cast<uint8_t>(s[l1 - 1]));
  return h;
}

// ================================================================ the live game ====
namespace {

namespace S = sr::devtools::skip;

constexpr uint32_t G_CAMERA_MGR = 0x8296D86Cu, CAM_FRAME = 0x28, MAPCAM = 0x1060, MAPCAM_MAXH = 0x74;
constexpr uint32_t V_FOV = 0x04, V_ASPECT = 0x08, V_TANFLAG = 0x14, V_POS = 0x20, V_EULER = 0x30;
constexpr uint32_t G_RENDERER = 0x829A9384u, R_WIDTH = 0x3C, R_HEIGHT = 0x40;
constexpr uint32_t G_LUA_MGR = 0x8296D8CCu, LUA_MGR_STATE = 0x0C, L_GT_TT = 0x40, L_GT = 0x44;
constexpr uint32_t T_LSIZENODE = 0x07, T_NODE = 0x10, NODE_SIZE = 20, TS_HASH = 0x08, TS_LEN = 0x0C, TS_CHARS = 0x10;
constexpr uint32_t LUA_TNUMBER = 3, LUA_TSTRING = 4, LUA_TTABLE = 5, LUA_TBOOLEAN = 1;

S::PageView livePage(uint32_t pg) {
  const uint32_t va = pg << 12;
  if (!isGuestVAInRange(va)) return {};
  uint8_t* p = guestVAToHost(va);
  if (!p || !hostRangeReadableCached(p, 4096)) return {};
  return {p, 4096};
}

float f32At(S::Mem& m, uint32_t va) {
  const uint32_t u = m.u32(va);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

std::array<float, 3> vec3At(S::Mem& m, uint32_t va) {
  return {f32At(m, va), f32At(m, va + 4), f32At(m, va + 8)};
}

class LiveReader : public GameReader {
 public:
  explicit LiveReader(S::PageSource src) : w_(std::move(src), &graph_) {}

  std::optional<int64_t> das(const std::string& name, const std::string& flow) override {
    try {
      const auto h = w_.flowDasByName(flow.empty() ? "BaseWorldFlow" : flow);
      if (!h) return std::nullopt;
      const uint32_t n = w_.dasNode(*h, name);
      if (!n) return std::nullopt;
      const uint32_t v = w_.mem().u32(n + S::C::NODE_VALUE);
      const uint32_t t = (w_.mem().u32(n + S::C::NODE_FLAGS) >> 17) & 0x7F;
      if (t == S::C::T_FLOAT) {
        float f;
        std::memcpy(&f, &v, 4);
        return static_cast<int64_t>(f);
      }
      return static_cast<int64_t>(v);
    } catch (...) {
      return std::nullopt;
    }
  }
  std::optional<double> stat(const std::string& name) override {
    try { return w_.statValue(name); } catch (...) { return std::nullopt; }
  }
  std::optional<double> lua(const std::string& name) override {
    // twice, and only an agreeing answer counts (the guest may rehash _G between reads)
    const auto a = luaOnce(name), b = luaOnce(name);
    if (a && b && *a == *b) return a;
    return std::nullopt;
  }
  const std::vector<TriggerInfo>& triggers() override {
    if (trig_) return *trig_;
    trig_.emplace();
    try {
      std::set<uint32_t> metro;
      try {
        for (uint32_t e : w_.filterMemberList("OBJ|TRIGGERS|METROEVENT")) metro.insert(e);
      } catch (...) {
      }
      for (const S::J& r : w_.triggers().a()) {
        TriggerInfo t;
        const S::J* idx = r.get("idx");
        if (!idx || !idx->isNum()) continue;
        t.idx = idx->i();
        if (const S::J* f = r.get("flow"); f && f->isStr()) t.flow = f->s();
        const S::J* vt = r.get("vt_ok");
        const S::J* fl = r.get("flags");
        t.available = vt && vt->isBool() && vt->b() && fl && fl->isNum() && !(fl->i() & 1);
        if (const S::J* en = r.get("engaged"); en && en->isBool()) t.engaged = en->b();
        const uint32_t member = hex(r.get("member")), actor = hex(r.get("actor")), das = hex(r.get("das"));
        t.metro = metro.count(member) != 0;
        try {
          if (actor) {
            t.pos = vec3At(w_.mem(), actor + 0x20);
            t.hasPos = std::isfinite(t.pos[0]) && std::isfinite(t.pos[1]) && std::isfinite(t.pos[2]);
          }
          if (das) {
            t.type = w_.dasStr(das, "triggerType").value_or("");
            t.nameKey = w_.dasStr(das, "objectiveNameLocalizationKey").value_or("");
            t.textKey = w_.dasStr(das, "objectiveTextLocalizationKey").value_or("");
          }
        } catch (...) {
        }
        trig_->push_back(std::move(t));
      }
    } catch (...) {
    }
    return *trig_;
  }
  std::vector<std::string> worlds() override {
    try { return w_.loaded(); } catch (...) { return {}; }
  }
  bool objectiveRunning() override {
    const auto v = das("objectiveActivated", "");
    return v && *v != 0;
  }

  // Superman's map position as the game computes it (SUPERMAN entity ACTR +0x20, y clamped to
  // the map camera's maxH - 1000, sub_822AAA00); for the self-check only.
  std::optional<std::array<float, 3>> superman() {
    try {
      const auto ms = w_.filterMemberList("SUPERMAN");
      if (ms.empty() || !ms[0]) return std::nullopt;
      const uint32_t actor = w_.actorOf(ms[0]);
      if (!actor) return std::nullopt;
      auto p = vec3At(w_.mem(), actor + 0x20);
      const uint32_t mgr = w_.mem().u32(G_CAMERA_MGR);
      if (mgr) {
        const float maxH = f32At(w_.mem(), mgr + MAPCAM + MAPCAM_MAXH);
        if (std::isfinite(maxH) && maxH > 0) p[1] = std::min(p[1], maxH - 1000.0f);
      }
      return p;
    } catch (...) {
      return std::nullopt;
    }
  }

  std::optional<Camera> camera() {
    try {
      S::Mem& m = w_.mem();
      const uint32_t mgr = m.u32(G_CAMERA_MGR);
      if (!mgr) return std::nullopt;
      const uint32_t v = m.u32(mgr + CAM_FRAME);
      if (!v) return std::nullopt;
      const uint32_t r = m.u32(G_RENDERER);
      if (!r) return std::nullopt;
      const float W = static_cast<float>(m.u32(r + R_WIDTH)), H = static_cast<float>(m.u32(r + R_HEIGHT));
      if (!(W > 0 && H > 0 && W < 16384 && H < 16384)) return std::nullopt;
      return cameraFromEuler(f32At(m, v + V_FOV), m.u32(v + V_TANFLAG) != 0, f32At(m, v + V_ASPECT), vec3At(m, v + V_POS),
                             vec3At(m, v + V_EULER), W, H);
    } catch (...) {
      return std::nullopt;
    }
  }

 private:
  static uint32_t hex(const S::J* j) {
    if (!j || !j->isStr()) return 0;
    return static_cast<uint32_t>(std::strtoul(j->s().c_str(), nullptr, 16));
  }

  std::optional<double> luaOnce(const std::string& name) {
    try {
      S::Mem& m = w_.mem();
      const uint32_t mgr = m.u32(G_LUA_MGR);
      if (!mgr) return std::nullopt;
      const uint32_t L = m.u32(mgr + LUA_MGR_STATE);
      if (!L || m.u32(L + L_GT_TT) != LUA_TTABLE) return std::nullopt;
      const uint32_t gt = m.u32(L + L_GT);
      if (!gt || m.u8(gt + 4) != LUA_TTABLE) return std::nullopt;
      const uint32_t lsize = m.u8(gt + T_LSIZENODE);
      if (lsize > 20) return std::nullopt;
      const uint32_t node = m.u32(gt + T_NODE);
      if (!node) return std::nullopt;
      const uint32_t h = luaStringHash(name);
      uint32_t n = node + NODE_SIZE * (h & ((1u << lsize) - 1));
      for (int hops = 0; n && hops < 4096; ++hops) {
        if (n < node || n >= node + NODE_SIZE * (1u << lsize)) return std::nullopt;   // a torn chain
        const uint32_t ktt = m.u32(n), vtt = m.u32(n + 8);
        if (ktt == LUA_TSTRING && vtt != 0) {
          const uint32_t ts = m.u32(n + 4);
          if (ts && m.u32(ts + TS_HASH) == h && m.u32(ts + TS_LEN) == name.size()) {
            const auto b = m.raw(ts + TS_CHARS, static_cast<uint32_t>(name.size()));
            if (std::equal(b.begin(), b.end(), name.begin(), [](uint8_t x, char y) { return x == static_cast<uint8_t>(y); })) {
              const uint32_t val = m.u32(n + 12);
              if (vtt == LUA_TNUMBER) {
                float f;
                std::memcpy(&f, &val, 4);
                return static_cast<double>(f);
              }
              if (vtt == LUA_TBOOLEAN) return val ? 1.0 : 0.0;
              return std::nullopt;   // a string / table / function global has no status value
            }
          }
        }
        n = m.u32(n + 16);
      }
    } catch (...) {
    }
    return std::nullopt;
  }

  S::Graph graph_;
  S::World w_;
  std::optional<std::vector<TriggerInfo>> trig_;
};

// ================================================================ the session ====
struct Session {
  std::mutex mu;
  bool open = false;
  Evaluation ev;
  std::vector<int32_t> sent;     // last packed position per marker
  int32_t artA = -2, artB = -2;   // last packed art corners
  int selected = -1;              // the screen's selected marker (its route is drawn)
  int dbgLines = 0;
  std::vector<int32_t> routeSent;
  std::map<int64_t, TriggerInfo> seenTriggers;   // every trigger seen this process (its own position)
  uint64_t ticks = 0, opens = 0;
  double worstCheck = 0;
  std::set<std::string> hiddenTypes;   // the map screen's filters (kept for the process)
  GameReader* testReader = nullptr;
  const Camera* testCam = nullptr;
  const std::vector<MapDef>* testMaps = nullptr;
};
Session& session() {
  static Session s;
  return s;
}

// The live triggers plus every non-metro trigger seen earlier in this process (its own position from
// the game): during an objective the world despawns the other triggers, but a race the player has
// unlocked still has a start on the map. A remembered trigger is never available or engaged.
class CachedReader : public GameReader {
 public:
  CachedReader(GameReader& g, std::map<int64_t, TriggerInfo>& seen) : g_(g), seen_(seen) {}
  std::optional<int64_t> das(const std::string& n, const std::string& f) override { return g_.das(n, f); }
  std::optional<double> stat(const std::string& n) override { return g_.stat(n); }
  std::optional<double> lua(const std::string& n) override { return g_.lua(n); }
  std::vector<std::string> worlds() override { return g_.worlds(); }
  bool objectiveRunning() override { return g_.objectiveRunning(); }
  const std::vector<TriggerInfo>& triggers() override {
    if (built_) return all_;
    built_ = true;
    all_ = g_.triggers();
    std::set<int64_t> live;
    for (const auto& t : all_) {
      live.insert(t.idx);
      if (!t.metro && t.hasPos) seen_[t.idx] = t;
    }
    for (const auto& kv : seen_) {
      if (live.count(kv.first)) continue;
      TriggerInfo t = kv.second;
      t.available = false;
      t.engaged = false;
      all_.push_back(t);
    }
    return all_;
  }

 private:
  GameReader& g_;
  std::map<int64_t, TriggerInfo>& seen_;
  std::vector<TriggerInfo> all_;
  bool built_ = false;
};

std::vector<MapDef> activeMaps(std::vector<std::string>* notes) {
  std::vector<Layer> layers;
  const auto& active = modActiveLayers();   // highest priority first
  for (auto it = active.rbegin(); it != active.rend(); ++it)
    if (it->map) layers.push_back({it->id, it->map});
  return mergeLayers(layers, notes);
}

// The map screen's marker colours (tools/apt/mods/pause_map/map.patch: one ring / pulse template per
// colour, painted by its own placement colour transform); a type's colour is matched to the nearest.
const uint32_t kPalette[] = {0xFFFFFF, 0xFFC830, 0x38B8FF, 0xC070FF, 0xFF5040, 0x40FF80, 0xFF9030, 0x30E0E0, 0xFF60C0, 0xA0A0A0};
uint32_t paletteIndex(uint32_t rgb) {
  uint32_t best = 0;
  long bestD = -1;
  for (uint32_t k = 0; k < sizeof(kPalette) / sizeof(kPalette[0]); ++k) {
    const long dr = long((rgb >> 16) & 255) - long((kPalette[k] >> 16) & 255), dg = long((rgb >> 8) & 255) - long((kPalette[k] >> 8) & 255),
               db = long(rgb & 255) - long(kPalette[k] & 255);
    const long d = dr * dr + dg * dg + db * db;
    if (bestD < 0 || d < bestD) { bestD = d; best = k; }
  }
  return best;
}

uint32_t iconId(const std::string& icon) {
  return icon == "reticle" ? 1u : icon == "superman" ? 2u : icon == "dot" ? 3u : icon == "pulse" ? 4u : 0u;
}


// where the selected marker's route landed on the stage (route evidence: start, a middle point,
// finish, and how many points fall inside the map window 24..1256 x 92..628)
void logRoute(const Session& s) {
  if (s.selected < 0 || static_cast<size_t>(s.selected) >= s.ev.markers.size()) return;
  const auto& rt = s.ev.markers[s.selected].route;
  if (rt.empty() || s.routeSent.size() != rt.size()) return;
  auto xy = [&](size_t k, int* x, int* y) {
    const int32_t p = s.routeSent[k];
    if (p < 0) return false;
    *x = (p >> 15) - 16384;
    *y = (p & 0x7FFF) - 16384;
    return true;
  };
  int in = 0;
  for (size_t k = 0; k < rt.size(); ++k) {
    int x, y;
    if (xy(k, &x, &y) && x > 24 && x < 1256 && y > 92 && y < 628) ++in;
  }
  int x0 = 0, y0 = 0, xm = 0, ym = 0, x1 = 0, y1 = 0;
  const bool h0 = xy(0, &x0, &y0), hm = xy(rt.size() / 2, &xm, &ym), h1 = xy(rt.size() - 1, &x1, &y1);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP] route %s: start %s(%d,%d) mid %s(%d,%d) finish %s(%d,%d) in-window %d/%zu",
              s.ev.markers[s.selected].id.c_str(), h0 ? "" : "none", x0, y0, hm ? "" : "none", xm, ym, h1 ? "" : "none", x1, y1, in,
              rt.size());
}

void describe(Session& s, std::vector<ModReply>* out) {
  const Evaluation& ev = s.ev;
  int32_t flags = ev.available ? 1 : 0;
  out->push_back({"SR_MapBegin", {ModArg::Int(static_cast<int32_t>(ev.types.size())),
                                  ModArg::Int(static_cast<int32_t>(ev.markers.size())), ModArg::Int(flags)}});
  if (ev.available && ev.view.kind == "art")   // a mod's own map art, loaded under the markers
    out->push_back({"SR_MapView", {ModArg::Int(1), ModArg::Str(ev.view.movie), ModArg::Int(ev.view.w * 65536 + ev.view.h)}});
  for (size_t i = 0; i < ev.types.size(); ++i) {
    const auto& t = ev.types[i];
    const int32_t packed = static_cast<int32_t>((iconId(t.icon) << 8) | paletteIndex(t.color));   // < 2^24
    const int32_t hidden = s.hiddenTypes.count(t.id) ? 1 : 0;
    out->push_back({"SR_MapType", {ModArg::Int(static_cast<int32_t>(i) | (hidden << 16)), ModArg::Int(packed),
                                   ModArg::Str(t.label)}});
    if (ev.typeTotal[i] >= 0)
      out->push_back({"SR_MapCount", {ModArg::Int(static_cast<int32_t>(i)), ModArg::Int(ev.typeDone[i]), ModArg::Int(ev.typeTotal[i])}});
    if (!t.glyph.empty()) out->push_back({"SR_MapGlyph", {ModArg::Int(static_cast<int32_t>(i)), ModArg::Str(t.glyph)}});
  }
  for (size_t i = 0; i < ev.markers.size(); ++i) {
    const auto& m = ev.markers[i];
    out->push_back({"SR_MapMk", {ModArg::Int(static_cast<int32_t>(i)),
                                 ModArg::Int(static_cast<int32_t>(m.typeIndex) | static_cast<int32_t>(m.state << 8)),
                                 ModArg::Str(m.title)}});
    out->push_back({"SR_MapInfo", {ModArg::Int(static_cast<int32_t>(i)), ModArg::Str(m.detail), ModArg::Str(m.status)}});
    if (!m.readout.empty()) out->push_back({"SR_MapRec", {ModArg::Int(static_cast<int32_t>(i)), ModArg::Str(m.readout)}});
  }
  out->push_back({"SR_MapEnd", {ModArg::Int(static_cast<int32_t>(ev.markers.size()))}});
}

// One position on the wire: (index, x + kWireOff, y + kWireOff) in whole stage pixels, kNoPos = none.
// Apt numbers are float32 in this runtime (the screen's arithmetic on a value above 2^24 drops its
// low bits), so the host never sends a packed value the screen has to take apart, and every value
// stays positive (no reliance on how the runtime's int objects carry a sign).
ModReply xyReply(const char* method, int32_t index, int32_t packed) {
  int32_t x = kNoPos, y = kNoPos;
  if (packed >= 0) {
    x = (packed >> 15) - 16384 + kWireOff;
    y = (packed & 0x7FFF) - 16384 + kWireOff;
  }
  return {method, {ModArg::Int(index), ModArg::Int(x), ModArg::Int(y)}};
}

void positions(Session& s, const Camera& cam, bool all, std::vector<ModReply>* out) {
  const auto& mk = s.ev.markers;
  if (s.ev.available && s.ev.view.kind == "art") {   // the art's corners through the same camera
    const ViewDef& v = s.ev.view;
    float x0, y0, x1, y1;
    int32_t a = -1, b = -1;
    if (project(cam, {v.rect[0], v.y, v.rect[1]}, &x0, &y0) && project(cam, {v.rect[2], v.y, v.rect[3]}, &x1, &y1)) {
      a = packXY(x0, y0);
      b = packXY(x1, y1);
    }
    if (all || a != s.artA || b != s.artB) {   // corner 0 then corner 1 (the screen places the art on 1)
      out->push_back(xyReply("SR_MapArt", 0, a));
      out->push_back(xyReply("SR_MapArt", 1, b));
    }
    s.artA = a;
    s.artB = b;
  }
  if (s.sent.size() != mk.size()) s.sent.assign(mk.size(), -2);
  std::vector<int32_t> now(mk.size(), -1);
  for (size_t i = 0; i < mk.size(); ++i) {
    float x, y;
    if (mk[i].hasPos && project(cam, mk[i].pos, &x, &y)) now[i] = packXY(x, y);
  }
  if (s.selected >= 0 && static_cast<size_t>(s.selected) < mk.size() && !mk[s.selected].route.empty()) {
    const auto& rt = mk[s.selected].route;
    if (s.routeSent.size() != rt.size()) s.routeSent.assign(rt.size(), -2);
    std::vector<int32_t> rn(rt.size(), -1);
    for (size_t k = 0; k < rt.size(); ++k) {
      float x, y;
      if (project(cam, rt[k], &x, &y)) rn[k] = packXY(x, y);
    }
    for (size_t k = 0; k < rt.size(); ++k) {
      if (!all && rn[k] == s.routeSent[k]) continue;
      out->push_back(xyReply("SR_MapRtXY", static_cast<int32_t>(k), rn[k]));
      s.routeSent[k] = rn[k];
    }
  }
  for (size_t i = 0; i < mk.size(); ++i) {
    if (!all && now[i] == s.sent[i]) continue;
    out->push_back(xyReply("SR_MapXY", static_cast<int32_t>(i), now[i]));
    s.sent[i] = now[i];
  }
}

std::vector<std::string> words(const std::string& a) {
  std::vector<std::string> w;
  size_t i = 0;
  while (i < a.size()) {
    while (i < a.size() && a[i] == ' ') ++i;
    size_t j = i;
    while (j < a.size() && a[j] != ' ') ++j;
    if (j > i) w.push_back(a.substr(i, j - i));
    i = j;
  }
  return w;
}

}  // namespace

std::optional<double> luaGlobalOver(const std::function<const uint8_t*(uint32_t page)>& page, const std::string& name) {
  LiveReader r([page](uint32_t pg) -> S::PageView {
    const uint8_t* p = page(pg);
    return p ? S::PageView{p, 4096} : S::PageView{};
  });
  return r.lua(name);
}

void mapSetTestReader(GameReader* reader, const Camera* cam, const std::vector<MapDef>* maps) {
  Session& s = session();
  std::lock_guard<std::mutex> lk(s.mu);
  s.testReader = reader;
  s.testCam = cam;
  s.testMaps = maps;
  s.open = false;
  s.hiddenTypes.clear();
  s.seenTriggers.clear();
  s.selected = -1;
}

void mapCommand(const std::string& args, std::vector<ModReply>* replies) {
  std::vector<ModReply> local;
  std::vector<ModReply>* out = replies ? replies : &local;
  Session& s = session();
  std::lock_guard<std::mutex> lk(s.mu);
  const auto w = words(args);
  const std::string verb = w.empty() ? "" : w[0];
  std::unique_ptr<LiveReader> live;
  GameReader* g = s.testReader;
  if (!g) {
    live = std::make_unique<LiveReader>(livePage);
    g = live.get();
  }
  auto cameraNow = [&]() -> std::optional<Camera> {
    if (s.testCam) return *s.testCam;
    return live ? live->camera() : std::nullopt;
  };
  if (verb == "open") {
    std::vector<std::string> notes;
    const std::vector<MapDef> maps = s.testMaps ? *s.testMaps : activeMaps(&notes);
    CachedReader cached(*g, s.seenTriggers);
    s.ev = evaluate(maps, cached);
    s.sent.clear();
    s.selected = -1;
    s.routeSent.clear();
    s.open = true;
    ++s.opens;
    int nPos = 0, nDone = 0;
    for (const auto& m : s.ev.markers) {
      nPos += m.hasPos ? 1 : 0;
      nDone += (m.state & kCompleted) ? 1 : 0;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP] open #%llu map=%s types=%zu markers=%zu (with position %d, completed %d) worlds=%zu",
                static_cast<unsigned long long>(s.opens), s.ev.available ? s.ev.mapId.c_str() : "(none)", s.ev.types.size(),
                s.ev.markers.size(), nPos, nDone, g->worlds().size());
    for (size_t i = 0; i < s.ev.types.size(); ++i)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP]   type %zu %s '%s'", i, s.ev.types[i].id.c_str(), s.ev.typeLabels[i].c_str());
    for (const auto& m : s.ev.markers)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP]   marker %s type=%s state=%u pos=%s(%.0f %.0f %.0f) title='%s' desc='%s' / '%s'",
                  m.id.c_str(), m.type.c_str(), m.state, m.hasPos ? "" : "none", m.pos[0], m.pos[1], m.pos[2], m.title.c_str(),
                  m.desc.c_str(), m.desc2.c_str());
    {  // the game state the markers were evaluated from (the evidence a reviewer checks them against)
      std::string st;
      auto add = [&](const std::string& n) {
        const auto v = g->das(n, "");
        char b[64];
        std::snprintf(b, sizeof b, " %s=%s", n.c_str(), v ? S::h8(static_cast<uint32_t>(*v)).c_str() : "?");
        st += b;
      };
      for (int i = 0; i < 5; ++i) add("objectiveStatus" + std::to_string(i));
      for (int i = 0; i < 4; ++i) add("kittens" + std::to_string(i));
      for (const char* n : {"kittensFound", "bizarroStatuesFound", "RoutesComplete", "objectiveActivated", "objectiveBitIndex"}) add(n);
      SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP]   state:%s", st.c_str());
    }
    {  // the live trigger census the story / metro generators saw
      std::map<std::string, std::pair<int, int>> census;   // type -> (seen, available or engaged)
      for (const auto& t : g->triggers()) {
        auto& c = census[t.metro ? "METROEVENT" : t.type.empty() ? "(none)" : t.type];
        ++c.first;
        if (t.available || t.engaged) ++c.second;
      }
      std::string line;
      for (const auto& kv : census) line += " " + kv.first + "=" + std::to_string(kv.second.first) + "/" + std::to_string(kv.second.second);
      SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP]   triggers (type=seen/available):%s", line.c_str());
    }
    for (const auto& n : notes) SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP]   note: %s", n.c_str());
    for (const auto& n : s.ev.warnings) SR_LOG_BOOT(sr::LogSeverity::Warn, "[MAP]   unreadable: %s", n.c_str());
    describe(s, out);
    if (const auto cam = cameraNow()) positions(s, *cam, true, out);
    return;
  }
  if (verb == "tick") {
    if (!s.open) return;
    const auto cam = cameraNow();
    if (!cam) return;
    ++s.ticks;
    positions(s, *cam, false, out);
    if (s.ticks == 90) {   // where the markers are once the map camera settled (alignment evidence; kittens counted)
      int kit = 0;
      for (size_t i = 0; i < s.ev.markers.size() && i < s.sent.size(); ++i) {
        const int32_t p = s.sent[i];
        if (p < 0) continue;
        if (s.ev.markers[i].type == "kitten") { ++kit; continue; }
        SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP] stage %s at (%d,%d)", s.ev.markers[i].id.c_str(), (p >> 15) - 16384,
                    (p & 0x7FFF) - 16384);
      }
      SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP] stage kittens with a position: %d", kit);
      if (live) {
        if (const auto p = live->superman())
          SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP] stage superman world=(%.0f %.0f %.0f)", (*p)[0], (*p)[1], (*p)[2]);
      }
      logRoute(s);
    }
    // the self-check: our projection of Superman vs the (x, y) the game just gave SetSupermanIcon
    if (live && w.size() >= 3 && (s.ticks <= 3 || s.ticks % 60 == 0)) {
      const double gx = std::atof(w[1].c_str()), gy = std::atof(w[2].c_str());
      float ox = 0, oy = 0;
      const auto p = live->superman();
      if (p && project(*cam, *p, &ox, &oy)) {
        const double d = std::hypot(ox - gx, oy - gy);
        s.worstCheck = std::max(s.worstCheck, d);
        SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP] check tick=%llu superman game=(%.1f,%.1f) ours=(%.1f,%.1f) d=%.2f px (worst %.2f)",
                    static_cast<unsigned long long>(s.ticks), gx, gy, ox, oy, d, s.worstCheck);
      } else {
        SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP] check tick=%llu superman game=(%.1f,%.1f) ours=unavailable",
                    static_cast<unsigned long long>(s.ticks), gx, gy);
      }
    }
    return;
  }
  if (verb == "filter" && w.size() >= 3) {
    // a type index of the open map (the screen's legend row) or a type id
    std::string id = w[1];
    if (!id.empty() && id.find_first_not_of("0123456789") == std::string::npos) {
      const size_t k = static_cast<size_t>(std::atoi(id.c_str()));
      if (k < s.ev.types.size()) id = s.ev.types[k].id;
    }
    if (w[2] == "0") s.hiddenTypes.insert(id);
    else s.hiddenTypes.erase(id);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP] filter %s %s", id.c_str(), w[2] == "0" ? "hidden" : "shown");
    return;
  }
  if (verb == "select" && w.size() >= 2) {
    const int i = std::atoi(w[1].c_str());
    s.selected = (i >= 0 && static_cast<size_t>(i) < s.ev.markers.size()) ? i : -1;
    s.routeSent.clear();
    const size_t n = s.selected >= 0 ? s.ev.markers[s.selected].route.size() : 0;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP] select %d %s route=%zu", i, s.selected >= 0 ? s.ev.markers[s.selected].id.c_str() : "-", n);
    out->push_back({"SR_MapRoute", {ModArg::Int(i), ModArg::Int(static_cast<int32_t>(n))}});
    if (n) {
      if (const auto cam = cameraNow()) positions(s, *cam, false, out);
      logRoute(s);
    }
    return;
  }
  if (verb == "dbg" && s.dbgLines < 200) {   // the screen's own diagnostics (capped per open)
    ++s.dbgLines;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP] dbg %s", args.size() > 4 ? args.c_str() + 4 : "");
    return;
  }
  if (verb == "key" && w.size() >= 2) {   // a pad code that reached ScrMap.HandleInput (input diagnostics)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP] key %s", w[1].c_str());
    return;
  }
  if (verb == "close") {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MAP] close after %llu ticks (worst self-check %.2f px)",
                static_cast<unsigned long long>(s.ticks), s.worstCheck);
    s.open = false;
    s.ticks = 0;
    return;
  }
}

}  // namespace mapmk
}  // namespace kernel
}  // namespace sr
