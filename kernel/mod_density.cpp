// iter-466 density (port feature): see mod_density.hpp and docs/MODS.md "Population and traffic density".
#include "mod_density.hpp"

#include "mod_costume_data.hpp"
#include "xam_content.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

namespace sr::kernel {
namespace fs = std::filesystem;

namespace {
std::string trimDensity(const std::string& s) {
  const char* ws = " \t\r\n";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(ws) - b + 1);
}
bool parsePct(const std::string& s, int* out) {
  if (s.empty() || s.size() > 4) return false;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (!end || *end != 0 || !modDensityValid(static_cast<int>(v))) return false;
  *out = static_cast<int>(v);
  return true;
}
fs::path optionsPath() { return xamContentRoot() / "settings" / "game_options.ini"; }
}  // namespace

bool modDensityValid(int pct) {
  return pct >= kDensityMin && pct <= kDensityMax && pct % kDensityStep == 0;
}

int modDensityScale(int retailCount, int pct) { return (retailCount * pct + 50) / 100; }

ModDensity modDensityLoad() {
  ModDensity d;
  std::ifstream f(optionsPath());
  std::string line;
  while (f && std::getline(f, line)) {
    line = trimDensity(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = trimDensity(line.substr(0, eq)), v = trimDensity(line.substr(eq + 1));
    int pct = 0;
    if (k == kTrafficDensityKey && parsePct(v, &pct)) d.traffic = pct;
    if (k == kPopulationDensityKey && parsePct(v, &pct)) d.population = pct;
  }
  return d;
}

bool modDensitySave(const ModDensity& d) {
  if (!modDensityValid(d.traffic) || !modDensityValid(d.population)) return false;
  const fs::path path = optionsPath();
  std::string text;
  {
    std::ifstream in(path, std::ios::binary);
    if (in) text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  // Keep every line except our two keys, then write them at the end.
  std::string out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    const size_t end = nl == std::string::npos ? text.size() : nl + 1;
    const std::string raw = text.substr(pos, end - pos);
    pos = end;
    const std::string line = trimDensity(raw);
    const size_t eq = line.find('=');
    const std::string k = eq == std::string::npos ? std::string() : trimDensity(line.substr(0, eq));
    if (k == kTrafficDensityKey || k == kPopulationDensityKey) continue;
    out += raw;
    if (out.back() != '\n') out += '\n';
  }
  out += std::string(kTrafficDensityKey) + "=" + std::to_string(d.traffic) + "\n";
  out += std::string(kPopulationDensityKey) + "=" + std::to_string(d.population) + "\n";
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path tmp = path.parent_path() / "game_options.ini.density.tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
    f.flush();
    if (!f) return false;
  }
  fs::rename(tmp, path, ec);
  if (ec) fs::remove(tmp, ec);
  return !ec;
}

bool modComposeTrafficDensity(const std::vector<uint8_t>& compiled, const ModDensity& d,
                              std::vector<uint8_t>* text, std::string* summary, std::string* err) {
  if (!modDensityValid(d.traffic) || !modDensityValid(d.population)) {
    *err = "density out of range (25.." + std::to_string(kDensityMax) + ", steps of 25)";
    return false;
  }
  std::map<std::string, int> seen;  // set type -> times seen
  bool first = true, rootOk = false;
  std::string sum;
  const ModXmlAttrEdit edit = [&](const std::string& element, std::vector<std::pair<std::string, std::string>>* attrs,
                                  std::string* e) {
    if (first) {
      first = false;
      rootOk = element == "trafficDef";
      if (!rootOk) { *e = "root is <" + element + ">, not <trafficDef>"; return false; }
      return true;
    }
    if (element != "trafficSet") return true;
    std::string type;
    for (const auto& [k, v] : *attrs) if (k == "type") type = v;
    const int pct = (type == "Car" || type == "ParkedCar") ? d.traffic : type == "Pedestrian" ? d.population : 0;
    if (!pct) return true;  // event sets (MEFireTruck, MERescueVehicles) stay as authored
    if (++seen[type] > 1) { *e = "trafficSet " + type + " appears twice"; return false; }
    bool counted = false;
    for (auto& [k, v] : *attrs) {
      if (k != "count") continue;
      char* end = nullptr;
      const long n = std::strtol(v.c_str(), &end, 10);
      if (v.empty() || v.size() > 6 || !end || *end || n < 0) { *e = "trafficSet " + type + " count is not an integer"; return false; }
      const int scaled = modDensityScale(static_cast<int>(n), pct);
      sum += (sum.empty() ? "" : ", ") + type + " " + v + "->" + std::to_string(scaled);
      v = std::to_string(scaled);
      counted = true;
    }
    if (!counted) { *e = "trafficSet " + type + " has no count"; return false; }
    return true;
  };
  std::vector<uint8_t> out;
  if (!modCompiledXmlToTextEdited(compiled, edit, &out, err)) return false;
  for (const char* t : {"Car", "ParkedCar", "Pedestrian"})
    if (seen[t] != 1) { *err = std::string("trafficSet ") + t + " missing"; return false; }
  text->swap(out);
  if (summary) *summary = sum;
  return true;
}

}  // namespace sr::kernel
