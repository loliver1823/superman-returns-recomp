// iter-463 audio: run the native XMA port (kernel/apu/xma_context_new) OFFLINE over real game
// packets and write PCM WAVs. No guest code, no game process, no sound device.
//
//   xma_offline stream <in.xma> <rate_hz> <channels> <out.wav> [options]
//   xma_offline list   <list.txt> <outdir> [options]     one "name rate channels" per line; the
//                                                        input is <dir of list>/<name>.xma
// options:
//   --pkts N     packets per input buffer (default 1 = EA's geometry in the Xenia golden dumps:
//                in0/in1 x 1 packet, refilled one packet at a time)
//   --sdc N      subframe_decode_count (default 4, EA)      --blocks N   output ring blocks (24)
//   --mut N      g_xmaMutation (mutation checks)            --be FILE    also write raw BE PCM
//
// The driver plays the GAME's side of the contract, the way EA DECEAXMA does through the XMA*
// exports: feed in0/in1 (XMASetInputBufferN + SetInputBufferNValid) whenever the decoder has
// consumed one, SetOutputBufferValid, kick (XMAEnableContext = Enable + Work), read the output
// ring from read_offset to write_offset, then SetOutputBufferReadOffset(write_offset). A ring
// that comes back with write == read and output_valid == 0 after a kick on valid input is FULL
// (EaXma_DecodeDrainOutputSpin_sub_826592F0's own reading: avail += 6144).
//
// Prints one line per stream: kicks, frames decoded, decode failures, samples out.
#include "kernel/apu/xma_context_new.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace sr::kernel::apu;

namespace {

struct FlatMem final : IXmaMemory {
  std::vector<uint8_t> m = std::vector<uint8_t>(8u << 20);
  uint8_t* TranslateVirtual(uint32_t va) override { return va + 64 <= m.size() ? &m[va] : nullptr; }
  uint8_t* TranslatePhysical(uint32_t, uint32_t pa, uint32_t bytes) override {
    return (uint64_t)pa + bytes <= m.size() ? &m[pa] : nullptr;
  }
};

constexpr uint32_t kCtx = 0x100, kIn0 = 0x10000, kIn1 = 0x210000, kOut = 0x410000;

struct Opts {
  int pkts = 1, sdc = 4, blocks = 24, mut = 0;
  std::string be;
};

bool readFile(const std::string& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), {});
  return true;
}

void writeWav(const std::string& path, const std::vector<int16_t>& le, int rate, int ch) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t data = (uint32_t)(le.size() * 2);
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16); u16(1); u16((uint16_t)ch); u32(rate); u32(rate * ch * 2); u16((uint16_t)(ch * 2));
  u16(16); std::fwrite("data", 1, 4, f); u32(data);
  std::fwrite(le.data(), 2, le.size(), f);
  std::fclose(f);
}

int rateIndex(int hz) {
  for (int i = 0; i < 4; ++i)
    if (kIdToSampleRate[i] == hz) return i;
  return 2;
}

struct Result {
  uint64_t kicks = 0, frames = 0, fails = 0, blocksOut = 0, phantom = 0;
  std::vector<uint8_t> be;   // BE int16 interleaved, as the guest's output ring holds it
};

