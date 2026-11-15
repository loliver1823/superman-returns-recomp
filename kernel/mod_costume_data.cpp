#include "mod_costume_data.hpp"
#include "mod_formats.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <cstdio>
#include <set>

namespace sr::kernel {
const char* const kRetailCostumeFamily = "retail";
const char* const kSupermanBanks[6] = {"SUPERMAN_FLIGHT", "SUPERMAN_ONFOOT", "SUPERMAN_MELEEGROUND",
                                       "SUPERMAN_SUPER_POWERS", "SUPERMAN_SUPERSPEED_ATTACKS",
                                       "SUPERMAN_MELEEAIR"};
namespace {
uint64_t little(const uint8_t* p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) v |= uint64_t(p[i]) << (i * 8);
  return v;
}
uint32_t big(const uint8_t* p, size_t n) {
  uint32_t v = 0;
  for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
  return v;
}
std::string escape(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default:
        // Compiled XML strings are Latin-1, not UTF-8. Numeric references keep
        // every byte's code point without silently changing the document.
        if (c >= 128 || c < 32) out += "&#" + std::to_string(c) + ";";
        else out += static_cast<char>(c);
    }
  }
  return out;
}
}

bool modReadNestedEntry(const std::vector<uint8_t>& b, const std::string& entry,
                        std::vector<uint8_t>* out, std::string* err) {
  auto bad = [&](const char* why) { *err = why; return false; };
  if (b.size() < 64 || std::memcmp(b.data(), "BGFA1.05", 8)) return bad("nested entry is not uncompressed BGFA1.05");
  const uint64_t count = little(b.data() + 12, 4), toc = little(b.data() + 16, 4);
  const uint64_t tocLen = little(b.data() + 24, 4), tags = little(b.data() + 40, 4), names = little(b.data() + 44, 4);
  const size_t wa = b[33], wi = b[34], wo = b[35], ws = b[36], wu = b[37], shift = b[38];
  if (wa > 8 || wi > 8 || wo > 8 || ws > 8 || wu > 8 || !wi || !wo || !ws || shift > 31 || names > 4096)
    return bad("invalid nested BGFA widths");
  const uint64_t record = 1 + wa + wi + wo + ws + wu + names;
  if (toc > b.size() || tocLen > b.size() - toc || 4 * tags > tocLen || count > (tocLen - 4 * tags) / record)
    return bad("nested BGFA TOC out of range");
  const uint64_t key = fnv1a64Upper(entry);
  for (uint64_t i = 0; i < count; ++i) {
    const uint8_t* p = b.data() + toc + tags * 4 + i * record + 1 + wa;
    const uint64_t id = little(p, wi); p += wi;
    const uint64_t units = little(p, wo); p += wo;
    const uint64_t size = little(p, ws);
    if (id != (wi == 8 ? key : key & ((uint64_t(1) << (wi * 8)) - 1))) continue;
    if (units > (uint64_t(-1) >> shift)) return bad("nested BGFA offset overflow");
    const uint64_t off = units << shift;
    if (off > b.size() || size > b.size() - off) return bad("nested BGFA payload out of range");
    out->assign(b.begin() + static_cast<size_t>(off), b.begin() + static_cast<size_t>(off + size));
    return true;
  }
  return bad("nested BGFA entry missing");
}

