#pragma once
// iter-464 modcore: small self-contained formats the mod system reads (docs/MODS.md):
// JSON (mod.json, mods.json, feature.json), SHA-256, FNV-1a-64, the RefPack stream header,
// and the SRPATCH1 delta format (tools/mods/srpatch.py is the reference; same rules).
// Our own code, no third-party source. Kernel-only header (never include from guest/).
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sr {
namespace kernel {

// ---------------------------------------------------------------- JSON
// RFC 8259 subset: objects, arrays, strings (\uXXXX incl. surrogate pairs -> UTF-8),
// numbers (kept as double + an exact int64 when integral), true/false/null. A UTF-8 BOM is
// skipped. Object key order is kept.
struct JsonValue {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool b = false;
  double num = 0;
  int64_t i64 = 0;
  bool isInt = false;
  std::string str;
  std::vector<JsonValue> arr;
  std::vector<std::pair<std::string, JsonValue>> obj;

  bool isObject() const { return type == Type::Object; }
  bool isArray() const { return type == Type::Array; }
  bool isString() const { return type == Type::String; }
  bool isInteger() const { return type == Type::Number && isInt; }
  const JsonValue* get(const std::string& key) const;  // object member or nullptr
};
// False with *err ("line L col C: ...") on a syntax error.
bool jsonParse(const std::string& text, JsonValue* out, std::string* err);
// A JSON string literal (quotes + escapes) for writers.
std::string jsonQuote(const std::string& s);

// ---------------------------------------------------------------- hashes
struct Sha256 {
  Sha256();
  void update(const void* data, size_t n);
  void final(uint8_t out[32]);
 private:
  uint32_t h_[8];
  uint64_t len_ = 0;
  uint8_t buf_[64];
  size_t blen_ = 0;
  void block(const uint8_t* p);
};
std::string sha256Hex(const void* data, size_t n);
std::string hexOf(const uint8_t* p, size_t n);
uint64_t fnv1a64Upper(const std::string& name);

// RefPack (EA "QFS") stream header -> the decompressed size; false if `head` is not one.
bool refpackDecompressedSize(const uint8_t* head, size_t n, uint64_t* size);

// ---------------------------------------------------------------- SRPATCH1
struct SrPatch {
  uint64_t baseSize = 0, outSize = 0;
  uint8_t baseSha[32] = {}, outSha[32] = {};
  bool usesFile = false;  // has COPYF ops (copies from the base's disc file)
  struct Op { uint8_t kind; uint64_t off; uint64_t len; size_t addAt; };  // addAt: ADD bytes in the patch
  std::vector<Op> ops;
};
enum : uint8_t { kSrpEnd = 0, kSrpCopy = 1, kSrpAdd = 2, kSrpFill = 3, kSrpCopyF = 4 };
bool srpatchParse(const std::vector<uint8_t>& patch, SrPatch* out, std::string* err);
// Reads `n` bytes of the base's disc file at `off` into `dst`; false if out of range.
using SrFileReader = std::function<bool(uint64_t off, uint64_t n, uint8_t* dst)>;
// Verifies the base (size + SHA-256), applies, verifies the output. `file` may be empty
// when the patch has no COPYF.
bool srpatchApply(const std::vector<uint8_t>& patch, const SrPatch& parsed, const std::vector<uint8_t>& base,
                  const SrFileReader& file, std::vector<uint8_t>* out, std::string* err);
// True when `base` is the patch's base (size + SHA-256).
bool srpatchBaseMatches(const SrPatch& parsed, const std::vector<uint8_t>& base);

}  // namespace kernel
}  // namespace sr
