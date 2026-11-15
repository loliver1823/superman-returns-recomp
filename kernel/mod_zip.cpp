// iter-464 modcore: zip mods (mod_zip.hpp). Our own code: a streaming RFC 1951 inflater with
// a 10-bit lookup table per Huffman code, and a zip central-directory reader (PKWARE
// APPNOTE 4.3.x, the fields every zip writer emits for stored/deflate members).
#include "mod_zip.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>

namespace sr {
namespace kernel {

namespace fs = std::filesystem;

namespace {
std::string zipPathU8(const fs::path& p) {
  const auto u = p.generic_u8string();
  return std::string(u.begin(), u.end());
}
}  // namespace

// ================================================================ CRC-32 (IEEE, reflected)
uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n) {
  static uint32_t table[256];
  static bool init = [] {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    return true;
  }();
  (void)init;
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// ================================================================ inflate
namespace {

constexpr int kFast = 10;

struct Huff {
  uint16_t count[16] = {};
  uint16_t symbol[320] = {};
  uint16_t fast[1 << kFast] = {};  // (len << 9) | sym, 0 = slow path
  bool build(const uint8_t* lengths, int n) {
    std::memset(count, 0, sizeof count);
    std::memset(fast, 0, sizeof fast);
    for (int s = 0; s < n; ++s) count[lengths[s]]++;
    count[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
      left <<= 1;
      left -= count[len];
      if (left < 0) return false;  // over-subscribed
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + count[len]);
    for (int s = 0; s < n; ++s)
      if (lengths[s]) symbol[offs[lengths[s]]++] = static_cast<uint16_t>(s);
    // fast table: canonical codes, bit-reversed (DEFLATE packs codes MSB-first in an LSB stream)
    uint32_t code = 0;
    int idx = 0;
    for (int len = 1; len <= kFast; ++len) {
      for (int k = 0; k < count[len]; ++k, ++idx) {
        uint32_t rev = 0;
        for (int b = 0; b < len; ++b) rev |= ((code >> b) & 1u) << (len - 1 - b);
        for (uint32_t fill = rev; fill < (1u << kFast); fill += (1u << len))
          fast[fill] = static_cast<uint16_t>((len << 9) | symbol[idx]);
        ++code;
      }
      code <<= 1;
    }
    return true;
  }
};

struct Inflater {
  std::function<size_t(uint8_t*, size_t)> pull;   // input source
  std::function<bool(const uint8_t*, size_t)> sink;  // output sink
  std::vector<uint8_t> in;
  size_t inPos = 0, inLen = 0;
  bool inEof = false;
  uint64_t bitbuf = 0;
  int bitcnt = 0;
  std::vector<uint8_t> out;  // window + pending output
  uint64_t total = 0;
  std::string err;

  bool fail(const char* m) { if (err.empty()) err = m; return false; }

  int byteIn() {
    if (inPos == inLen) {
      if (inEof) return -1;
      if (in.empty()) in.resize(1 << 20);
      inLen = pull(in.data(), in.size());
      inPos = 0;
      if (inLen == 0) { inEof = true; return -1; }
    }
    return in[inPos++];
  }
  // Ensure >= n bits buffered when possible; returns false only when the stream ended AND
  // fewer than n bits remain.
  bool need(int n) {
    while (bitcnt < n) {
      const int c = byteIn();
      if (c < 0) return false;
      bitbuf |= uint64_t(c) << bitcnt;
      bitcnt += 8;
    }
    return true;
  }
  bool bits(int n, uint32_t* v) {
    if (n == 0) { *v = 0; return true; }
    if (!need(n)) return fail("deflate stream ended early");
    *v = static_cast<uint32_t>(bitbuf & ((1ull << n) - 1));
    bitbuf >>= n;
    bitcnt -= n;
    return true;
  }
  bool decode(const Huff& h, int* sym) {
    need(kFast);  // may come up short at the very end; the table lookup checks the length
    const uint16_t e = h.fast[bitbuf & ((1u << kFast) - 1)];
    if (e && (e >> 9) <= bitcnt) {
      const int len = e >> 9;
      bitbuf >>= len;
      bitcnt -= len;
      *sym = e & 0x1FF;
      return true;
    }
    // slow path (codes longer than kFast bits): canonical decode, one bit at a time
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      uint32_t b;
      if (!bits(1, &b)) return false;
      code |= static_cast<int>(b);
      const int cnt = h.count[len];
      if (code - cnt < first) { *sym = h.symbol[index + (code - first)]; return true; }
      index += cnt;
      first += cnt;
      first <<= 1;
      code <<= 1;
    }
    return fail("bad Huffman code");
  }
  bool emit(uint8_t b) {
    out.push_back(b);
    return out.size() < (8u << 20) || flush(false);
  }
  bool flush(bool all) {
    const size_t keep = all ? 0 : 32768;
    if (out.size() <= keep) return true;
    const size_t n = out.size() - keep;
    if (!sink(out.data(), n)) return fail("write failed");
    total += n;
    out.erase(out.begin(), out.begin() + static_cast<ptrdiff_t>(n));
    return true;
  }
  bool copy(size_t dist, size_t len) {
    if (dist == 0 || dist > out.size()) return fail("distance too far back");
    for (size_t i = 0; i < len; ++i)
      if (!emit(out[out.size() - dist])) return false;
    return true;
  }

  bool codes(const Huff& lit, const Huff& dist) {
    static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                     3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                       6145, 8193, 12289, 16385, 24577};
    static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    while (true) {
      int sym;
      if (!decode(lit, &sym)) return false;
      if (sym < 256) {
        if (!emit(static_cast<uint8_t>(sym))) return false;
      } else if (sym == 256) {
        return true;
      } else {
        sym -= 257;
        if (sym >= 29) return fail("bad length symbol");
        uint32_t e;
        if (!bits(lext[sym], &e)) return false;
        const size_t len = lbase[sym] + e;
        int ds;
        if (!decode(dist, &ds)) return false;
        if (ds >= 30) return fail("bad distance symbol");
        if (!bits(dext[ds], &e)) return false;
        if (!copy(dbase[ds] + e, len)) return false;
      }
    }
  }

  bool run() {
    while (true) {
      uint32_t last, type;
      if (!bits(1, &last) || !bits(2, &type)) return false;
      if (type == 0) {
        bitbuf >>= (bitcnt & 7);
        bitcnt -= (bitcnt & 7);
        uint32_t len, nlen;
        if (!bits(16, &len) || !bits(16, &nlen)) return false;
        if ((len ^ 0xFFFF) != nlen) return fail("stored block length mismatch");
        for (uint32_t i = 0; i < len; ++i) {
          uint32_t b;
          if (!bits(8, &b) || !emit(static_cast<uint8_t>(b))) return false;
        }
      } else if (type == 1) {
        static Huff flit, fdist;
        static bool built = [] {
          uint8_t l[288];
          int s = 0;
          for (; s < 144; ++s) l[s] = 8;
          for (; s < 256; ++s) l[s] = 9;
          for (; s < 280; ++s) l[s] = 7;
          for (; s < 288; ++s) l[s] = 8;
          flit.build(l, 288);
          uint8_t d[30];
          for (int k = 0; k < 30; ++k) d[k] = 5;
          fdist.build(d, 30);
          return true;
        }();
        (void)built;
        if (!codes(flit, fdist)) return false;
      } else if (type == 2) {
        uint32_t hlit, hdist, hclen;
        if (!bits(5, &hlit) || !bits(5, &hdist) || !bits(4, &hclen)) return false;
        hlit += 257; hdist += 1; hclen += 4;
        if (hlit > 286 || hdist > 30) return fail("bad dynamic block counts");
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        uint8_t lens[320] = {};
        for (uint32_t i = 0; i < hclen; ++i) {
          uint32_t v;
          if (!bits(3, &v)) return false;
          lens[order[i]] = static_cast<uint8_t>(v);
        }
        Huff lencode;
        if (!lencode.build(lens, 19)) return fail("bad code-length code");
        uint8_t all[320] = {};
        uint32_t idx = 0;
        while (idx < hlit + hdist) {
          int sym;
          if (!decode(lencode, &sym)) return false;
          if (sym < 16) { all[idx++] = static_cast<uint8_t>(sym); continue; }
          uint32_t rep = 0, v = 0;
          uint8_t val = 0;
          if (sym == 16) {
            if (idx == 0) return fail("repeat with no previous length");
            val = all[idx - 1];
            if (!bits(2, &v)) return false;
            rep = 3 + v;
          } else if (sym == 17) {
            if (!bits(3, &v)) return false;
            rep = 3 + v;
          } else {
            if (!bits(7, &v)) return false;
            rep = 11 + v;
          }
          if (idx + rep > hlit + hdist) return fail("too many code lengths");
          while (rep--) all[idx++] = val;
        }
        if (all[256] == 0) return fail("no end-of-block code");
        Huff lit, dist;
        if (!lit.build(all, static_cast<int>(hlit)) || !dist.build(all + hlit, static_cast<int>(hdist)))
          return fail("bad literal/distance code");
        if (!codes(lit, dist)) return false;
      } else {
        return fail("bad block type");
      }
      if (last) return flush(true);
    }
  }
};

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

bool safeZipName(const std::string& n) {
  if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find(':') != std::string::npos) return false;
  size_t st = 0;
  while (st <= n.size()) {
    size_t e = n.find_first_of("/\\", st);
    if (e == std::string::npos) e = n.size();
    const std::string c = n.substr(st, e - st);
    if (c == "." || c == "..") return false;
    st = e + 1;
  }
  return true;
}

}  // namespace