bool modComposeCostumeAttributes(const std::vector<uint8_t>& b,
                                 const std::vector<ModCostumeInfo>& additions,
                                 std::vector<uint8_t>* out, std::string* err, int startupSlot,
                                 const std::string& activeFamily) {
  auto bad = [&](const char* why) { *err = why; return false; };
  // Only the verified flat <attributes><attribute .../></attributes> schema.
  // An unexpected document fails closed rather than losing fields or retail suits.
  if (b.size() < 44 || big(b.data(), 4) != 0x05F5E0FF || big(b.data() + 4, 4) != 0x14 || big(b.data() + 20, 4) != 44)
    return bad("costume base must be compiled attribute XML");
  const size_t ne = big(b.data() + 16, 4) >> 16, ns = big(b.data() + 24, 4) & 0xFFFF;
  const size_t blobLen = big(b.data() + 32, 4), blob = 44 + 12 * ns;
  if (!ne || blob > b.size() || blobLen > b.size() - blob || ne > (b.size() - blob - blobLen) / 12)
    return bad("compiled attribute XML bounds");
  const size_t elements = blob + blobLen;
  std::vector<std::string> strings;
  for (size_t i = 0; i < ns; ++i) {
    const size_t len = big(b.data() + 44 + i * 12, 4), off = big(b.data() + 48 + i * 12, 4);
    if (off > blobLen || len > blobLen - off) return bad("compiled XML string bounds");
    strings.emplace_back(reinterpret_cast<const char*>(b.data() + blob + off), len);
    if (strings.back().find('\0') != std::string::npos) return bad("compiled XML embedded NUL");
  }
  std::vector<std::vector<std::pair<std::string, std::string>>> records;
  std::set<std::string> keys;
  for (size_t i = 0; i < ne; ++i) {
    const uint8_t* e = b.data() + elements + i * 12;
    const size_t off = big(e, 4), count = big(e + 4, 2), name = big(e + 6, 2), end = big(e + 8, 2);
    if (name >= strings.size() || off > b.size() || count > (b.size() - off) / 4 ||
        (i == 0 ? (strings[name] != "attributes" || end != ne - 1 || count != 0)
                : (strings[name] != "attribute" || end != i))) return bad("unsupported attribute XML tree");
    if (!i) continue;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::map<std::string, std::string> values;
    for (size_t j = 0; j < count; ++j) {
      const size_t k = big(b.data() + off + j * 4, 2), v = big(b.data() + off + j * 4 + 2, 2);
      if (k >= strings.size() || v >= strings.size() || !values.emplace(strings[k], strings[v]).second)
        return bad("invalid or duplicate XML attribute");
      // XML field names are engine tokens, never arbitrary markup.
      if (strings[k].empty() || !((strings[k][0] >= 'A' && strings[k][0] <= 'Z') ||
                                  (strings[k][0] >= 'a' && strings[k][0] <= 'z') || strings[k][0] == '_') ||
          !modValidId(strings[k])) return bad("invalid XML field name");
      attrs.emplace_back(strings[k], strings[v]);
    }
    if (!values.count("key") || !keys.insert(values["key"]).second) return bad("duplicate or missing DAS key");
    records.push_back(std::move(attrs));
  }
  for (const char* key : {"SetCostumeNum", "Costume1", "Costume2", "Costume3", "Costume4"})
    if (!keys.count(key)) return bad("retail costume entry missing");
  std::set<std::string> identities, entities;
  for (const auto& c : additions) {
    const std::string key = "Costume" + std::to_string(c.slot);
    if (c.slot < 5 || !modValidId(c.entity) || c.id.empty() || !identities.insert(c.id).second ||
        !entities.insert(modLower(c.entity)).second || !keys.insert(key).second)
      return bad("duplicate or invalid costume addition");
    records.push_back({{"key", key}, {"type", "String"}, {"category", "0"}, {"constant", "1"}, {"value", c.entity}});
  }
  // iter-465 Fortress costumes (port feature). The actor flow's switcher script
  // (modComposeCostumeSwitchLua) reads these; the retail entries above are unchanged.
  //   SRCostumeFamily<n>  the suit's animation family: "retail" (the disc's SupermanAnims banks),
  //                       "banks_<hash>" (a suit that ships its own banks, docs/MODS.md) or an
  //                       own animation project. A game session animates ONE family (the banks
  //                       inside baseworl.AST load once, at start), so a suit switches at once
  //                       (retail ReplaceEntityVisRep) only within SRActiveFamily
  //   SRActiveFamily      this session's family (constant: chosen at boot from the worn suit)
  //   SRWornCostume       the suit Superman wears (0 = never chosen: the retail default);
  //                       its file value is the player's saved choice at boot
  //   SRDressedSuit       the suit the switcher or LoadMetropolis last put on (a worn retail
  //                       suit the loaded save has not unlocked: 1, the default)
  //   SRAppliedTo         the Superman entity the switcher last dressed (a game respawn
  //                       shows up as a different id and is dressed again)
  if (startupSlot != 0) {
    bool valid = startupSlot >= 1 && startupSlot <= 3;
    for (const auto& c : additions) valid = valid || c.slot == startupSlot;
    if (!valid) return bad("startup costume slot is not present");
  }
  if (!modValidId(activeFamily)) return bad("invalid active costume family");
  for (int n = 1; n <= 3; ++n)
    records.push_back({{"key", "SRCostumeFamily" + std::to_string(n)}, {"type", "String"}, {"category", "0"},
                       {"constant", "1"}, {"value", kRetailCostumeFamily}});
  for (const auto& c : additions) {
    const std::string family = modCostumeFamily(c);
    if (!modValidId(family)) return bad("invalid costume family");
    records.push_back({{"key", "SRCostumeFamily" + std::to_string(c.slot)}, {"type", "String"}, {"category", "0"},
                       {"constant", "1"}, {"value", family}});
  }
  // iter-465 character switch (port feature, docs/MODS.md "Character switch"):
  //   SRCostumeChar<n>  1 = a whole character on its OWN animation project (the suit names an
  //                     animation_project): worn by spawning that character entity in Superman's
  //                     place the way PLAYABLEBIZ swaps in Bizarro; 0 = a model on Superman
  //   SRCharID          the character entity out now (0 = Superman himself is out)
  //   SRSupesID         the game's own Superman, parked and hidden while a character is out
  for (int n = 1; n <= 3; ++n)
    records.push_back({{"key", "SRCostumeChar" + std::to_string(n)}, {"type", "Integer"}, {"category", "0"},
                       {"constant", "1"}, {"value", "0"}});
  for (const auto& c : additions)
    records.push_back({{"key", "SRCostumeChar" + std::to_string(c.slot)}, {"type", "Integer"}, {"category", "0"},
                       {"constant", "1"}, {"value", (c.character || !c.animationProject.empty()) ? "1" : "0"}});
  for (const char* key : {"SRWornCostume", "SRAppliedTo", "SRDressedSuit", "SRActiveFamily", "SRCharID", "SRSupesID"})
    if (keys.count(key)) return bad("costume switch attribute already present");
  records.push_back({{"key", "SRCharID"}, {"type", "Integer"}, {"category", "0"}, {"constant", "0"}, {"value", "0"}});
  records.push_back({{"key", "SRSupesID"}, {"type", "Integer"}, {"category", "0"}, {"constant", "0"}, {"value", "0"}});
  records.push_back({{"key", "SRWornCostume"}, {"type", "Integer"}, {"category", "0"}, {"constant", "0"},
                     {"value", std::to_string(startupSlot)}});
  records.push_back({{"key", "SRAppliedTo"}, {"type", "Integer"}, {"category", "0"}, {"constant", "0"}, {"value", "0"}});
  records.push_back({{"key", "SRDressedSuit"}, {"type", "Integer"}, {"category", "0"}, {"constant", "0"}, {"value", "0"}});
  records.push_back({{"key", "SRActiveFamily"}, {"type", "String"}, {"category", "0"}, {"constant", "1"},
                     {"value", activeFamily}});
  std::string text = "<attributes>\n";
  for (const auto& attrs : records) {
    text += "  <attribute";
    for (const auto& [k, v] : attrs) text += " " + k + "=\"" + escape(v) + "\"";
    text += "/>\n";
  }
  text += "</attributes>";
  text += '\0';  // loose text documents require the game's NUL terminator
  out->assign(text.begin(), text.end());
  return true;
}

