// iter-464 (modmenu lane): the shared engine of the host-driven menu pages. Contract:
// host_pages.hpp. Extracted from kernel/devtools_menu.cpp (fe lane) so the Dev Tools menu and
// the MODS menu run the same dispatcher and talk to the same Apt renderer.
//
// Charter: host UI plumbing. No guest memory, no guest calls.
#include "host_pages.hpp"

#include "devtools_menu.hpp"
#include "display_menu.hpp"
#include "cheats_menu.hpp"
#include "mod_menu.hpp"
#include "../app/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace sr {
namespace kernel {

namespace {

void emit(const HostPage& p, int focus, int action, std::vector<ModReply>* out) {
  const int n = std::min<int>(static_cast<int>(p.rows.size()), kHostPageMaxRows);
  out->push_back({"SR_DTBegin", {ModArg::Str(p.id), ModArg::Str(p.title), ModArg::Str(p.body)}});
  for (int i = 0; i < n; ++i)
    out->push_back({"SR_DTItem", {ModArg::Int(i), ModArg::Str(p.rows[i].label), ModArg::Int(int(p.rows[i].kind))}});
  out->push_back({"SR_DTEnd", {ModArg::Int(n), ModArg::Int(std::max(0, std::min(focus, n - 1))), ModArg::Int(action)}});
}

}  // namespace

std::string hostUpper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

void hostPageSplit(const std::string& id, std::string* name, int* off) {
  const size_t c = id.find(':');
  *name = id.substr(0, c);
  *off = 0;
  if (c != std::string::npos) *off = std::max(0, std::atoi(id.c_str() + c + 1));
}

void hostPagesCommand(const std::string& args, const HostPageBuilder& build, std::vector<ModReply>* out,
                      std::string* ctxOut, const char* logTag) {
  std::istringstream ss(args);
  std::string verb, page;
  ss >> verb >> page;
  int row = -1, delta = 0;
  if (verb == "open") {
    std::string ctx;
    ss >> ctx;
    if (ctxOut && (ctx == "FE" || ctx == "IG")) *ctxOut = ctx;
  } else {
    ss >> row >> delta;
  }
  const HostPage cur = build(page);
  HostPageOut o;
  o.page = cur.id;
  o.focus = row >= 0 ? row : 0;
  if (verb == "open") {
    o.focus = 0;
  } else if (verb == "back") {
    if (cur.back) cur.back(o);
    else o.action = 0;
  } else if (verb == "press") {
    if (row >= 0 && row < static_cast<int>(cur.rows.size()) && cur.rows[row].press) cur.rows[row].press(o);
  } else if (verb == "adjust") {
    if (row >= 0 && row < static_cast<int>(cur.rows.size()) && cur.rows[row].adjust && (delta == 1 || delta == -1)) {
      cur.rows[row].adjust(o, delta);
    } else {
      o.action = 2;   // nothing to change: the AS keeps its page (rows rebuilt unchanged)
    }
  } else {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[%s] menu: unknown verb '%s'", logTag, verb.c_str());
  }
  if (o.action == 0) {
    out->push_back({"SR_DTEnd", {ModArg::Int(0), ModArg::Int(0), ModArg::Int(0)}});
    return;
  }
  emit(build(o.page), o.focus, o.action, out);
}

void menuPagesCommand(const std::string& args, std::vector<ModReply>* out) {
  std::istringstream ss(args);
  std::string verb, page;
  ss >> verb >> page;
  if (modMenuOwnsPage(page)) modMenuPagesCommand(args, out);
  else if (displayMenuOwnsPage(page)) displayMenuCommand(args, out);   // iter-465 polish
  else if (cheatsMenuOwnsPage(page)) cheatsMenuCommand(args, out);     // iter-465 charswitch
  else devMenuCommand(args, out);
}

HostPageForTest hostPageDecodeForTest(const std::vector<ModReply>& calls) {
  HostPageForTest t;
  for (const auto& c : calls) {
    if (c.method == "SR_DTBegin" && c.args.size() == 3) {
      t.page = c.args[0].s;
      t.title = c.args[1].s;
      t.body = c.args[2].s;
    } else if (c.method == "SR_DTItem" && c.args.size() == 3) {
      t.labels.push_back(c.args[1].s);
      t.kinds.push_back(c.args[2].i);
    } else if (c.method == "SR_DTEnd" && c.args.size() == 3) {
      t.focus = c.args[1].i;
      t.action = c.args[2].i;
    }
  }
  return t;
}

}  // namespace kernel
}  // namespace sr