bool inflateBuffer(const uint8_t* in, size_t n, std::vector<uint8_t>* out, std::string* err) {
  Inflater f;
  bool given = false;
  f.pull = [&](uint8_t* dst, size_t cap) -> size_t {
    if (given) return 0;
    given = true;
    (void)cap;
    f.in.assign(in, in + n);  // whole buffer at once
    std::memcpy(dst, in, 0);
    return 0;
  };
  // feed the whole buffer directly
  f.in.assign(in, in + n);
  f.inLen = n;
  f.inEof = true;
  out->clear();
  f.sink = [&](const uint8_t* p, size_t k) { out->insert(out->end(), p, p + k); return true; };
  if (!f.run()) {
    if (err) *err = f.err;
    return false;
  }
  return true;
}

bool zipList(const std::string& zipPath, std::vector<ZipMember>* out, std::string* err) {
  auto bad = [&](const std::string& m) { if (err) *err = m; return false; };
  std::FILE* fp = nullptr;
#if defined(_WIN32)
  if (_wfopen_s(&fp, fs::u8path(zipPath).wstring().c_str(), L"rb") != 0) fp = nullptr;
#else
  fp = std::fopen(zipPath.c_str(), "rb");
#endif
  if (!fp) return bad("cannot open the zip");
  struct Closer { std::FILE* f; ~Closer() { std::fclose(f); } } closer{fp};
  _fseeki64(fp, 0, SEEK_END);
  const int64_t size = _ftelli64(fp);
  if (size < 22) return bad("not a zip (too small)");
  const int64_t tail = size < 65557 ? size : 65557;
  std::vector<uint8_t> t(static_cast<size_t>(tail));
  _fseeki64(fp, size - tail, SEEK_SET);
  if (std::fread(t.data(), 1, t.size(), fp) != t.size()) return bad("read failed");
  int64_t eocd = -1;
  for (int64_t i = tail - 22; i >= 0; --i)
    if (rd32(&t[static_cast<size_t>(i)]) == 0x06054B50u) { eocd = i; break; }
  if (eocd < 0) return bad("not a zip (no end-of-central-directory record)");
  const uint8_t* e = &t[static_cast<size_t>(eocd)];
  const uint16_t count = rd16(e + 10);
  const uint32_t cdSize = rd32(e + 12), cdOff = rd32(e + 16);
  if (count == 0xFFFF || cdOff == 0xFFFFFFFFu) return bad("zip64 is not supported (use a folder mod)");
  if (static_cast<int64_t>(cdOff) + cdSize > size) return bad("central directory outside the file");
  std::vector<uint8_t> cd(cdSize);
  _fseeki64(fp, cdOff, SEEK_SET);
  if (std::fread(cd.data(), 1, cd.size(), fp) != cd.size()) return bad("read failed");
  size_t p = 0;
  out->clear();
  for (uint16_t i = 0; i < count; ++i) {
    if (p + 46 > cd.size() || rd32(&cd[p]) != 0x02014B50u) return bad("corrupt central directory");
    ZipMember m;
    m.flags = rd16(&cd[p + 8]);
    m.method = rd16(&cd[p + 10]);
    m.crc = rd32(&cd[p + 16]);
    m.csize = rd32(&cd[p + 20]);
    m.usize = rd32(&cd[p + 24]);
    const uint16_t nlen = rd16(&cd[p + 28]), xlen = rd16(&cd[p + 30]), clen = rd16(&cd[p + 32]);
    m.localOffset = rd32(&cd[p + 42]);
    if (p + 46 + nlen > cd.size()) return bad("corrupt central directory");
    m.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nlen);
    if (m.csize == 0xFFFFFFFFu || m.usize == 0xFFFFFFFFu || m.localOffset == 0xFFFFFFFFu)
      return bad("zip64 is not supported (use a folder mod)");
    p += 46u + nlen + xlen + clen;
    out->push_back(std::move(m));
  }
  return true;
}

