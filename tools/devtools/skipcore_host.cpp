// skipcore_host.cpp - the kernel level-skip core (kernel/devtools_skip_core.cpp) as an
// offline task host, for the differential against tools/devtools/skipexec.py (the
// reference) in tools/devtools/test_skipcore_cpp.py. Test tool only; never in the exe.
//
// Protocol (the same as frida_skip.js's offline host, tools/devtools/jshost.py): one JSON
// object per stdin line, one JSON reply per stdout line.
//   {"op":"pages","pages":{"<page>":"<hex>"|null,...}}  -> {"ok":true,"n":N}
//   {"op":"reset"}                                       -> {"ok":true}
//   {"op":"parse","text":T,"expand":bool}                -> {"ok":true,"steps":[...],"lines":[...]} | {"ok":false,"error":..}
//   {"op":"report","graph":G,"steps":[[prim,idx,flow]],"conds":[..]} -> {"ok":true,"report":{..}}
//   {"op":"sim", plan, graph, ticks, variants, patches, caps, times, polls, idles} -> {"ok":true,"sim":{..}}
//   {"op":"hash","names":[..]}                           -> {"ok":true,"fnv1":[..],"hash31":[..]}
// A page the task needs and was not given -> {"ok":false,"miss":<page>} (the host serves it
// from the image and retries), so the core sees exactly the bytes objstate.py sees.
//
// Build: tools/devtools/test_skipcore_cpp.py compiles this file with the core using clang++
// (and the CMake target sr_skipcore_host builds the same two files).
#include "kernel/devtools_skip_core.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace sr::devtools::skip;

namespace {

// ------------------------------------------------------------------ JSON parse ----
struct Parser {
  const std::string& s;
  size_t i = 0;
  explicit Parser(const std::string& x) : s(x) {}
  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
  }
  [[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string("json: ") + what); }
  static void putUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) out += char(c);
    else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) {
      out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F));
      out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F));
    }
  }
  uint32_t hex4() {
    if (i + 4 > s.size()) fail("short \\u");
    uint32_t v = std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
    i += 4;
    return v;
  }
  std::string str() {
    if (s[i] != '"') fail("expected string");
    i++;
    std::string out;
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c != '\\') { out += c; continue; }
      char e = s[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t c1 = hex4();
          if (c1 >= 0xD800 && c1 < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            i += 2;
            uint32_t c2 = hex4();
            c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
          }
          putUtf8(out, c1);
          break;
        }
        default: fail("bad escape");
      }
    }
    i++;
    return out;
  }
  J val() {
    ws();
    if (i >= s.size()) fail("eof");
    char c = s[i];
    if (c == '{') {
      i++;
      J o = J::obj();
      ws();
      if (s[i] == '}') { i++; return o; }
      for (;;) {
        ws();
        std::string k = str();
        ws();
        if (s[i++] != ':') fail("expected :");
        o.set(k, val());
        ws();
        if (s[i] == ',') { i++; continue; }
        if (s[i] == '}') { i++; return o; }
        fail("expected , or }");
      }
    }
    if (c == '[') {
      i++;
      J a = J::arr();
      ws();
      if (s[i] == ']') { i++; return a; }
      for (;;) {
        a.push(val());
        ws();
        if (s[i] == ',') { i++; continue; }
        if (s[i] == ']') { i++; return a; }
        fail("expected , or ]");
      }
    }
    if (c == '"') return J::str(str());
    if (s.compare(i, 4, "true") == 0) { i += 4; return J::boolean(true); }
    if (s.compare(i, 5, "false") == 0) { i += 5; return J::boolean(false); }
    if (s.compare(i, 4, "null") == 0) { i += 4; return J::null(); }
    size_t j = i;
    bool flt = false;
    while (j < s.size() && (std::strchr("+-0123456789.eE", s[j]))) {
      if (s[j] == '.' || s[j] == 'e' || s[j] == 'E') flt = true;
      j++;
    }
    if (j == i) fail("bad value");
    std::string num = s.substr(i, j - i);
    i = j;
    if (flt) return J::dbl(std::strtod(num.c_str(), nullptr));
    return J::num(std::strtoll(num.c_str(), nullptr, 10));
  }
};

// ------------------------------------------------------------------ pages ----
std::map<uint32_t, std::optional<std::vector<uint8_t>>> g_pages;

std::vector<uint8_t> unhex(const std::string& h) {
  std::vector<uint8_t> b(h.size() / 2);
  for (size_t k = 0; k < b.size(); k++) b[k] = uint8_t(std::strtoul(h.substr(2 * k, 2).c_str(), nullptr, 16));
  return b;
}

PageView offline(uint32_t pg) {
  auto it = g_pages.find(pg);
  if (it == g_pages.end()) throw Miss{pg};
  if (!it->second) return {};
  return {it->second->data(), uint32_t(it->second->size())};
}