bool modScopedBankCopy(const std::vector<uint8_t>& bank, const std::string& bankName, const std::string& ns,
                       std::set<uint32_t>* used, std::vector<uint8_t>* out, std::string* err) {
  auto fnv1 = [](const std::string& t) {
    uint32_t h = 0x811C9DC5u;
    for (unsigned char c : t) { h *= 0x01000193u; h ^= c; }
    return h;
  };
  auto put = [](uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); };
  if (!modValidSupermanBank(bank, err)) return false;
  if (bank.size() < 0x34) { *err = "bank header too short"; return false; }
  std::vector<uint8_t> b = bank;
  put(b.data() + 8, fnv1(bankName));
  const uint32_t nd = big(b.data() + 0x2C, 4), pd = big(b.data() + 0x30, 4);
  if (nd > 100000 || pd > b.size() || uint64_t(nd) * 4 > b.size() - pd) { *err = "bank block table out of range"; return false; }
  for (uint32_t i = 0; i < nd; ++i) {
    const uint32_t off = big(b.data() + pd + 4 * i, 4);
    if (off > b.size() || b.size() - off < 8 || big(b.data() + off + 4, 4) != 8) { *err = "bank block is not a clip block"; return false; }
    char hex[16];
    std::snprintf(hex, sizeof hex, "%08x", big(b.data() + off, 4));
    uint32_t h = fnv1(ns + "_" + hex);
    for (int k = 1; used->count(h); ++k) h = fnv1(ns + "_" + hex + "_" + std::to_string(k));
    used->insert(h);
    put(b.data() + off, h);
  }
  out->swap(b);
  return true;
}

bool modCompiledXmlToText(const std::vector<uint8_t>& b, std::vector<uint8_t>* out, std::string* err) {
  return modCompiledXmlToTextEdited(b, nullptr, out, err);
}

bool modCompiledXmlToTextEdited(const std::vector<uint8_t>& b, const ModXmlAttrEdit& edit,
                                std::vector<uint8_t>* out, std::string* err) {
  auto bad = [&](const char* why) { *err = why; return false; };
  // Same header as the attribute/preload documents: magic 0x05F5E0FF, +4 = 0x14, +20 = 44.
  if (b.size() < 44 || b.size() > 16 * 1024 * 1024 || big(b.data(), 4) != 0x05F5E0FF ||
      big(b.data() + 4, 4) != 0x14 || big(b.data() + 20, 4) != 44)
    return bad("not a compiled XML document");
  const size_t ne = big(b.data() + 16, 4) >> 16, ns = big(b.data() + 24, 4) & 0xFFFF;
  const size_t blobLen = big(b.data() + 32, 4), blob = 44 + 12 * ns, elements = blob + blobLen;
  if (!ne || blob > b.size() || blobLen > b.size() - blob || ne > (b.size() - elements) / 12)
    return bad("compiled XML bounds");
  std::vector<std::string> strings;
  for (size_t i = 0; i < ns; ++i) {
    const size_t len = big(b.data() + 44 + i * 12, 4), off = big(b.data() + 48 + i * 12, 4);
    if (off > blobLen || len > blobLen - off) return bad("compiled XML string bounds");
    strings.emplace_back(reinterpret_cast<const char*>(b.data() + blob + off), len);
    if (strings.back().find('\0') != std::string::npos) return bad("compiled XML embedded NUL");
  }
  auto field = [&](size_t i, size_t at, size_t n) { return static_cast<size_t>(big(b.data() + elements + 12 * i + at, n)); };
  std::string text;
  std::vector<size_t> open;  // elements whose subtree is still being written
  auto closeTo = [&](size_t i) {
    while (!open.empty() && field(open.back(), 8, 2) < i) {
      const size_t top = open.back();
      open.pop_back();
      text += std::string(2 * open.size(), ' ') + "</" + strings[field(top, 6, 2)] + ">\n";
    }
  };
  for (size_t i = 0; i < ne; ++i) {
    closeTo(i);
    const size_t off = field(i, 0, 4), count = field(i, 4, 2), name = field(i, 6, 2), end = field(i, 8, 2);
    if (name >= ns || end < i || end >= ne || off > b.size() || count > (b.size() - off) / 4 ||
        (!open.empty() && end > field(open.back(), 8, 2)))
      return bad("compiled XML element bounds");
    text += std::string(2 * open.size(), ' ') + "<" + strings[name];
    std::vector<std::pair<std::string, std::string>> attrs;
    for (size_t j = 0; j < count; ++j) {
      const size_t k = big(b.data() + off + 4 * j, 2), v = big(b.data() + off + 4 * j + 2, 2);
      if (k >= ns || v >= ns) return bad("compiled XML attribute bounds");
      attrs.emplace_back(strings[k], strings[v]);
    }
    if (edit && !edit(strings[name], &attrs, err)) return false;
    for (const auto& [k, v] : attrs) text += " " + k + "=\"" + escape(v) + "\"";
    if (end == i) {
      text += "/>\n";
    } else {
      text += ">\n";
      open.push_back(i);
    }
  }
  closeTo(ne);
  text.push_back('\0');
  out->assign(text.begin(), text.end());
  return true;
}

