#pragma once
// Content composition only: no guest state or guest calls. Enabled character
// packs share one generated attribute document; retail entries are preserved.
#include "mod_manager.hpp"

#include <functional>
#include <set>
#include <utility>

namespace sr::kernel {
bool modReadNestedEntry(const std::vector<uint8_t>& container, const std::string& entry,
                        std::vector<uint8_t>* out, std::string* err);
bool modComposeCostumeAttributes(const std::vector<uint8_t>& base,
                                 const std::vector<ModCostumeInfo>& additions,
                                 std::vector<uint8_t>* out, std::string* err, int startupSlot = 0,
                                 const std::string& activeFamily = "retail");
// The actor flow's costume-switch state as composed (helpers at chunk level; SR_COSTUME_SWITCH_LEGACY=1: the
// old text with them inside main). False + *err if the text's markers are missing.
bool modCostumeSwitchText(std::string* out, std::string* err);
// iter-465 Fortress costumes: the actor flow's costume state becomes the family-aware switcher
// (retail ReplaceEntityVisRep for a suit of this session's animation family, nothing for another
// family: it is worn from the next start); BASEWORLDFLOWFSM_S_7E412353 / _S_4982930A keep the
// worn suit on.
bool modComposeCostumeSwitchLua(const std::vector<uint8_t>& base, const std::string& entry,
                                std::vector<uint8_t>* out, std::string* err);
// Animation families (docs/MODS.md "Fortress costumes"): "retail" = the disc's SupermanAnims banks;
// a suit with "animation_banks" = "banks_<16 hex of its bank set>"; an own project = its name.
extern const char* const kRetailCostumeFamily;   // "retail"
extern const char* const kSupermanBanks[6];      // the SupermanAnims banks in BASEWORLDFLOW_ANIM
std::string modCostumeFamily(const ModCostumeInfo& c);
bool modValidSupermanBank(const std::vector<uint8_t>& bank, std::string* err);
// A whole baseworl.AST image with own banks written into their retail slots (in place).
bool modPatchBaseworldBanks(std::vector<uint8_t>* file,
                            const std::vector<std::pair<std::string, std::vector<uint8_t>>>& banks,
                            std::string* err);
// Preserve every retail preload group/attribute, append enabled own projects once.
// A compiled (binary) game XML document -> the equivalent text document (NUL terminated), for
// serving a retail resource as a loose file. Generic: elements, attributes, nesting.
// A Superman-skeleton bank (kind 9) with a new identity for a scoped project: header +08 =
// FNV-1(bankName), every clip block's +00 frame-cache hash = FNV-1("<ns>_<old hash>") (made unique
// against *used). Bone data, clip names, events and timing are unchanged.
bool modScopedBankCopy(const std::vector<uint8_t>& bank, const std::string& bankName, const std::string& ns,
                       std::set<uint32_t>* used, std::vector<uint8_t>* out, std::string* err);
bool modCompiledXmlToText(const std::vector<uint8_t>& compiled, std::vector<uint8_t>* out, std::string* err);
// The same conversion with each element's attributes passed through `edit` (element name, its
// attributes in document order) before they are written; `edit` returns false (with *err) to refuse
// the document. modCompiledXmlToText = this with no edit.
using ModXmlAttrEdit = std::function<bool(const std::string& element,
                                          std::vector<std::pair<std::string, std::string>>* attrs, std::string* err)>;
bool modCompiledXmlToTextEdited(const std::vector<uint8_t>& compiled, const ModXmlAttrEdit& edit,
                                std::vector<uint8_t>* out, std::string* err);
bool modComposeCostumePreloads(const std::vector<uint8_t>& base,
                              const std::vector<ModCostumeInfo>& additions,
                              std::vector<uint8_t>* out, std::string* err);
}