Graph graphOf(const J* g, bool* present) {
  Graph out;
  *present = g && g->isObj() && g->get("ids") && g->get("trans");   // Python: an empty dict is no graph
  if (!*present) return out;
  for (const J& x : g->get("ids")->a()) out.ids.push_back(uint32_t(x.isNull() ? 0xFFFFFFFFu : x.i()));
  for (const J& t : g->get("trans")->a()) {
    std::vector<int64_t> v;
    for (const J& x : t.a()) v.push_back(x.isNull() ? -1 : x.i());
    out.trans.push_back(v);
  }
  const J* fo = g->get("fail_objective");
  const J* fg = g->get("fail_game_over");
  if (fo && !fo->isNull() && fg && !fg->isNull()) {
    out.hasFail = true;
    for (const J& x : fo->a()) out.failObjective.push_back(uint32_t(x.i()));
    for (const J& x : fg->a()) out.failGameOver.push_back(uint32_t(x.i()));
  }
  return out;
}

J okJ() {
  J o = J::obj();
  o.set("ok", J::boolean(true));
  return o;
}

J runParse(const J& task) {
  J out = J::obj();
  try {
    auto steps = parsePlan(task.get("text")->s());
    const J* ex = task.get("expand");
    if (ex && ex->b()) steps = expandDefaults(steps);
    J st = J::arr(), ln = J::arr();
    for (const auto& s : steps) {
      st.push(stepJson(s));
      ln.push(J::num(s.line));
    }
    out.set("ok", J::boolean(true));
    out.set("steps", std::move(st));
    out.set("lines", std::move(ln));
  } catch (const PlanError& e) {
    out.set("ok", J::boolean(false));
    out.set("error", J::str(e.msg));
  }
  return out;
}

J runReport(const J& task) {
  bool present = false;
  const Graph g = graphOf(task.get("graph"), &present);
  std::vector<StepKey> keys;
  for (const J& k : task.get("steps")->a()) {
    StepKey sk;
    sk.prim = k.a()[0].s();
    if (!k.a()[1].isNull()) sk.idx = k.a()[1].i();
    if (!k.a()[2].isNull()) sk.flow = k.a()[2].s();
    if (k.a().size() > 3 && !k.a()[3].isNull()) sk.arg = k.a()[3].s();   // DT-2 commands
    keys.push_back(sk);
  }
  std::vector<std::string> conds;
  for (const J& c : task.get("conds")->a()) conds.push_back(c.s());
  World w(offline, present ? &g : nullptr);
  J out = okJ();
  out.set("report", report(w, keys, conds));
  return out;
}

J runSim(const J& task) {
  bool present = false;
  const Graph g = graphOf(task.get("graph"), &present);
  std::map<uint32_t, std::map<uint32_t, uint8_t>> overlay;   // page -> off -> byte
  std::map<uint32_t, std::vector<uint8_t>> patched;          // scratch copies per lookup
  auto poke = [&](uint32_t va, const std::vector<uint8_t>& b) {
    for (size_t k = 0; k < b.size(); k++) {
      const uint32_t a = va + uint32_t(k);
      overlay[a >> 12][a & 0xFFF] = b[k];
    }
  };
  auto src = [&](uint32_t pg) -> PageView {
    PageView p = offline(pg);
    auto it = overlay.find(pg);
    if (it == overlay.end() || it->second.empty()) return p;
    if (!p.p) return {};
    std::vector<uint8_t>& c = patched[pg];
    c.assign(p.p, p.p + p.len);
    for (const auto& kv : it->second)
      if (kv.first < c.size()) c[kv.first] = kv.second;
    return {c.data(), uint32_t(c.size())};
  };
  auto writeW = [&](const J& w) {
    const std::string hx = w.get("new")->s().substr(2);
    std::vector<uint8_t> b = unhex(hx);
    b.resize(size_t(w.get("width")->i()));
    poke(uint32_t(std::stoul(w.get("va")->s(), nullptr, 16)), b);
  };
  const auto steps = parsePlan(task.get("plan")->s());
  const J* times = task.get("times");
  const bool haveTimes = times && times->isArr();
  std::optional<double> now;
  if (haveTimes && !times->a().empty()) now = times->a()[0].d();
  const J* caps = task.get("caps");
  const bool pad = !(caps && caps->isObj() && caps->get("pad") && !caps->get("pad")->b());
  Executor ex(steps, [&] { return World(src, present ? &g : nullptr); }, writeW, pad, [&] { return now; });
  const int64_t ticks = task.get("ticks")->i();
  const J* variants = task.get("variants");
  const J* patches = task.get("patches");
  const J* polls = task.get("polls");
  const J* idles = task.get("idles");
  for (int64_t t = 1; t <= ticks + 1; t++) {
    if (polls && polls->isArr())
      for (const J& q : polls->a())
        if (q.get("before_tick")->i() == t) {
          now = q.get("now")->d();
          ex.poll(q.get("now")->d());
        }
    if (idles && idles->isArr())
      for (const J& q : idles->a())
        if (q.get("before_tick")->i() == t && ex.status() == "running") {
          if (const J* ps = q.get("patches"); ps && ps->isArr())
            for (const J& p : ps->a()) poke(uint32_t(std::stoul(p.get("va")->s(), nullptr, 16)), unhex(p.get("hex")->s()));
          ex.idlePad();
        }
    if (t > ticks || ex.status() != "running") break;
    if (patches && patches->isArr())
      for (const J& p : patches->a())
        if (p.get("tick")->i() == t) poke(uint32_t(std::stoul(p.get("va")->s(), nullptr, 16)), unhex(p.get("hex")->s()));
    if (haveTimes) now = times->a()[size_t(t - 1)].d();
    std::string v = "active";
    if (variants && variants->isArr() && !variants->a().empty())
      v = variants->a()[size_t((t - 1) % int64_t(variants->a().size()))].s();
    ex.tick(v);
  }
  J sim = J::obj();
  J ev = J::arr();
  for (const J& e : ex.events()) ev.push(e);
  sim.set("events", std::move(ev));
  sim.set("status", J::str(ex.status()));
  sim.set("k", J::num(int64_t(ex.k())));
  sim.set("tick", J::num(ex.tickAll()));
  sim.set("tick_active", J::num(ex.tickActive()));
  J out = okJ();
  out.set("sim", std::move(sim));
  return out;
}