bool modComposeCostumePreloads(const std::vector<uint8_t>& b,
                              const std::vector<ModCostumeInfo>& additions,
                              std::vector<uint8_t>* out, std::string* err) {
  auto bad = [&](const char* why) { *err = why; return false; };
  // Deliberately accept only the verified three-level preload schema. A mod's
  // unsupported winning document blocks composition, never falls back to disc
  // and silently discards that mod's preloads. Text bases remain future work.
  if (b.size() < 44 || b.size() > 16 * 1024 * 1024 || big(b.data(), 4) != 0x05F5E0FF ||
      big(b.data() + 4, 4) != 0x14 || big(b.data() + 20, 4) != 44)
    return bad("costume preloads need compiled preload XML");
  const size_t ne = big(b.data() + 16, 4) >> 16, ns = big(b.data() + 24, 4) & 0xFFFF;
  const size_t blobLen = big(b.data() + 32, 4), blob = 44 + 12 * ns, elements = blob + blobLen;
  if (!ne || ne > 4096 || ns > 8192 || blob > b.size() || blobLen > b.size() - blob ||
      ne > (b.size() - elements) / 12) return bad("compiled preload XML bounds");
  std::vector<std::string> strings;
  size_t stringBudget = 0;
  for (size_t i = 0; i < ns; ++i) {
    const size_t len = big(b.data() + 44 + i * 12, 4), off = big(b.data() + 48 + i * 12, 4);
    stringBudget += len;
    if (off > blobLen || len > blobLen - off || len > 65536 || stringBudget > 16 * 1024 * 1024)
      return bad("preload string bounds");
    strings.emplace_back(reinterpret_cast<const char*>(b.data() + blob + off), len);
    if (strings.back().find('\0') != std::string::npos) return bad("preload embedded NUL");
  }
  struct Element { std::string name, attrs; size_t end; std::map<std::string, std::string> values; };
  std::vector<Element> nodes;
  size_t outputBudget = 0;
  for (size_t i = 0; i < ne; ++i) {
    const uint8_t* e = b.data() + elements + 12 * i;
    const size_t off = big(e, 4), count = big(e + 4, 2), name = big(e + 6, 2), end = big(e + 8, 2);
    if (name >= ns || end < i || end >= ne || off > b.size() || count > (b.size() - off) / 4)
      return bad("preload element bounds");
    Element node{strings[name], {}, end, {}};
    for (size_t j = 0; j < count; ++j) {
      const size_t key = big(b.data() + off + 4 * j, 2), value = big(b.data() + off + 4 * j + 2, 2);
      if (key >= ns || value >= ns || !modValidId(strings[key]) ||
          !((strings[key][0] >= 'A' && strings[key][0] <= 'Z') ||
            (strings[key][0] >= 'a' && strings[key][0] <= 'z') || strings[key][0] == '_') ||
          !node.values.emplace(strings[key], strings[value]).second)
        return bad("invalid preload attribute");
      node.attrs += " " + strings[key] + "=\"" + escape(strings[value]) + "\"";
      if (node.attrs.size() > 16 * 1024 * 1024) return bad("preload output too large");
    }
    outputBudget += node.attrs.size() + 64;
    if (outputBudget > 16 * 1024 * 1024) return bad("preload output too large");
    nodes.push_back(std::move(node));
  }
  if (nodes[0].name != "preloadDefs" || nodes[0].end != ne - 1) return bad("unsupported preload root");
  std::string text = "<preloadDefs" + nodes[0].attrs + ">\n";
  std::map<std::string, std::string> projects;
  for (size_t i = 1; i < ne;) {
    const auto& group = nodes[i];
    if (group.name != "preload" || !group.values.count("type")) return bad("unsupported preload group");
    text += "  <preload" + group.attrs + ">\n";
    for (size_t j = i + 1; j <= group.end; ++j) {
      const auto& file = nodes[j];
      if (file.name != "file" || file.end != j || !file.values.count("name"))
        return bad("unsupported preload file tree");
      if (group.values.at("type") == "ANIM") {
        const auto& project = file.values.at("name");
        auto [it, inserted] = projects.emplace(modLower(project), project);
        if (!inserted && it->second != project) return bad("case-ambiguous preload project");
      }
      text += "    <file" + file.attrs + "/>\n";
    }
    text += "  </preload>\n";
    i = group.end + 1;
  }
  std::vector<std::string> extra;
  for (const auto& costume : additions) {
    const auto& project = costume.animationProject;
    if (project.empty()) continue;
    if (project.size() > 63 || !modValidId(project) || project.find('.') != std::string::npos ||
        !modValidId(costume.source) || modLower(project).rfind(modLower(modAssetPrefix(costume.source)), 0) != 0)
      return bad("unscoped costume preload project");
    auto [it, inserted] = projects.emplace(modLower(project), project);
    if (!inserted && it->second != project) return bad("case-ambiguous preload project");
    if (inserted) extra.push_back(project);
  }
  if (!extra.empty()) {
    text += "  <preload type=\"ANIM\">\n";
    for (const auto& project : extra) text += "    <file name=\"" + project + "\"/>\n";
    text += "  </preload>\n";
  }
  // built-in characters' resources (e.g. Bizarro's extra projects and arenas), one group each
  std::set<std::string> seenExtra;
  for (const auto& costume : additions)
    for (const auto& [type, name] : costume.preloads) {
      if (!modValidId(type) || !modValidId(name) || !seenExtra.insert(modLower(type + "/" + name)).second) continue;
      if (type == "ANIM" && !projects.emplace(modLower(name), name).second) continue;
      text += "  <preload type=\"" + type + "\">\n    <file name=\"" + name + "\"/>\n  </preload>\n";
    }
  text += "</preloadDefs>";
  text.push_back('\0');
  out->assign(text.begin(), text.end());
  return true;
}