std::string zipModRoot(const std::vector<ZipMember>& members) {
  std::vector<std::string> tops;
  bool fileAtRoot = false, modJson = false;
  for (const auto& m : members) {
    if (m.name.empty()) continue;
    std::string low = m.name;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low == "mod.json") modJson = true;
    const size_t s = m.name.find('/');
    const std::string top = s == std::string::npos ? m.name : m.name.substr(0, s);
    if (s == std::string::npos) fileAtRoot = true;
    bool seen = false;
    for (const auto& x : tops) seen = seen || x == top;
    if (!seen) tops.push_back(top);
  }
  if (modJson || fileAtRoot || tops.size() != 1) return "";
  return tops[0] + "/";
}

bool zipExtractMod(const std::string& zipPath, const std::string& dstDir, std::string* err) {
  auto bad = [&](const std::string& m) { if (err) *err = m; return false; };
  std::vector<ZipMember> ms;
  if (!zipList(zipPath, &ms, err)) return false;
  const std::string root = zipModRoot(ms);
  std::FILE* fp = nullptr;
#if defined(_WIN32)
  if (_wfopen_s(&fp, fs::u8path(zipPath).wstring().c_str(), L"rb") != 0) fp = nullptr;
#else
  fp = std::fopen(zipPath.c_str(), "rb");
#endif
  if (!fp) return bad("cannot open the zip");
  struct Closer { std::FILE* f; ~Closer() { std::fclose(f); } } closer{fp};
  std::error_code ec;
  fs::create_directories(fs::u8path(dstDir), ec);
  for (const auto& m : ms) {
    if (m.name.compare(0, root.size(), root) != 0) continue;
    if (!m.name.empty() && m.name.back() == '/') continue;  // a folder entry
    const std::string rel = m.name.substr(root.size());
    if (!safeZipName(rel)) return bad("unsafe path '" + m.name + "' in the zip");
    if (m.flags & 1) return bad("'" + m.name + "' is encrypted");
    if (m.method != 0 && m.method != 8) return bad("'" + m.name + "' uses compression method " + std::to_string(m.method));
    uint8_t lh[30];
    _fseeki64(fp, static_cast<int64_t>(m.localOffset), SEEK_SET);
    if (std::fread(lh, 1, 30, fp) != 30 || rd32(lh) != 0x04034B50u) return bad("corrupt local header of '" + m.name + "'");
    _fseeki64(fp, static_cast<int64_t>(m.localOffset) + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
    const fs::path outPath = fs::u8path(dstDir) / fs::u8path(rel);
    fs::create_directories(outPath.parent_path(), ec);
    std::FILE* of = nullptr;
#if defined(_WIN32)
    if (_wfopen_s(&of, outPath.wstring().c_str(), L"wb") != 0) of = nullptr;
#else
    of = std::fopen(outPath.string().c_str(), "wb");
#endif
    if (!of) return bad("cannot write '" + zipPathU8(outPath) + "'");
    struct OClose { std::FILE* f; ~OClose() { std::fclose(f); } } oclose{of};
    uint32_t crc = 0;
    uint64_t written = 0;
    auto sink = [&](const uint8_t* p, size_t n) -> bool {
      crc = crc32Update(crc, p, n);
      written += n;
      return std::fwrite(p, 1, n, of) == n;
    };
    uint64_t left = m.csize;
    auto pull = [&](uint8_t* dst, size_t cap) -> size_t {
      const size_t want = static_cast<size_t>(left < cap ? left : cap);
      if (!want) return 0;
      const size_t got = std::fread(dst, 1, want, fp);
      left -= got;
      return got;
    };
    if (m.method == 0) {
      std::vector<uint8_t> buf(1 << 20);
      size_t got;
      while ((got = pull(buf.data(), buf.size())) > 0)
        if (!sink(buf.data(), got)) return bad("write failed");
    } else {
      Inflater f;
      f.pull = pull;
      f.sink = sink;
      if (!f.run()) return bad("'" + m.name + "': " + f.err);
    }
    if (written != m.usize) return bad("'" + m.name + "': size mismatch");
    if (crc != m.crc) return bad("'" + m.name + "': CRC mismatch (damaged zip)");
  }
  return true;
}

}  // namespace kernel
}  // namespace sr