J runTask(const J& task) {
  const std::string op = task.get("op")->s();
  try {
    if (op == "pages") {
      int n = 0;
      for (const auto& kv : task.get("pages")->o()) {
        const uint32_t pg = uint32_t(std::stoul(kv.first));
        if (kv.second.isNull()) g_pages[pg] = std::nullopt;
        else g_pages[pg] = unhex(kv.second.s());
        n++;
      }
      J o = okJ();
      o.set("n", J::num(n));
      return o;
    }
    if (op == "reset") {
      g_pages.clear();
      return okJ();
    }
    if (op == "parse") return runParse(task);
    if (op == "report") return runReport(task);
    if (op == "sim") return runSim(task);
    if (op == "hash") {
      J f = J::arr(), h = J::arr();
      for (const J& n : task.get("names")->a()) {
        // the names arrive as UTF-8; the guest strings the ports hash are latin1 bytes
        std::string l1;
        const std::string& u = n.s();
        for (size_t k = 0; k < u.size(); k++) {
          const unsigned char c = static_cast<unsigned char>(u[k]);
          if (c >= 0xC0 && k + 1 < u.size()) {
            l1 += static_cast<char>(((c & 0x1F) << 6) | (static_cast<unsigned char>(u[k + 1]) & 0x3F));
            k++;
          } else {
            l1 += static_cast<char>(c);
          }
        }
        f.push(J::num(fnv1(l1)));
        h.push(J::num(hash31(l1)));
      }
      J o = okJ();
      o.set("fnv1", std::move(f));
      o.set("hash31", std::move(h));
      return o;
    }
    J o = J::obj();
    o.set("ok", J::boolean(false));
    o.set("error", J::str("unknown op " + op));
    return o;
  } catch (const Miss& m) {
    J o = J::obj();
    o.set("ok", J::boolean(false));
    o.set("miss", J::num(m.pg));
    return o;
  } catch (const PlanError& e) {
    J o = J::obj();
    o.set("ok", J::boolean(false));
    o.set("error", J::str("plan: " + e.msg));
    return o;
  } catch (const Refusal& e) {
    J o = J::obj();
    o.set("ok", J::boolean(false));
    o.set("error", J::str("uncaught refusal " + e.code));
    return o;
  } catch (const ReadErr& e) {
    J o = J::obj();
    o.set("ok", J::boolean(false));
    o.set("error", J::str("uncaught ReadErr " + e.msg));
    return o;
  } catch (const UnknownErr& e) {
    J o = J::obj();
    o.set("ok", J::boolean(false));
    o.set("error", J::str("uncaught UnknownErr " + e.msg));
    return o;
  } catch (const std::exception& e) {
    J o = J::obj();
    o.set("ok", J::boolean(false));
    o.set("error", J::str(std::string("exception ") + e.what()));
    return o;
  }
}

}  // namespace

int main() {
  std::ios::sync_with_stdio(false);
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    J out;
    try {
      Parser p(line);
      out = runTask(p.val());
    } catch (const std::exception& e) {
      out = J::obj();
      out.set("ok", J::boolean(false));
      out.set("error", J::str(std::string("host: ") + e.what()));
    }
    std::string s = out.dump();
    s += '\n';
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
  }
  return 0;
}