namespace {
// The retail actor-flow costume state (SUPERMENACTORWORLDFLOWFSM_S_60265281) must be exactly the
// script this switcher supersedes; any other base (a mod's own) is refused, never merged.
const char* const kRetailCostumeState[] = {
    "local costumeNum = GetDASValue(\"SetCostumeNum\")",
    "SetDASValue(\"SetCostumeNum\", 0)",
    "local newCostume = GetDASValue(\"Costume\" .. costumeNum)",
    "local supermanID = GetIDFromBinIndex(\"SUPERMAN\", 0)",
    "ReplaceEntityVisRep(supermanID, newCostume);",
};

// Our script (port feature). Same consumer contract as retail - SetCostumeNum names
// Costume<n>, ReplaceEntityVisRep puts its model on - plus: the switch happens only for a suit of
// this session's animation family (its banks are the ones the game loaded at start; a suit of
// another family on them would be stretched or squashed), and a Superman the game respawns
// (boot, load, reopen) is dressed in the worn suit.
const char* const kCostumeSwitchLua = R"LUA(function main(mode)

if (mode == "enter") then

elseif (mode == "process") then

	-- Superman Returns PC port, Fortress costumes (docs/MODS.md "Fortress costumes").
	-- The retail state consumed SetCostumeNum before Superman existed and lost it; wait for him.
	local supermanID = GetIDFromBinIndex("SUPERMAN", 0)
	if (supermanID == -1 or supermanID == 0) then return end

	-- Character switch (docs/MODS.md "Character switch"): a whole character on its own animation
	-- project is worn the way PLAYABLEBIZ swaps in Bizarro - the game's Superman is parked and
	-- hidden, the character entity is spawned where he stands and takes the controller and camera.
	local function srExists(id)
		if (id == nil or id == 0 or id == -1) then return false end
		local e = EntityIDExists(id)
		return (e == true or e == 1)
	end
	local charID = GetDASValue("SRCharID")
	if (charID ~= 0) then
		local supesID = GetDASValue("SRSupesID")
		if ((not srExists(charID)) or (not srExists(supesID)) or GetBinSize("SUPERMAN") > 1) then
			-- the game unloaded or respawned the pair (a load, the reopen): start again from its Superman
			LuaDebugPrint("Character switch: pair gone, starting again from the game's Superman")
			if (srExists(charID) and GetBinSize("SUPERMAN") > 1) then
				ClearFilter("SUPERMAN", charID)
				DespawnEntity(charID)
			end
			SetDASValue("SRCharID", 0)
			SetDASValue("SRSupesID", 0)
			charID = 0
			supermanID = GetIDFromBinIndex("SUPERMAN", 0)
			if (supermanID == -1 or supermanID == 0) then return end
		end
	end
	-- hand the player from the actor out now to another (same place, facing, meters and upgrades)
	local function srHandOver(from, to)
		ApplyPowerups(to)
		SetPowerMagnitude("StaminaPoints", GetPowerMagnitude("StaminaPoints", from), to)
		SetDASValue("SelectedPower", GetDASValue("SelectedPower", from), to)
		-- keep flying: the Superman FSM's DetermineMovement (SupermanFSM_s_7E05F603) starts flight
		-- when CurMoveMode is 1 (a new actor has -1 and asks IsEntityInAir, false on its first tick)
		local moveMode = GetDASValue("CurMoveMode", from)
		if (moveMode == 0 or moveMode == 1) then SetDASValue("CurMoveMode", moveMode, to) end
		if (moveMode == 1) then RestartFSM(to) end
		SetCameraHomeEntity(to)
		SetActorToDefaultController(to)
	end
	local function srPark(id)
		ClearFilter("SUPERMAN", id)
		ClearFilter("CAMERATARGET", id)
		ClearFilter("TRAFFICOBSTACLE", id)
		DisableVisRep(id)
		PauseFSM(id, true)
		SetEntityPosition(id, 0, 0, 0)
	end
	local function srSpawnCharacter(from, entity)
		local px = GetEntityPositionX(from)
		local py = GetEntityPositionY(from)
		local pz = GetEntityPositionZ(from)
		local ox = GetEntityOrientationX(from)
		local oy = GetEntityOrientationY(from)
		local oz = GetEntityOrientationZ(from)
		local supes = GetDASValue("SRSupesID")
		if (from == supes) then srPark(from) else ClearFilter("SUPERMAN", from) end
		local newID = SpawnEntityActive(entity, px, py, pz, ox, oy, oz)
		if (newID == nil or newID == 0 or newID == -1) then newID = GetIDFromBinIndex("SUPERMAN", 0) end
		SetEntityOrientation(newID, ox, oy, oz)
		srHandOver(from, newID)
		if (from ~= supes) then DespawnEntity(from) end
		SetDASValue("SRCharID", newID)
		SetDASValue("SRAppliedTo", newID)
		return newID
	end
	local function srRestoreSuperman(from)
		local supes = GetDASValue("SRSupesID")
		local px = GetEntityPositionX(from)
		local py = GetEntityPositionY(from)
		local pz = GetEntityPositionZ(from)
		local ox = GetEntityOrientationX(from)
		local oy = GetEntityOrientationY(from)
		local oz = GetEntityOrientationZ(from)
		ClearFilter("SUPERMAN", from)
		AddFilter("SUPERMAN", supes)
		AddFilter("CAMERATARGET", supes)
		AddFilter("TRAFFICOBSTACLE", supes)
		EnableVisRep(supes)
		PauseFSM(supes, false)
		SetEntityPosition(supes, px, py, pz)
		SetEntityOrientation(supes, ox, oy, oz)
		SetDASValue("DASPosture", 0, supes)
		ResetActor(supes)
		srHandOver(from, supes)
		DespawnEntity(from)
		SetDASValue("SRCharID", 0)
		SetDASValue("SRSupesID", 0)
		SetDASValue("SRAppliedTo", supes)
		return supes
	end