Result decodeStream(const std::vector<uint8_t>& pk, int rate, int ch, const Opts& o) {
  Result r;
  FlatMem mem;
  XmaContextNew ctx;
  ctx.Setup(0, &mem, kCtx, makeFfmpegXmaFrameDecoder());
  ctx.set_is_allocated(true);
  const uint32_t npk = (uint32_t)(pk.size() / 2048);
  uint32_t next = 0;
  const uint32_t cap = (uint32_t)o.blocks * 256;
  {
    XMA_CONTEXT_DATA d;
    d.output_buffer_ptr = kOut;
    d.output_buffer_block_count = o.blocks;
    d.subframe_decode_count = o.sdc;
    d.is_stereo = ch == 2;
    d.sample_rate = rateIndex(rate);
    d.input_buffer_read_offset = 32;   // XMAInitializeContext + Clear
    d.Store(mem.TranslateVirtual(kCtx));
  }
  auto feed = [&](int which) {
    if (next >= npk) return;
    const uint32_t n = std::min<uint32_t>((uint32_t)o.pkts, npk - next);
    const uint32_t addr = which ? kIn1 : kIn0;
    std::memcpy(&mem.m[addr], &pk[next * 2048], n * 2048);
    next += n;
    uint8_t* c = mem.TranslateVirtual(kCtx);
    XMA_CONTEXT_DATA d(c);
    if (which) { d.input_buffer_1_ptr = addr; d.input_buffer_1_packet_count = n; d.input_buffer_1_valid = 1; }
    else { d.input_buffer_0_ptr = addr; d.input_buffer_0_packet_count = n; d.input_buffer_0_valid = 1; }
    d.Store(c);
  };
  for (int guard = 0; guard < 1000000; ++guard) {
    uint8_t* c = mem.TranslateVirtual(kCtx);
    {
      XMA_CONTEXT_DATA d(c);
      if (!d.input_buffer_0_valid) feed(0);
    }
    {
      XMA_CONTEXT_DATA d(c);
      if (!d.input_buffer_1_valid) feed(1);
    }
    XMA_CONTEXT_DATA pre(c);
    if (!pre.IsAnyInputBufferValid() && ctx.current_frame_remaining_subframes() == 0) break;
    pre.output_buffer_valid = 1;
    pre.Store(c);
    ctx.BumpKickSeq();
    ctx.Enable();
    ctx.Work();
    r.kicks++;
    XMA_CONTEXT_DATA d(c);
    uint32_t avail = (d.output_buffer_write_offset + o.blocks - d.output_buffer_read_offset) % o.blocks;
    if (avail == 0 && !d.output_buffer_valid) {
      if (pre.IsAnyInputBufferValid()) avail = o.blocks;   // full ring
      else r.phantom++;
    }
    for (uint32_t b = 0; b < avail; ++b) {
      const uint32_t blk = (d.output_buffer_read_offset + b) % o.blocks;
      r.be.insert(r.be.end(), &mem.m[kOut + blk * 256], &mem.m[kOut + blk * 256] + 256);
    }
    r.blocksOut += avail;
    d.output_buffer_read_offset = d.output_buffer_write_offset;   // SetOutputBufferReadOffset
    d.Store(c);
    (void)cap;
  }
  r.frames = ctx.frames_decoded();
  r.fails = ctx.decode_failures();
  return r;
}

int runOne(const std::string& in, int rate, int ch, const std::string& outWav, const Opts& o,
           const std::string& label) {
  std::vector<uint8_t> pk;
  if (!readFile(in, pk) || pk.size() < 2048) {
    std::printf("FAIL %s: cannot read %s\n", label.c_str(), in.c_str());
    return 1;
  }
  Result r = decodeStream(pk, rate, ch, o);
  std::vector<int16_t> le(r.be.size() / 2);
  for (size_t i = 0; i < le.size(); ++i) le[i] = (int16_t)((r.be[2 * i] << 8) | r.be[2 * i + 1]);
  writeWav(outWav, le, rate, ch);
  if (!o.be.empty()) {
    FILE* f = std::fopen(o.be.c_str(), "wb");
    if (f) { std::fwrite(r.be.data(), 1, r.be.size(), f); std::fclose(f); }
  }
  std::printf("%s packets=%zu kicks=%llu frames=%llu decode_fails=%llu blocks=%llu samples=%zu "
              "phantom=%llu\n", label.c_str(), pk.size() / 2048, (unsigned long long)r.kicks,
              (unsigned long long)r.frames, (unsigned long long)r.fails,
              (unsigned long long)r.blocksOut, le.size() / (size_t)ch,
              (unsigned long long)r.phantom);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: xma_offline stream|list ... (see source header)\n");
    return 2;
  }
  std::string mode = argv[1];
  std::vector<std::string> pos;
  Opts o;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto nextI = [&] { return (i + 1 < argc) ? std::atoi(argv[++i]) : 0; };
    if (a == "--pkts") o.pkts = std::max(1, nextI());
    else if (a == "--sdc") o.sdc = nextI();
    else if (a == "--blocks") o.blocks = std::max(1, std::min(31, nextI()));
    else if (a == "--mut") o.mut = nextI();
    else if (a == "--be") o.be = (i + 1 < argc) ? argv[++i] : "";
    else pos.push_back(a);
  }
  g_xmaMutation.store(o.mut);
  if (mode == "stream" && pos.size() >= 4) {
    return runOne(pos[0], std::atoi(pos[1].c_str()), std::atoi(pos[2].c_str()), pos[3], o, pos[0]);
  }
  if (mode == "list" && pos.size() >= 2) {
    std::ifstream lf(pos[0]);
    std::string dir = pos[0].substr(0, pos[0].find_last_of("/\\") + 1);
    std::string line;
    int fails = 0, n = 0;
    while (std::getline(lf, line)) {
      std::istringstream ss(line);
      std::string name;
      int rate = 0, ch = 0;
      if (!(ss >> name >> rate >> ch)) continue;
      Opts oo = o;
      if (!o.be.empty()) oo.be = pos[1] + "/" + name + ".pcmbe";
      fails += runOne(dir + name + ".xma", rate, ch, pos[1] + "/" + name + ".wav", oo, name);
      ++n;
    }
    std::printf("list: %d streams, %d unreadable\n", n, fails);
    return fails ? 1 : 0;
  }
  std::fprintf(stderr, "bad arguments\n");
  return 2;
}
