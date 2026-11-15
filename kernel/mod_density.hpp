#pragma once
// iter-466 density (port feature, docs/MODS.md "Population and traffic density").
//
// The game's ambient traffic manager (MetropolisTraffic, dword_8296E110) takes its pool sizes from
// one document: WORLDDATA TrafficXML = "MetropolisTraffic" (metropol.AST ->
// METROPOLISWORLDFLOW_OTHER -> METROPOLISTRAFFIC, compiled XML). Retail: trafficSet Car count 60,
// ParkedCar 40, Pedestrian 60. The setting gives the game a different copy of that document
// (counts scaled) as a loose DATA/_workinprogress file; the game reads it, sizes its own pools and
// makes every spawn decision itself. Nothing here touches guest state or calls guest code.
//
// Store: <save root>/settings/game_options.ini, keys traffic_density and population_density
// (integer percent, 25..kDensityMax in steps of 25, default 100). traffic_density scales driving and
// parked cars, population_density pedestrians. At 100/100 nothing is mounted (the retail
// document, byte-identical). The document is chosen at boot: a change applies after a restart.
// Rolls: SR_TRAFFIC_DENSITY / SR_POPULATION_DENSITY override the file (port features must be on).
#include "pc_settings.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sr::kernel {

// The maximum is the PC SETTINGS slider's (pcsettings::kDensityMax): lowered there if the stability
// soak caps the setting, it caps the stored value and the served document too.
constexpr int kDensityMin = 25, kDensityMax = pcsettings::kDensityMax, kDensityStep = 25, kDensityDefault = 100;
constexpr const char* kTrafficDensityKey = "traffic_density";
constexpr const char* kPopulationDensityKey = "population_density";

struct ModDensity {
  int traffic = kDensityDefault;     // percent: driving + parked cars
  int population = kDensityDefault;  // percent: pedestrians
  bool operator==(const ModDensity& o) const { return traffic == o.traffic && population == o.population; }
  bool operator!=(const ModDensity& o) const { return !(*this == o); }
};

bool modDensityValid(int pct);                  // 25..kDensityMax, a multiple of 25
int modDensityScale(int retailCount, int pct);  // round half up

// game_options.ini: missing or invalid keys read as 100 (each key independently).
ModDensity modDensityLoad();
// Writes the two keys and keeps every other line of game_options.ini (other options, comments).
bool modDensitySave(const ModDensity& d);
// What this boot applied (100/100 when the feature is off or failed). A UI shows "restart to
// apply" while modDensityLoad() differs from this.
ModDensity modDensityApplied();

// The retail compiled METROPOLISTRAFFIC -> the text document with the Car / ParkedCar counts scaled
// by traffic% and the Pedestrian count by population%. Every other element and attribute is
// unchanged. Fails closed (false + *err) unless exactly one Car, ParkedCar and Pedestrian
// trafficSet with an integer count exists under a trafficDef root. *summary: "Car 60->120, ...".
bool modComposeTrafficDensity(const std::vector<uint8_t>& compiled, const ModDensity& d,
                              std::vector<uint8_t>* text, std::string* summary, std::string* err);

}  // namespace sr::kernel