	-- a retail suit (Costume2 / Costume3) follows the loaded save: it is put on a Superman the game
	-- spawned only when that save has it unlocked (BaseWorldFlow costumeUnlocked bit n-1, the bit
	-- Fortress COSTUMES reads: sub_82206F08). costumeUnlocked is a Bitfield attribute, which
	-- GetWorldFlowDASValue does not return; the game's GetWorldFlowBitfieldValue(flow, attribute,
	-- bit) (sub_82207C00) gives 1 or 0. Read only. Mod suits and characters are never locked.
	local function srSaveHasSuit(n)
		if (n < 2 or n > 3) then return true end
		return GetWorldFlowBitfieldValue("BaseWorldFlow", "costumeUnlocked", n - 1) == 1
	end

	local costumeNum = GetDASValue("SetCostumeNum")
	local worn = GetDASValue("SRWornCostume")
	if (charID == 0 and 0 == costumeNum and (worn == 2 or worn == 3)) then
		-- a worn retail suit, checked every tick: the loaded save's bits arrive after the spawn
		-- and a load from the pause menu keeps this flow (and the last save's bits) for a while.
		-- SRDressedSuit = the retail suit on him now (also set by LoadMetropolis); only a change
		-- of the wanted suit or a new Superman changes the model. SRWornCostume is not changed.
		local want = worn
		if (not srSaveHasSuit(worn)) then want = 1 end
		if ((GetDASValue("SRAppliedTo") ~= supermanID or GetDASValue("SRDressedSuit") ~= want) and
		    GetDASValue("SRCostumeFamily" .. want) == GetDASValue("SRActiveFamily")) then
			local suit = GetDASValue("Costume" .. want)
			if (suit ~= nil) then ReplaceEntityVisRep(supermanID, suit) end
			SetDASValue("SRDressedSuit", want)
			SetDASValue("SRAppliedTo", supermanID)
		end
	elseif (charID == 0 and GetDASValue("SRAppliedTo") ~= supermanID) then
		-- a Superman the game spawned itself (SupermanLayer: SupermanEnt): dress him in the worn suit
		if (worn > 1 and GetDASValue("SRCostumeChar" .. worn) == 1 and
		    GetWorldFlowDASValue("BaseWorldFlow", "gameStarted") ~= 1) then
			-- the front end's attract Superman stays the game's own (a character's FSM only takes
			-- player control once BaseWorldFlow gameStarted is 1); the character comes with play
		else
			SetDASValue("SRAppliedTo", supermanID)
			if (0 == costumeNum and worn > 1) then
				costumeNum = worn
			end
		end
	end

	if( 0 ~= costumeNum ) then

		-- Reset for next use
		SetDASValue("SetCostumeNum", 0)

		-- Costume is Key entry named "Costume#"
		local newCostume = GetDASValue("Costume" .. costumeNum)

		if( nil ~= newCostume and GetDASValue("SRCostumeChar" .. costumeNum) == 1 ) then
			-- a whole character on its own (preloaded) animation project
			LuaDebugPrint("Character switch: " .. newCostume)
			if (charID == 0) then SetDASValue("SRSupesID", supermanID) end
			srSpawnCharacter(supermanID, newCostume)
			SetDASValue("SRWornCostume", costumeNum)
		-- only a suit that moves on this session's animation banks (the others: next start)
		elseif( nil ~= newCostume and GetDASValue("SRCostumeFamily" .. costumeNum) == GetDASValue("SRActiveFamily") ) then
			LuaDebugPrint("Costume switch: " .. newCostume)
			if (charID ~= 0) then supermanID = srRestoreSuperman(charID) end
			ReplaceEntityVisRep(supermanID, newCostume);
			SetDASValue("SRWornCostume", costumeNum)
			SetDASValue("SRDressedSuit", costumeNum)
			SetDASValue("SRAppliedTo", supermanID)
		end
	end

elseif (mode == "exit") then

end

end
)LUA";

// LoadMetropolis keeps its own synchronous model set right after LoadWorldFlow (the retail
// order; two rolls that deferred it to the actor flow's tick froze in this arena -> Metropolis
// load, archive/v1_fortress famA2 + ng_hold2). Only the suit changes: the worn one (always of this session's family), read
// only while the actor flow runs (the guard retail S_7E412353 uses for that flow's DAS).
const char* const kWornRequestLua =
    "local srSuit = \"SupermanEnt\"\n"
    "\tif ( IsWorldFlowRunning(\"SupermenActorWorldFlow\") ) then\n"
    "\t\tlocal srWorn = GetWorldFlowDASValue(\"SupermenActorWorldFlow\", \"SRWornCostume\")\n"
    "\t\tif (srWorn ~= nil and srWorn > 1) then\n"
    "\t\t\tlocal srWornSuit = GetWorldFlowDASValue(\"SupermenActorWorldFlow\", \"Costume\" .. srWorn)\n"
    "\t\t\tif (srWornSuit ~= nil) then srSuit = srWornSuit end\n"
    // a worn whole character (own project) is never a model on Superman: while it is out the call
    // re-dresses the character in its own model; otherwise Superman keeps his until it is spawned
    "\t\t\tif (GetWorldFlowDASValue(\"SupermenActorWorldFlow\", \"SRCostumeChar\" .. srWorn) == 1 and\n"
    "\t\t\t    GetWorldFlowDASValue(\"SupermenActorWorldFlow\", \"SRCharID\") == 0) then srSuit = \"SupermanEnt\" end\n"
    // a retail suit follows the loaded save: worn only when its costumeUnlocked bit (srWorn-1,
    // sub_82206F08) is set in this save; read only, the switcher's srSaveHasSuit test
    "\t\t\tif (srWorn <= 3 and GetWorldFlowBitfieldValue(\"BaseWorldFlow\", \"costumeUnlocked\", srWorn - 1) ~= 1) then\n"
    "\t\t\t\tsrSuit = \"SupermanEnt\"\n"
    "\t\t\tend\n"
    "\t\tend\n"
    // the suit put on, for the switcher's per-tick check (1 = the default SupermanEnt)
    "\t\tlocal srDressed = 1\n"
    "\t\tif (srWorn ~= nil and srWorn > 1 and srSuit ~= \"SupermanEnt\") then srDressed = srWorn end\n"
    "\t\tSetWorldFlowDASValue(\"SupermenActorWorldFlow\", \"SRDressedSuit\", srDressed)\n"
    "\tend\n"
    "\tReplaceEntityVisRep(supermanID, srSuit);";
}  // namespace

// The switcher's six helpers (srExists, srHandOver, srPark, srSpawnCharacter, srRestoreSuperman, srSaveHasSuit)
// are written inside main above for review in reading order, but the composed chunk declares them at CHUNK level:
// built once when the chunk runs (AptScriptLoadAndExecute 82191AE8 -> FmScript_RunString 82176EB8), then captured by
// main as upvalues (none of them reads a main local). Declared inside main they were six closures and two upvalues
// on EVERY 'process' tick (30 Hz, ~5.9 KB/s); the game collects its script heap only when it loads a script
// (docs/ENGINE_MAP.md 6c), so with port features on the heap reached the Gameplay::FSM allocator's limit ~3.3 min
// into Metropolis and every Lua allocation failed (LUA_ERRMEM; step4 lane B 2026-10-20, docs/LOGIC_AUDIT.md F4).
// Retail's state allocates nothing per tick. SR_COSTUME_SWITCH_LEGACY=1 composes the old text (the A/B).
// AptScriptLoadAndExecute renames the FIRST "main(" of the text to the chunk's unique name, so the prelude and
// the helpers must not contain that substring (checked here).
bool modCostumeSwitchText(std::string* out, std::string* err) {
  const std::string legacy(kCostumeSwitchLua);
  const char* gate = std::getenv("SR_COSTUME_SWITCH_LEGACY");
  if (gate && gate[0] == '1') { *out = legacy; return true; }
  auto once = [&](const char* marker) {
    const size_t at = legacy.find(marker);
    return at != std::string::npos && legacy.find(marker, at + 1) == std::string::npos ? at : std::string::npos;
  };
  const size_t a = once("\t-- Character switch (docs/MODS.md");             // + srExists
  const size_t b = once("\tlocal charID = GetDASValue(\"SRCharID\")");
  const size_t c = once("\t-- hand the player from the actor out now");     // srHandOver .. srSaveHasSuit
  const size_t d = once("\tlocal costumeNum = GetDASValue(\"SetCostumeNum\")");
  if (a == std::string::npos || b == std::string::npos || c == std::string::npos || d == std::string::npos ||
      !(a < b && b < c && c < d)) {
    *err = "costume switch helpers not found";
    return false;
  }
  const std::string prelude =
      "-- Superman Returns PC port: the costume switcher's helpers, built once when this chunk loads (the game\n"
      "-- never collects its script heap between script loads, so the per-tick state below allocates nothing).\n";
  const std::string helpers = legacy.substr(a, b - a) + legacy.substr(c, d - c);
  if ((prelude + helpers).find("main(") != std::string::npos) { *err = "costume switch helpers name main("; return false; }
  *out = prelude + helpers + legacy.substr(0, a) + legacy.substr(b, c - b) + legacy.substr(d);
  return true;
}

bool modComposeCostumeSwitchLua(const std::vector<uint8_t>& base, const std::string& entry,
                                std::vector<uint8_t>* out, std::string* err) {
  auto bad = [&](const char* why) { *err = why; return false; };
  if (base.empty() || base.size() > 1024 * 1024) return bad("invalid costume Lua input");
  std::string text(base.begin(), base.end());
  if (text.back() == '\0') text.pop_back();
  if (text.find('\0') != std::string::npos || text.rfind("function main(mode)", 0) != 0)
    return bad("costume switch needs retail Lua source");
  auto once = [&](const std::string& marker) {
    const size_t at = text.find(marker);
    return at != std::string::npos && text.find(marker, at + marker.size()) == std::string::npos ? at : std::string::npos;
  };
  if (entry == "SUPERMENACTORWORLDFLOWFSM_S_60265281") {
    for (const char* stmt : kRetailCostumeState)
      if (once(stmt) == std::string::npos) return bad("costume actor Lua statement missing or ambiguous");
    std::string ours;
    if (!modCostumeSwitchText(&ours, err)) return false;
    out->assign(ours.begin(), ours.end());
    return true;
  }
  std::string marker, replacement;
  if (entry == "BASEWORLDFLOWFSM_S_7E412353") {
    // retail: every unload puts Superman back in Costume1. The respawned actor is dressed in the
    // worn suit by the switcher instead (no request to an actor that is about to be unloaded).
    marker = "SetWorldFlowDASValue(\"SupermenActorWorldFlow\", \"SetCostumeNum\", 1)";
    replacement = "-- PC port: the actor flow re-dresses the respawned Superman in the worn suit";
  } else if (entry == "BASEWORLDFLOWFSM_S_4982930A") {
    // retail LoadMetropolis: model cleared for the arena load, then SupermanEnt's model; the
    // same synchronous call puts the worn suit back on instead (kWornRequestLua).
    marker = "ReplaceEntityVisRep(supermanID, \"SupermanEnt\");";
    replacement = kWornRequestLua;
  } else return bad("unsupported costume Lua entry");
  const size_t at = once(marker);
  if (at == std::string::npos) return bad("costume Lua statement missing or ambiguous");
  text.replace(at, marker.size(), replacement);
  out->assign(text.begin(), text.end());
  return true;
}

std::string modCostumeFamily(const ModCostumeInfo& c) {
  if (!c.bankFamily.empty()) return c.bankFamily;
  if (!c.animationProject.empty()) return c.animationProject;
  return kRetailCostumeFamily;
}

bool modValidSupermanBank(const std::vector<uint8_t>& b, std::string* err) {
  // The retail SUPERMAN_* banks: +00 fixup-list offset, +04 kind 9, +08 bank hash,
  // +0C skeleton hash 0xE3D7527A (FNV-1 "Superman"); docs/ENGINE_MAP.md "Fortress COSTUMES".
  if (b.size() < 16 || b.size() > 4 * 1024 * 1024) { *err = "bank size out of range"; return false; }
  if (big(b.data() + 4, 4) != 9 || big(b.data() + 12, 4) != 0xE3D7527Au || big(b.data(), 4) >= b.size()) {
    *err = "not a Superman-skeleton animation bank (kind 9, skeleton 0xE3D7527A)";
    return false;
  }
  return true;
}

namespace {
struct BgfaRecord { uint64_t off = 0, size = 0; size_t sizeField = 0, sizeWidth = 0; bool compressed = false; };
// One entry of the BGFA1.05 container b[0, n) (top-level file or nested entry: same header).
bool bgfaFind(const uint8_t* b, size_t n, const std::string& entry, BgfaRecord* out, std::string* err) {
  auto bad = [&](const std::string& why) { *err = why; return false; };
  if (n < 64 || std::memcmp(b, "BGFA1.05", 8)) return bad(entry + ": container is not BGFA1.05");
  const uint64_t count = little(b + 12, 4), toc = little(b + 16, 4);
  const uint64_t tocLen = little(b + 24, 4), tags = little(b + 40, 4), names = little(b + 44, 4);
  const size_t wa = b[33], wi = b[34], wo = b[35], ws = b[36], wu = b[37], shift = b[38];
  if (wa > 8 || wi > 8 || wo > 8 || ws > 8 || wu > 8 || !wi || !wo || !ws || shift > 31 || names > 4096)
    return bad(entry + ": invalid BGFA widths");
  const uint64_t record = 1 + wa + wi + wo + ws + wu + names;
  if (toc > n || tocLen > n - toc || 4 * tags > tocLen || count > (tocLen - 4 * tags) / record)
    return bad(entry + ": BGFA TOC out of range");
  const uint64_t key = fnv1a64Upper(entry);
  const uint64_t mask = wi == 8 ? ~uint64_t(0) : ((uint64_t(1) << (wi * 8)) - 1);
  for (uint64_t i = 0; i < count; ++i) {
    const size_t at = static_cast<size_t>(toc + tags * 4 + i * record) + 1 + wa;
    if (little(b + at, wi) != (key & mask)) continue;
    const uint64_t units = little(b + at + wi, wo);
    if (units > (uint64_t(-1) >> shift)) return bad(entry + ": BGFA offset overflow");
    out->off = units << shift;
    out->sizeField = at + wi + wo;
    out->sizeWidth = ws;
    out->size = little(b + out->sizeField, ws);
    out->compressed = wu && little(b + out->sizeField + ws, wu) != 0;
    if (out->off > n || out->size > n - out->off) return bad(entry + ": BGFA payload out of range");
    return true;
  }
  return bad(entry + ": entry missing");
}
}  // namespace

bool modPatchBaseworldBanks(std::vector<uint8_t>* file,
                            const std::vector<std::pair<std::string, std::vector<uint8_t>>>& banks,
                            std::string* err) {
  // In place, the way the costume lane's patch_ast_banks.py does it (measured: banks delivered any
  // other way overflow the animation budget): each own bank goes into its retail slot inside
  // BASEWORLDFLOW_ANIM, which must be large enough; the TOC size field is updated and the rest of
  // the slot is zero-filled, so no offset moves. Everything is validated before a byte changes.
  BgfaRecord anim;
  if (!file || !bgfaFind(file->data(), file->size(), "BASEWORLDFLOW_ANIM", &anim, err)) return false;
  if (anim.compressed) { *err = "BASEWORLDFLOW_ANIM is compressed"; return false; }
  uint8_t* sub = file->data() + anim.off;
  const size_t subLen = static_cast<size_t>(anim.size);
  std::vector<BgfaRecord> slots;
  std::set<std::string> seen;
  for (const auto& [name, bytes] : banks) {
    const std::string up = [&] { std::string u = name; for (auto& ch : u) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); return u; }();
    bool known = false;
    for (const char* retail : kSupermanBanks) known = known || up == retail;
    if (!known || !seen.insert(up).second) { *err = name + " is not a SupermanAnims bank (or is listed twice)"; return false; }
    if (!modValidSupermanBank(bytes, err)) { *err = name + ": " + *err; return false; }
    BgfaRecord slot;
    if (!bgfaFind(sub, subLen, up, &slot, err)) return false;
    std::vector<uint8_t> retail(sub + slot.off, sub + slot.off + slot.size);
    if (slot.compressed || !modValidSupermanBank(retail, err)) { *err = up + ": the retail slot is not a plain Superman bank"; return false; }
    if (bytes.size() > slot.size) {
      *err = up + ": own bank " + std::to_string(bytes.size()) + " bytes > retail slot " + std::to_string(slot.size);
      return false;
    }
    slots.push_back(slot);
  }
  for (size_t i = 0; i < banks.size(); ++i) {
    const auto& bytes = banks[i].second;
    const BgfaRecord& slot = slots[i];
    std::memcpy(sub + slot.off, bytes.data(), bytes.size());
    std::memset(sub + slot.off + bytes.size(), 0, static_cast<size_t>(slot.size - bytes.size()));
    for (size_t k = 0; k < slot.sizeWidth; ++k) sub[slot.sizeField + k] = static_cast<uint8_t>((bytes.size() >> (8 * k)) & 0xFF);
  }
  return true;
}
}
