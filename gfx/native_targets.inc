// iter-467 native-b (strand B: native render targets, resolves, EDRAM elimination).
// Included once, from gfx/d3d12_pm4_shmem.cpp (inside namespace sr::gfx).
// docs/NATIVE_ROADMAP.md "Strand B"; the PM4 / EDRAM path stays the oracle.
//
// B0: the census (SR_NB_CENSUS=<frames>, observe only). Every resolve, texture load (the GPU
// loader: 2D, array layers) and ownership-transfer set of a recorded frame becomes one
// [NB_CENSUS] line with its GPU time: the SR_PM4_GPU_TIME segments that END while the record is
// current (gtMark stamps the record id beside its tag). A texture load names the resolve that
// last wrote its base range (any frame since the census started) and how its identity differs
// from that resolve's destination - which resolves a texture samples as-is, which it
// reinterprets. CPU readers of GPU-written memory (pm4ShmSyncToGuest) are listed too.
//   SR_NB_CENSUS=<n>        record n frames (0 / unset = off)
//   SR_NB_CENSUS_EVERY=<k>  every k-th frame (default 1)
//   SR_NB_CENSUS_SKIP=<s>   start after frame s (default 0)
// tools/nb_census.py aggregates the lines by class.

#if SR_ORACLE
namespace {
struct NbCfg {
  uint32_t max = 0, every = 1, skip = 0;
  NbCfg() {
#if SR_ORACLE  // C1 stage 3: the census is an oracle (observe only); the release configuration records nothing
    if (const char* e = std::getenv("SR_NB_CENSUS")) max = uint32_t(std::strtoul(e, nullptr, 10));
    if (const char* e = std::getenv("SR_NB_CENSUS_EVERY")) every = std::max<uint32_t>(1u, uint32_t(std::strtoul(e, nullptr, 10)));
    if (const char* e = std::getenv("SR_NB_CENSUS_SKIP")) skip = uint32_t(std::strtoul(e, nullptr, 10));
#endif
  }
};
const NbCfg& nbCfg() {
  static const NbCfg c;
  return c;
}
bool nbCensusOn() {
#if SR_ORACLE
  return nbCfg().max != 0;
#else
  return false;
#endif
}
constexpr uint32_t kNbDestRing = 4096;
constexpr uint32_t kNbMaxRecs = 60000;
}  // namespace

uint32_t D3D12Backend::nbAdd(const char* fmt, ...) {
  if (!m_nbFrameOn) return 0;
  std::vector<NbRec>& recs = m_nbRecs[m_frameIndex];
  if (recs.size() >= kNbMaxRecs) return 0;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  recs.push_back(NbRec{buf, 0.0});
  return uint32_t(recs.size());
}

void D3D12Backend::nbAppend(uint32_t id, const char* fmt, ...) {
  std::vector<NbRec>& recs = m_nbRecs[m_frameIndex];
  if (!id || id > recs.size()) return;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (recs[id - 1].line.size() < 8192) recs[id - 1].line += buf;
}

void D3D12Backend::nbAttribute(uint32_t slot, const uint64_t* t, uint32_t n) {
  std::vector<NbRec>& recs = m_nbRecs[slot];
  if (!m_gtFreq) return;
  const uint16_t* items = m_gtItem[slot];
  const uint8_t* tags = m_gtTag[slot];
#if SR_ORACLE
  if (m_nbReplay) {
    NbReplaySlot& sealed = m_nbReplaySlots[slot];
    if (!sealed.sealed && recs.empty()) return;  // an explicitly unsampled frame
    if (!sealed.sealed || !sealed.submission || sealed.count != n || sealed.records != recs.size()) {
      nbReplayFail("unsealed-or-changed-records");
      return;
    }
    if (sealed.attributed) return;
    items = sealed.item;
    tags = sealed.tag;
    for (uint32_t i = 0; i < n; ++i) {
      if (!t[i] || (i && t[i] < t[i - 1]) || tags[i] >= GtTagCount || items[i] > recs.size()) {
        nbReplayFail("invalid-resolved-timestamps");
        return;
      }
    }
    sealed.attributed = true;
  } else if (recs.empty()) return;
#else
  if (recs.empty()) return;
#endif
  const double toMs = 1000.0 / double(m_gtFreq);
  for (uint32_t i = 1; i < n; i++) {
    const uint32_t item = items[i];
    if (!item || item > recs.size() || tags[i] == GtListStart || t[i] <= t[i - 1]) continue;
    recs[item - 1].ms += double(t[i] - t[i - 1]) * toMs;
  }
}

void D3D12Backend::nbDumpCompletedSlot(uint32_t slot) {
  std::vector<NbRec>& recs = m_nbRecs[slot];
  bool pending = !recs.empty();
#if SR_ORACLE
  if (m_nbReplay) pending = pending || m_nbReplaySlots[slot].sealed;
#endif
  if (pending) {
#if SR_ORACLE
    if (m_nbReplay && (m_nbReplayFailed || !m_nbReplaySlots[slot].attributed ||
                      m_nbRecFrame[slot] <= m_nbReplayLastDumpedFrame)) {
      nbReplayFail("unattributed-records");
      return;
    }
#endif
    double ms[5] = {}, cnt[5] = {};
    for (size_t i = 0; i < recs.size(); i++) {
      const NbRec& r = recs[i];
      SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_CENSUS] f=%llu #%zu gpu=%.4f %s",
                  (unsigned long long)m_nbRecFrame[slot], i + 1, r.ms, r.line.c_str());
      const int k = r.line[0] == 'R' ? 0 : r.line[0] == 'T' ? 1 : r.line[0] == 'X' ? 2 : r.line[0] == 'M' ? 3 : 4;
      ms[k] += r.ms;
      cnt[k]++;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NB_CENSUS] f=%llu summary resolves %.0f %.4f ms | tex loads %.0f %.4f ms | transfer sets %.0f "
                "%.4f ms | materialize %.0f %.4f ms | other %.0f",
                (unsigned long long)m_nbRecFrame[slot], cnt[0], ms[0], cnt[1], ms[1], cnt[2], ms[2], cnt[3], ms[3],
                cnt[4]);
    // release/emu-stage3 (Astra round 1 P1): the run's resolves the native kernel did not take, so far - owned = the
    // kernel refused a span it was asked for (nbNatStatus names the reasons; in the dev build the model took it),
    // owners = the owner selection kept the span from the kernel. A pre-merge check expects owned 0.
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NB_CENSUS] f=%llu native declined: owned %llu owners %llu | range splits %llu | %s",
                (unsigned long long)m_nbRecFrame[slot], (unsigned long long)g_nat.declinedOwned,
                (unsigned long long)g_nat.declinedOwners,
                (unsigned long long)edram::g_rangeRectSplits.load(std::memory_order_relaxed), nbNatStatus().c_str());
#if SR_ORACLE
    if (m_nbReplay) {
      ++m_nbReplayDumpedFrames;
      m_nbReplayDumpedRecords += recs.size();
      m_nbReplayLastDumpedFrame = m_nbRecFrame[slot];
      m_nbReplaySlots[slot] = {};
    }
#endif
    recs.clear();
  }
}

void D3D12Backend::nbFrameStart(uint32_t slot) {
  if (!nbCensusOn()) return;
  nbDumpCompletedSlot(slot);
#if SR_ORACLE
  if (m_nbReplay && (m_nbReplayFailed || m_nbReplayFinished)) {
    m_nbFrameOn = false;
    m_nbCur = 0;
    return;
  }
#endif
  static uint32_t s_recorded = 0;
  const NbCfg& c = nbCfg();
  const uint64_t f = m_frameEpoch;
  m_nbFrameOn = s_recorded < c.max && f > c.skip && ((f - c.skip - 1u) % c.every) == 0u;
  if (m_nbFrameOn) s_recorded++;
  m_nbRecFrame[slot] = f;
  m_nbResOrd = 0;
  m_nbCur = 0;
}

#if SR_ORACLE
bool D3D12Backend::nbReplayFail(const char* reason) {
  m_nbReplayFailed = true;
  SR_LOG_BOOT(sr::LogSeverity::Error, "[NB_CENSUS_FINISH] complete=0 reason=%s", reason);
  return false;
}

void D3D12Backend::nbReplayBegin() {
  if (!nbCensusOn()) return;
  if (m_nbReplay || !m_nbRecs[0].empty() || !m_nbRecs[1].empty()) {
    nbReplayFail("replay-already-started-or-pending");
    return;
  }
  m_nbReplay = true;
}

void D3D12Backend::nbReplaySeal(uint32_t slot) {
  if (!m_nbReplay) return;
  // gtFrameEnd has recorded ResolveQueryData, but not necessarily submitted it.
  // Later scanout/readback can reuse m_gtN/tag/item; retain this resolved pair.
  const bool captured = m_nbFrameOn;
  m_nbFrameOn = false;
  m_nbCur = 0;
  if (!captured || m_nbReplayFailed) return;
  NbReplaySlot& sealed = m_nbReplaySlots[slot];
  const uint32_t count = m_gtSlotN[slot];
  if (sealed.sealed || count < 2 || count >= kGtPerSlot - 1 || m_nbRecs[slot].size() >= kNbMaxRecs) {
    nbReplayFail("missing-or-capped-resolved-count");
    return;
  }
  sealed.count = count;
  sealed.records = uint32_t(m_nbRecs[slot].size());
  sealed.recording = m_nativeRecording ? m_nativeRecording->ownership().lock() : nullptr;
  if (!sealed.recording) {
    nbReplayFail("missing-recording-identity");
    return;
  }
  std::copy_n(m_gtTag[slot], count, sealed.tag);
  std::copy_n(m_gtItem[slot], count, sealed.item);
  sealed.sealed = true;
  ++m_nbReplayFrames;
}

void D3D12Backend::nbReplaySubmitted() {
  // openNativeRecording creates a new payload for every list; retain that
  // identity in the seal so a later allocation or reusable COM pointer cannot
  // satisfy the join. Merely closing or abandoning a recording never does.
  const auto recording = m_nativeRecording ? m_nativeRecording->ownership().lock() : nullptr;
  for (NbReplaySlot& sealed : m_nbReplaySlots) {
    if (!sealed.sealed || sealed.recording != recording) continue;
    if (sealed.submission || !m_nativeSubmission) {
      nbReplayFail("duplicate-or-missing-submission");
      continue;
    }
    sealed.submission = m_nativeSubmission;
  }
}

bool D3D12Backend::nbReplayFinish() {
  if (!nbCensusOn()) return true;
  if (!m_nbReplay || m_nbReplayFailed) return nbReplayFail("replay-not-ready");
  if (m_nbReplayFinished) return true;
  if (m_rtOn || m_clState != ClState::Closed || m_pm4DrainOpen)
    return nbReplayFail("render-worker-or-recording-active");
  if (!m_nbReplayFrames || !m_device || !m_commandQueue || !m_gtReady || !m_gtFreq || !m_gtReadback)
    return nbReplayFail("missing-frame-or-query-storage");
  for (uint32_t slot = 0; slot < kFrameCount; ++slot) {
    const NbReplaySlot& sealed = m_nbReplaySlots[slot];
    if (m_nbRecs[slot].empty() && !sealed.sealed) continue;
    if (!sealed.sealed || !sealed.submission || sealed.records != m_nbRecs[slot].size() || sealed.count < 2 ||
        sealed.count >= kGtPerSlot - 1 || sealed.count != m_gtSlotN[slot])
      return nbReplayFail("missing-or-changed-resolved-count");
  }
  // This fence belongs to the same real direct queue. A failed or timed-out
  // checkpoint cannot authorize mapping, printing or clearing any pending row.
  uint64_t ticket = 0;
  HRESULT hr = m_nativeImages.checkpoint(ticket);
  if (FAILED(hr) || !ticket) return nbReplayFail("queue-checkpoint-failed");
  hr = m_nativeImages.wait(ticket, 10000);
  if (FAILED(hr)) return nbReplayFail("queue-completion-failed");

  // Validate/attribute all slots before publishing any final pending inventory.
  for (uint32_t slot = 0; slot < kFrameCount; ++slot) {
    if (m_nbRecs[slot].empty() && !m_nbReplaySlots[slot].sealed) continue;
    const uint32_t n = m_nbReplaySlots[slot].count;
    const size_t off = size_t(slot) * kGtPerSlot * sizeof(uint64_t);
    D3D12_RANGE read{off, off + size_t(n) * sizeof(uint64_t)};
    void* mapped = nullptr;
    hr = m_gtReadback->Map(0, &read, &mapped);
    if (FAILED(hr) || !mapped) return nbReplayFail("query-map-failed");
    nbAttribute(slot, reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(mapped) + off), n);
    const D3D12_RANGE noWrite{0, 0};
    m_gtReadback->Unmap(0, &noWrite);
    if (m_nbReplayFailed) return false;
  }
  uint32_t order[kFrameCount];
  for (uint32_t slot = 0; slot < kFrameCount; ++slot) order[slot] = slot;
  std::sort(order, order + kFrameCount, [&](uint32_t a, uint32_t b) {
    return m_nbRecFrame[a] < m_nbRecFrame[b];
  });
  for (const uint32_t slot : order) {
    nbDumpCompletedSlot(slot);
    if (m_nbReplayFailed) return false;
    m_gtSlotN[slot] = 0;
  }
  if (m_nbReplayFrames != m_nbReplayDumpedFrames) return nbReplayFail("undumped-frame");
  m_nbFrameOn = false;
  m_nbCur = 0;
  m_nbReplayFinished = true;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NB_CENSUS_FINISH] complete=1 frames=%llu dumped_frames=%llu records=%llu fence=%llu",
              (unsigned long long)m_nbReplayFrames, (unsigned long long)m_nbReplayDumpedFrames,
              (unsigned long long)m_nbReplayDumpedRecords, (unsigned long long)ticket);
  return true;
}
#endif

uint32_t D3D12Backend::nbResolveBegin(const edram::ResolveInfo& ri) {
  m_nbResOrd++;
  if (!m_nbFrameOn) return 0;
  const edram::RtKey k = ri.srcKey;
  uint32_t id = nbAdd("R #%u key=%08X %s f%u m%u b%u p%u rect=(%u,%u)-(%u,%u) ss=%u", m_nbResOrd, k.v,
                      ri.isDepth ? "D" : "C", ri.srcFormat, ri.msaa, ri.srcBase, ri.surfacePitch, ri.x0, ri.y0,
                      ri.x1, ri.y1, ri.sampleSelect);
  if (ri.copy)
    nbAppend(id, " -> dest=%08X p%u h%u df%u e%u sw%d arr%d sl%u bias=%g %s", ri.destBase, ri.destPitch,
             ri.destHeight, ri.destFormat, ri.destEndian, ri.destSwap ? 1 : 0, ri.destArray ? 1 : 0, ri.destSlice,
             ri.expBiasFactor, ri.raw ? "raw" : "cvt");
  else
    nbAppend(id, " -> nocopy");
  if (ri.clearColor) nbAppend(id, " clrC=%08X", ri.colorClearKey.v);
  if (ri.clearDepth) nbAppend(id, " clrD=%08X", ri.depthClearKey.v);
  nbBegin(id);
  return id;
}

void D3D12Backend::nbResolveEnd(uint32_t id, const edram::ResolveInfo& ri, char path, uint32_t extStart,
                                uint32_t extLen) {
  if (id) nbAppend(id, " path=%c ext=%08X+%u", path, extStart, extLen);
  if (!nbCensusOn() || !ri.copy || !extLen) return;
  if (m_nbDests.empty()) {
    m_nbDests.resize(kNbDestRing);
    m_nbPageDest.assign(size_t(1) << 17, 0u);  // 512 MiB / 4 KiB
  }
  const uint32_t idx = m_nbDestNext++ % kNbDestRing;
  NbDest& nd = m_nbDests[idx];
  nd.frame = m_frameEpoch;
  nd.ord = m_nbResOrd;
  nd.destBase = ri.destBase & 0x1FFFFFFFu;
  nd.destPitch = ri.destPitch;
  nd.destHeight = ri.destHeight;
  nd.destFormat = ri.destFormat;
  nd.endian = ri.destEndian;
  nd.srcKey = ri.srcKey.v;
  nd.extStart = extStart & 0x1FFFFFFFu;
  nd.extLen = extLen;
  const uint32_t p0 = nd.extStart >> 12;
  const uint32_t p1 = uint32_t(std::min<uint64_t>((uint64_t(nd.extStart) + extLen - 1u) >> 12, (1u << 17) - 1u));
  for (uint32_t p = p0; p <= p1; p++) m_nbPageDest[p] = idx + 1u;
}

uint32_t D3D12Backend::nbTexLoadBegin(const ShmTexDesc& d, bool direct) {
  if (!m_nbFrameOn) return 0;
  const uint32_t id = nbAdd("T base=%08X mip=%08X f%u %ux%u p%u t%d e%u lv%u sub%u s%ux%u %s", d.basePhys, d.mipPhys,
                            d.fmt, d.width, d.height, d.pitchTexels, d.tiled ? 1 : 0, d.endian, d.levels,
                            d.subresource, d.scaleX, d.scaleY, direct ? "direct" : "scratch");
  if (!id) return 0;
  nbBegin(id);
  // The resolves under the base range (pitch and rows aligned to 32 blocks, as the loader reads).
  if (m_nbPageDest.empty()) {
    nbAppend(id, " src=none");
    return id;
  }
  const uint32_t bw = std::max(d.blockW, 1u);
  const uint64_t len = uint64_t((((d.pitchTexels + bw - 1u) / bw) + 31u) & ~31u) * d.blockBytes *
                       (((((d.height + bw - 1u) / bw)) + 31u) & ~31u);
  const uint32_t base = d.basePhys & 0x1FFFFFFFu;
  const uint32_t p0 = base >> 12;
  const uint32_t p1 = uint32_t(std::min<uint64_t>((uint64_t(base) + std::max<uint64_t>(len, 1u) - 1u) >> 12, (1u << 17) - 1u));
  uint32_t found[4] = {};
  uint32_t nf = 0, pagesRes = 0, pagesLive = 0;
  for (uint32_t p = p0; p <= p1; p++) {
    const uint32_t di = m_nbPageDest[p];
    if (!di) continue;
    pagesRes++;
    if (m_shmReady && m_shmGpuWritten[p]) pagesLive++;
    bool seen = false;
    for (uint32_t k = 0; k < nf; k++) seen = seen || found[k] == di;
    if (!seen && nf < 4) found[nf++] = di;
  }
  if (!nf) {
    nbAppend(id, " src=none");
    return id;
  }
  // The destination whose base is the texture's base, else the first one found.
  uint32_t pick = found[0];
  for (uint32_t k = 0; k < nf; k++)
    if ((m_nbDests[found[k] - 1].destBase & ~0xFFFu) == base) pick = found[k];
  const NbDest& nd = m_nbDests[pick - 1];
  char mm[16];
  int mn = 0;
  if ((nd.destBase & ~0xFFFu) != base) mm[mn++] = 'b';
  if (nd.destPitch != ((d.pitchTexels + 31u) & ~31u)) mm[mn++] = 'p';
  if (nd.destFormat != d.fmt) mm[mn++] = 'f';
  if (nd.endian != d.endian) mm[mn++] = 'e';
  if (d.width > nd.destPitch) mm[mn++] = 'w';
  if (d.height > nd.destHeight) mm[mn++] = 'h';
  if (d.levels > 1u) mm[mn++] = 'l';
  if (!d.tiled) mm[mn++] = 'L';
  mm[mn] = 0;
  nbAppend(id, " src=R@f%llu#%u(age %llu) dest=%08X p%u h%u df%u e%u key=%08X dests=%u pages=%u/%u live=%u match=%s",
           (unsigned long long)nd.frame, nd.ord, (unsigned long long)(m_frameEpoch - nd.frame), nd.destBase,
           nd.destPitch, nd.destHeight, nd.destFormat, nd.endian, nd.srcKey, nf, pagesRes, p1 - p0 + 1u, pagesLive,
           mn ? mm : "X");
  return id;
}

void D3D12Backend::nbCpuRead(const char* who, uint32_t phys, uint32_t len, uint32_t pages) {
  if (!nbCensusOn()) return;
  static std::atomic<uint32_t> s_n{0};
  if (s_n.fetch_add(1, std::memory_order_relaxed) < 256)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_CENSUS] f=%llu CPU %s %08X+%u gpuPages=%u",
                (unsigned long long)m_frameEpoch, who, phys, len, pages);
}

// ── SR_NB_RHASH: the sampler-visible destination of each resolve ────────────────────────────────
// The PM4 path: the destination range loaded by the texture loader as its own format (the texture
// a fetch of exactly that identity would get - Xenia's loader, at the draw resolution scale), read
// back over the resolve rectangle (scaled), FNV-1a. The native path (B2) hashes the native target
// over the same rectangle. One GPU sync per resolve: replay / diagnostic only.
namespace {
bool nbRhashOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_RHASH");
    return e && e[0] == '1';
  }();
  return v;
#else
  return false;  // C1 stage 3: a diagnostic of the dev build (the check rolls' RHASH parity)
#endif
}
}  // namespace

void D3D12Backend::nbResolveHash(const edram::ResolveInfo& ri, char path, uint32_t extStart, uint32_t extLen) {
  if (!nbRhashOn() || !ri.copy) return;
  static uint32_t s_n = 0;
  const uint32_t n = ++s_n;
  const resscale::Scale& rs = resscale::get();
  const uint32_t sx = rs.scaled() ? rs.x : 1u, sy = rs.scaled() ? rs.y : 1u;
  DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
  uint32_t bw = 1, bb = 4;
  const bool loadable = pm4LoaderHostFormat(ri.destFormat, dxgi, bw, bb) && pm4ShmLoadable(ri.destFormat) &&
                        bw == 1u && (sx == 1u && sy == 1u ? true : scaledLoadable(ri.destFormat));
  if (!extLen || !loadable || ri.x1 > 4096u || ri.y1 > 4096u || !m_shmReady) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_RHASH] #%u f=%llu key=%08X dest=%08X df%u rect=(%u,%u)-(%u,%u) path=%c unhashable",
                n, (unsigned long long)m_frameEpoch, ri.srcKey.v, ri.destBase, ri.destFormat, ri.x0, ri.y0, ri.x1,
                ri.y1, path);
    return;
  }
  const uint32_t W = ri.x1 * sx, H = ri.y1 * sy;
  // A temporary texture of the destination identity, as getOrCreateTexture2D would create it.
  DXGI_FORMAT resFmt = dxgi;
  bool uav = false;
  if ((sx > 1u || sy > 1u) && pm4TexLoadDirectOn()) {
    DXGI_FORMAT tl = DXGI_FORMAT_UNKNOWN;
    if (pm4TexDirectFormat(dxgi, &tl, nullptr, nullptr, nullptr)) {
      resFmt = tl;
      uav = true;
    }
  }
  NbTmpTex* tt = nullptr;
  for (NbTmpTex& t : m_nbTmpTex)
    if (t.w == W && t.h == H && t.fmt == resFmt && t.uav == uav) tt = &t;
  if (!tt) {
    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W;
    td.Height = H;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = resFmt;
    td.SampleDesc.Count = 1;
    td.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    NbTmpTex nt;
    if (FAILED(m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&nt.tex))))
      return;
    nt.w = W;
    nt.h = H;
    nt.fmt = resFmt;
    nt.uav = uav;
    m_nbTmpTex.push_back(nt);
    tt = &m_nbTmpTex.back();
    tt->tex->SetName(L"NB_RHASH_TMP");
  }
  ID3D12Resource* src = tt->tex.Get();
  // B2: a native-target resolve left the memory held. The hash reads what a later reader of the MEMORY would
  // get: the hold is written back (materialize, the side texture of an overhang included) and the memory
  // loaded like any other path's - which also checks the write-back itself.
  if (path == 'T') nbMaterialize(extStart, extLen);
  NbTgt heldT;
  const bool held = false;
  if (!held) {
    ShmTexDesc d;
    d.fmt = ri.destFormat;
    d.width = ri.x1;
    d.height = ri.y1;
    d.pitchTexels = ri.destPitch;
    d.blockW = 1;
    d.blockBytes = bb;
    d.endian = ri.destEndian;
    d.basePhys = ri.destBase & 0x1FFFFFFFu;
    d.levels = 1;
    d.tiled = true;
    d.dxgi = dxgi;
    d.scaleX = sx;
    d.scaleY = sy;
    // The temp texture starts in COPY_DEST (new) or PIXEL_SHADER_RESOURCE (reused, after the copy below).
    static std::vector<ID3D12Resource*> s_fresh;
    const bool fresh = std::find(s_fresh.begin(), s_fresh.end(), src) == s_fresh.end();
    if (fresh) s_fresh.push_back(src);
    if (!pm4ShmLoadTexture(src, /*fromSR=*/!fresh, d)) {
      SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_RHASH] #%u load failed df%u", n, ri.destFormat);
      return;
    }
  }
  // Read back the scaled rectangle.
  D3D12_RESOURCE_DESC desc = src->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT rows = 0;
  UINT64 rowBytes = 0, total = 0;
  const UINT sub = held ? heldT.slice : 0u;  // an array target's layer (one level)
  m_device->GetCopyableFootprints(&desc, sub, 1, 0, &fp, &rows, &rowBytes, &total);
  fp.Offset = 0;
  if (total > m_nbReadbackBytes) {
    m_nbReadback.Reset();
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&m_nbReadback)))) {
      m_nbReadbackBytes = 0;
      return;
    }
    m_nbReadbackBytes = total;
  }
  pm4ShmBarrier(m_commandList.Get(), src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
  dl.pResource = m_nbReadback.Get();
  dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dl.PlacedFootprint = fp;
  sl.pResource = src;
  sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  sl.SubresourceIndex = sub;
  m_commandList->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
  pm4ShmBarrier(m_commandList.Get(), src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  pm4Flush();
  uint8_t* mp = nullptr;
  const D3D12_RANGE rr{0, SIZE_T(total)};
  if (FAILED(m_nbReadback->Map(0, &rr, reinterpret_cast<void**>(&mp))) || !mp) return;
  const uint32_t bpt = uint32_t(rowBytes / std::max<UINT>(fp.Footprint.Width, 1u));
  uint64_t h = 0xCBF29CE484222325ull;
  uint64_t nz = 0;
  for (uint32_t y = ri.y0 * sy; y < H; y++) {
    const uint8_t* row = mp + uint64_t(y) * fp.Footprint.RowPitch + uint64_t(ri.x0 * sx) * bpt;
    const uint64_t nb = uint64_t(W - ri.x0 * sx) * bpt;
    for (uint64_t i = 0; i < nb; i++) {
      h = (h ^ row[i]) * 0x100000001B3ull;
      nz += row[i] != 0;
    }
  }
  const D3D12_RANGE wr{0, 0};
  m_nbReadback->Unmap(0, &wr);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NB_RHASH] #%u f=%llu key=%08X dest=%08X p%u df%u e%u rect=(%u,%u)-(%u,%u) s%ux%u path=%c src=%s nz=%llu "
              "hash=%016llX",
              n, (unsigned long long)m_frameEpoch, ri.srcKey.v, ri.destBase, ri.destPitch, ri.destFormat, ri.destEndian,
              ri.x0, ri.y0, ri.x1, ri.y1, sx, sy, path, held ? "target" : "load", (unsigned long long)nz,
              (unsigned long long)h);
}

// ── B2: native targets (SR_NB_RESOLVE_TEX, baked ON 2026-10-07, =0 A/B; at a draw resolution scale) ────────────────
#endif  // SR_ORACLE: census records, attribution and per-resolve readback hashes
// The PM4 path resolves into the scaled resolve memory and every texture that samples it LOADS it
// again (texture_load_*_scaled). Here a resolve whose destination is exactly the identity of a texture
// already in the cache (same base, pitch, bytes per pixel, tiled, scale, one level, typeless + UAV,
// guest size covering the rectangle) and that is current up to this resolve writes THAT texture: the
// NAT_TEX kernels (native_resolve.inc) run the native resolve's exact per-pixel path and store each
// destination word through the loader's own transform. The memory is not written; its pages are HELD
// by the target. Any reader of held memory first materializes it: the loader run backwards (texture
// texel -> loader inverse -> endian swap -> the scaled tiled address), so the bytes are exactly what
// the PM4 path left there. Readers: texture loads (2D and array layers), the scan-out, a later resolve
// into the range, a texture eviction.
namespace {
bool nbResTexOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_RESOLVE_TEX");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON (=0 A/B)
  }();
  return v;
#else
  return true;
#endif
}
// SR_NB_OVERHANG (iter-467 native-b, baked ON after check roll cr2; =0 off): a resolve rectangle that runs up to 7 rows past
// the texture's last row (the 8-row resolve granularity: the downsample chain's 320x184 into a 320x180
// texture) still targets the texture; the rows past it go to a side texture of the held entry.
// SR_NB_DEPTH_TARGET (iter-467 native-b, baked ON after check roll cr2; =0 off): a k_24_8_FLOAT depth resolve whose
// destination is a depth texture's identity writes that texture (texture_load_depth_float's transform) and keeps
// the stencil bytes the texture drops in a side texture, so the memory can be written back exactly.
bool nbDepthTargetOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_DEPTH_TARGET");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON (=0 A/B)
  }();
  return v;
#else
  return true;
#endif
}
bool nbOverhangOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_OVERHANG");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON (=0 A/B)
  }();
  return v;
#else
  return true;
#endif
}
// Step-4 lane C, item M7 (Astra P2 on the M2 ZPD merge): SR_NB_GPU_NOTE_LAST (default ON; =0 the old order, dev
// build only). nbResolveToTarget noted the extent GPU-written (pm4ShmRangeWrittenByGpu: the resource memory's
// noteGpuAt and the opt-in reader probe's gpuWrite, both tied to the CURRENT recording) before the overhang block -
// whose second uploadNativeOwnerPlan may flush and open a new recording that then dispatches the overhang rows. The
// note (and the target stamps that read its version) now follow the last dispatch that covers the extent.
bool nbGpuNoteLastOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_GPU_NOTE_LAST");
    return !e || e[0] != '0';
  }();
  return v;
#else
  return true;
#endif
}
// SR_NB_FRONT (iter-467 native-b B4, baked ON after check roll cr2; =0 off): a 32bpp resolve into a buffer the game has
// presented (VdSwap's front buffer) writes a front target - an R32 texture of the destination's raw dwords at the
// draw scale - and holds the memory; the scan-out reads the texture (no guest-layout write, no de-tiling read).
bool nbFrontOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_FRONT");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON (=0 A/B)
  }();
  return v;
#else
  return true;
#endif
}
// SR_NB_TEX_FANOUT (iter-467 native-b, default OFF): a resolve whose destination has more than one current
// target (the shadow cascades: a 2D fetch and layers of stacked fetches) writes the first two extra ones from the
// kernel's own stores (u4 / u5) instead of copying the rectangle into each after it (a read + a write per copy).
// SR_NB_TARGETS_1X (iter-467 native-b, default OFF): B2 at 1x. A resolve whose destination is a cached texture's
// identity writes that texture AND the guest-layout memory in one dispatch (the NAT_DUAL kernels), then stamps the
// texture current - the memory stays exact for any reader (no hold, no materialize), and the texture is not
// reloaded. Textures over GPU-written pages are created typeless + UAV for it. Colour only (depth targets, the
// front target and the overhang side textures stay scale-only: at 1x the memory holds those texels).
bool nbTargets1xOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_TARGETS_1X");
    return e && e[0] == '1';
  }();
  static const int ab = sr::abt::reg("SR_NB_TARGETS_1X");  // SR_AB_TOGGLE (read at the point of use)
  return v && (ab < 0 || sr::abt::on(ab));
#else
  return false;
#endif
}
bool nbTexFanoutOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_TEX_FANOUT");
    return e && e[0] == '1';
  }();
  static const int ab = sr::abt::reg("SR_NB_TEX_FANOUT");  // SR_AB_TOGGLE (read at the point of use)
  return v && (ab < 0 || sr::abt::on(ab));
#else
  return false;
#endif
}
}  // namespace
bool pm4NbTargetsOn() { return nbResTexOn(); }
bool pm4NbScaledRemapOn() { return natScaledRemapOn(); }  // SR_NB_SCALED_REMAP
bool pm4NbTargets1xOn() { return nbTargets1xOn(); }        // SR_NB_TARGETS_1X

// SR_NB_SCALED_REMAP's witness (the check roll grades it): a scaled resolve the native path takes only through the
// remap - mixed spans, or an owner of another sample grid or depth-ness than the source. First 16, then powers of 2.
void nbRemapWitness(const char* path, const edram::ResolveInfo& ri, edram::RtKey owner, edram::RtKey ownerB) {
#if !SR_ORACLE  // C1 stage 3: the witness of a baked gate (the check rolls grade it) - the development build's
  (void)path;
  (void)ri;
  (void)owner;
  (void)ownerB;
  return;
#else
  if (!resscale::get().scaled() || !natScaledRemapOn()) return;
  const edram::RtKey s = ri.srcKey;
  const bool mixed = !ownerB.empty();
  if (!(mixed || owner.isDepth() != s.isDepth() ||
        (owner.msaa() >= edram::kMsaa4X) != (s.msaa() >= edram::kMsaa4X) ||
        (owner.msaa() >= edram::kMsaa2X) != (s.msaa() >= edram::kMsaa2X)))
    return;
  static std::atomic<uint64_t> s_n{0};
  const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1u;
  if (n <= 16u || (n & (n - 1u)) == 0u)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NB_REMAP] scaled remap #%llu path %s key=%08X owner=%08X%s second=%08X rect=(%u,%u)-(%u,%u)",
                (unsigned long long)n, path, s.v, owner.v, mixed ? " mixed" : "", ownerB.v, ri.x0, ri.y0, ri.x1,
                ri.y1);
#endif
}
namespace {

const char kNbMaterializeHlsl[] = R"HLSL(
#define SHADING_LANGUAGE_HLSL_XE 1
#define XE_TEXTURE_LOAD_RESOLUTION_SCALED
#include "endian.xesli"
#include "pixel_formats.xesli"
#include "texture_load.xesli"

RWByteAddressBuffer gMtDst : register(u0, space0);      // the scaled resolve memory at base * area
Texture2DArray<uint4> gMtSrc : register(t0, space0);    // the target, UINT view
Texture2DArray<uint> gMtStencil : register(t1, space0); // a depth target's stencil side texture
Texture2DArray<uint4> gMtAux : register(t2, space0);    // SR_NB_OVERHANG: the side texture
cbuffer Mt : register(b1, space0) {
  uint gMtLog2Bpb; uint gMtUnpack; uint gMtX0; uint gMtX1; uint gMtY0; uint gMtY1;
  uint gMtAuxRow0;  // the guest row (host texels) of the side texture's row 0
  uint gMtMainExt;  // 0: no side texture; else the texture's host width | height << 16 (texels past it: aux)
};
uint4 MtTex(int x, int y) {
  [branch] if (gMtMainExt != 0u && (uint(x) >= (gMtMainExt & 0xFFFFu) || uint(y) >= (gMtMainExt >> 16)))
    return gMtAux.Load(int4(x, y - int(gMtAuxRow0), 0, 0));
  return gMtSrc.Load(int4(x, y, 0, 0));
}
uint MtPack32(uint4 t) {
  if (gMtUnpack == 1u) return (t.x & 0xFFu) | ((t.y & 0xFFu) << 8) | ((t.z & 0xFFu) << 16) | (t.w << 24);
  if (gMtUnpack == 2u) return (t.x & 0xFFFFu) | (t.y << 16);
  return t.x;
}
// One 32-bit guest word from the texel at p: depth targets (unpack 3) invert texture_load_depth_float -
// XeFloat32To20e4 is exact on XeFloat20e4To32's values (low 3 mantissa bits zero) - and take the stencil back.
uint MtWord32(int x, int y) {
  [branch] if (gMtUnpack == 3u) {
    int4 p = int4(x, y, 0, 0);
    return (XeFloat32To20e4(gMtSrc.Load(p).x, true) << 8) | (gMtStencil.Load(p) & 0xFFu);
  }
  return MtPack32(MtTex(x, y));
}
// tex_load_direct.inc's thread layout and addressing, the store and the load swapped.
[numthreads(4, 32, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  XeTextureLoadInfo li = XeTextureLoadGetInfo(pass_push_consts_xe);
  uint b = gMtLog2Bpb;
  uint bpc = 16u >> b;
  uint3 bi = tid << uint3(5u - b, 0u, 0u);
  if (any(bi.xy >= li.size_blocks.xy)) return;
  if (bi.y < gMtY0 || bi.y >= gMtY1) return;
  uint go = XeTextureLoadSourceAddress(li, bi, b);
  [unroll] for (uint c = 0u; c < 2u; c++) {
    uint x0 = bi.x + c * bpc;
    if (x0 >= gMtX0 && x0 < gMtX1) {
      uint4 w;
      if (b == 1u) {  // 8 texels of 16 bits
        uint t[8];
        [unroll] for (uint j = 0u; j < 8u; j++) t[j] = MtTex(int(x0 + j), int(bi.y)).x & 0xFFFFu;
        w = uint4(t[0] | (t[1] << 16), t[2] | (t[3] << 16), t[4] | (t[5] << 16), t[6] | (t[7] << 16));
      } else if (b == 2u) {
        w.x = MtWord32(int(x0), int(bi.y));
        w.y = MtWord32(int(x0 + 1u), int(bi.y));
        w.z = MtWord32(int(x0 + 2u), int(bi.y));
        w.w = MtWord32(int(x0 + 3u), int(bi.y));
      } else {
        uint4 t0 = MtTex(int(x0), int(bi.y));
        uint4 t1 = MtTex(int(x0 + 1u), int(bi.y));
        w = uint4((t0.x & 0xFFFFu) | (t0.y << 16), (t0.z & 0xFFFFu) | (t0.w << 16),
                  (t1.x & 0xFFFFu) | (t1.y << 16), (t1.z & 0xFFFFu) | (t1.w << 16));
      }
      // the loader's swap (16 bpb: XeEndianSwap16) is its own inverse
      gMtDst.Store4(go, b == 1u ? XeEndianSwap16(w, li.endian_32) : XeEndianSwap32(w, li.endian_32));
    }
    if (c == 0u) go += XeTextureLoadLocalXAddressXor(bpc, b, li.is_tiled);
  }
}
)HLSL";

struct NbState {
  std::atomic<bool> texReady{false}, texFailed{false}, texStarted{false};
  Microsoft::WRL::ComPtr<ID3D12RootSignature> rsTex, rsMat;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> psoTex[7], psoMat;
  bool matTried = false, matOk = false;
  int32_t srvBase = -1;
  uint32_t srvNext = 0;
};
NbState g_nb;
constexpr uint32_t kNbSrvRing = 64;

// The NAT_TEX resolve kernels: the native resolve's root signature + u2 (the target) + b3.
bool nbBuildMaterialize(ID3D12Device* dev);
bool nbBuildTexPipelines(ID3D12Device* dev, bool aotOnly = false) {
  auto fail = [&](const char* what, const char* detail) {
    g_nb.texFailed.store(true, std::memory_order_release);
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NB_TARGET] NAT_TEX init failed: %s %s", what, detail ? detail : "");
    return false;
  };
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 1;
  D3D12_DESCRIPTOR_RANGE range2{};
  range2.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  range2.NumDescriptors = 1;
  range2.BaseShaderRegister = 2;
  D3D12_DESCRIPTOR_RANGE range3{};  // t3, t4: a depth owner's planes
  range3.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range3.NumDescriptors = 2;
  range3.BaseShaderRegister = 3;
  D3D12_DESCRIPTOR_RANGE range4{};  // u3: a depth target's stencil side texture
  range4.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  range4.NumDescriptors = 1;
  range4.BaseShaderRegister = 3;
  D3D12_DESCRIPTOR_RANGE range5{}, range6{};  // u4, u5: SR_NB_TEX_FANOUT's second / third target
  range5.RangeType = range6.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  range5.NumDescriptors = range6.NumDescriptors = 1;
  range5.BaseShaderRegister = 4;
  range6.BaseShaderRegister = 5;
  D3D12_ROOT_PARAMETER p[14]{};
  p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  p[0].Constants.Num32BitValues = 5;
  p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p[2].DescriptorTable.NumDescriptorRanges = 1;
  p[2].DescriptorTable.pDescriptorRanges = &range;
  p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  p[3].Descriptor.ShaderRegister = 1;
  p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  p[4].Constants.ShaderRegister = 1;
  p[4].Constants.Num32BitValues = 1;
  p[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  p[5].Descriptor.ShaderRegister = 2;
  p[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  p[6].Constants.ShaderRegister = 2;
  p[6].Constants.Num32BitValues = 24;
  p[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p[7].DescriptorTable.NumDescriptorRanges = 1;
  p[7].DescriptorTable.pDescriptorRanges = &range2;
  p[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  p[8].Constants.ShaderRegister = 3;
  p[8].Constants.Num32BitValues = 8;
  p[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p[9].DescriptorTable.NumDescriptorRanges = 1;
  p[9].DescriptorTable.pDescriptorRanges = &range3;
  p[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p[10].DescriptorTable.NumDescriptorRanges = 1;
  p[10].DescriptorTable.pDescriptorRanges = &range4;
  p[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p[11].DescriptorTable.NumDescriptorRanges = 1;
  p[11].DescriptorTable.pDescriptorRanges = &range5;
  p[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p[12].DescriptorTable.NumDescriptorRanges = 1;
  p[12].DescriptorTable.pDescriptorRanges = &range6;
  p[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  p[13].Descriptor.ShaderRegister = 4;
  for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rd{};
  rd.NumParameters = 14;
  rd.pParameters = p;
  Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
  if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
    return fail("root signature", err ? static_cast<const char*>(err->GetBufferPointer()) : nullptr);
  if (FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_nb.rsTex))))
    return fail("root signature (create)", nullptr);
  const bool scaledSet = resscale::get().scaled();
  for (int k : {0, 1, 3, 4, 5}) {
    char kv[4];
    std::snprintf(kv, sizeof(kv), "%d", k);
    // SR_NB_TARGETS_1X: at 1x the memory is written too (NAT_DUAL). iter-467 freezefx (gfx/pm4_half_gate.hpp):
    // SR_PM4_HALF_SIGNED=0 compiles the old unsigned half-overflow compare here too (a full A/B).
    const bool hexCmp = !pm4HalfSignedOn();
    const D3D_SHADER_MACRO defs[] = {{"NAT_KIND", kv},
                                     {"NAT_TEX", "1"},
                                     {scaledSet ? "XE_RESOLVE_RESOLUTION_SCALED" : "NAT_DUAL", "1"},
                                     {hexCmp ? "SR_HALF_HEXCMP" : nullptr, hexCmp ? "1" : nullptr},
                                     {nullptr, nullptr}};
    std::vector<uint8_t> code;
    std::string cerr;
    if (!nbAotCode("nat_tex", kNativeResolveHlsl, sizeof(kNativeResolveHlsl) - 1, "gpu/shaders/sr_native_resolve.cs.hlsl",
                   defs, aotOnly, code, cerr)) {
      if (aotOnly) return false;
      return fail("compile", cerr.c_str());
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    cd.pRootSignature = g_nb.rsTex.Get();
    cd.CS = {code.data(), code.size()};
    if (FAILED(dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&g_nb.psoTex[k])))) return fail("pso", kv);
  }
  if (nbAotEmit()) nbBuildMaterialize(dev);  // SR_NB_AOT_EMIT: the rarely-needed materialize kernel too
  g_nb.texReady.store(true, std::memory_order_release);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_TARGET] ready: NAT_TEX resolve kernels 16/32/64bpp%s",
              scaledSet ? ", resolution-scaled" : "");
  return true;
}

bool nbBuildMaterialize(ID3D12Device* dev) {
  if (g_nb.matTried) return g_nb.matOk;
  g_nb.matTried = true;
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  D3D12_DESCRIPTOR_RANGE rangeS{};
  rangeS.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  rangeS.NumDescriptors = 1;
  rangeS.BaseShaderRegister = 1;
  D3D12_DESCRIPTOR_RANGE rangeA{};
  rangeA.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  rangeA.NumDescriptors = 1;
  rangeA.BaseShaderRegister = 2;
  D3D12_ROOT_PARAMETER p[6]{};
  p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b0 the loader's constants
  p[0].Constants.Num32BitValues = 10;
  p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u0 the memory
  p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;  // t0 the target
  p[2].DescriptorTable.NumDescriptorRanges = 1;
  p[2].DescriptorTable.pDescriptorRanges = &range;
  p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b1
  p[3].Constants.ShaderRegister = 1;
  p[3].Constants.Num32BitValues = 8;
  p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;  // t1 a depth target's stencil
  p[4].DescriptorTable.NumDescriptorRanges = 1;
  p[4].DescriptorTable.pDescriptorRanges = &rangeS;
  p[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;  // t2 the side texture (SR_NB_OVERHANG)
  p[5].DescriptorTable.NumDescriptorRanges = 1;
  p[5].DescriptorTable.pDescriptorRanges = &rangeA;
  for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rd{};
  rd.NumParameters = 6;
  rd.pParameters = p;
  Microsoft::WRL::ComPtr<ID3DBlob> blob, err, code, cerr;
  if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
      FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_nb.rsMat)))) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NB_TARGET] materialize root signature failed");
    return false;
  }
  std::vector<uint8_t> mcode;
  std::string merr;
  if (!nbAotCode("nb_materialize", kNbMaterializeHlsl, sizeof(kNbMaterializeHlsl) - 1,
                 "gpu/shaders/sr_nb_materialize.cs.hlsl", nullptr, false, mcode, merr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NB_TARGET] materialize compile failed: %s", merr.c_str());
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
  cd.pRootSignature = g_nb.rsMat.Get();
  cd.CS = {mcode.data(), mcode.size()};
  if (FAILED(dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&g_nb.psoMat)))) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NB_TARGET] materialize pso failed");
    return false;
  }
  g_nb.matOk = true;
  return true;
}
}  // namespace

void D3D12Backend::texSharedErase(uint64_t shareKey) {
  auto sit = m_texShared.find(shareKey);
  if (sit == m_texShared.end()) return;
  // A target that holds memory gives it back before it goes (the retire list keeps it alive for the
  // GPU until this frame's work, the materialize included, has run).
  for (uint32_t i = 0; i < m_nbHeld.size(); i++)
    if (m_nbHeld[i].on && m_nbHeld[i].kind == 0u && m_nbHeld[i].key == shareKey) nbMaterializeHeld(i + 1u);
  auto br = m_texSharedByBase.equal_range(sit->second.base);
  for (auto b = br.first; b != br.second; ++b)
    if (b->second == shareKey) {
      m_texSharedByBase.erase(b);
      break;
    }
  m_texShared.erase(sit);
}

bool D3D12Backend::nbTgtInfo(uint8_t kind, uint64_t key, uint32_t slice, NbTgt& t) {
  t = NbTgt{};
  t.kind = kind;
  t.key = key;
  t.slice = slice;
  if (kind == 2u) {  // SR_NB_FRONT: key = the front buffer's guest base
    for (const NbFront& f : m_nbFronts) {
      if (f.base != uint32_t(key) || !f.res) continue;
      t.res = f.res.Get();
      t.base = f.base;
      t.baseLen = f.span;
      t.pitch = f.pitch;
      t.fmt = 6u;     // k_8_8_8_8 (logging only: the words are raw)
      t.endian = 0u;  // the texel IS the memory word
      t.w = f.w;
      t.h = f.h;
      t.tsx = f.tsx;
      t.tsy = f.tsy;
      t.dxgi = DXGI_FORMAT_R32_UINT;
      t.current = f.ver == pm4ShmVersion(f.base, f.span);
      return true;
    }
    return false;
  }
  if (kind == 0u) {
    auto sit = m_texShared.find(key);
    if (sit == m_texShared.end() || !sit->second.tex) return false;
    const TexShared& s = sit->second;
    if (!s.uav || !s.single || !s.tiled) return false;
    t.res = s.tex.Get();
    t.base = s.base;
    t.baseLen = s.baseLen;
    t.pitch = s.pitch;
    t.fmt = s.fmt;
    t.endian = s.endian;
    t.w = s.w;
    t.h = s.h;
    t.tsx = s.tsx;
    t.tsy = s.tsy;
    t.dxgi = s.dxgi;
    t.current = s.contentHash == ((1ull << 63) | pm4ShmVersion(s.base, s.baseLen));
    return true;
  }
  auto ait = m_texArrays.find(key);
  if (ait == m_texArrays.end()) return false;
  const TexArray& a = ait->second;
  if (!a.res || !a.gpu || !a.uav || !a.tiled || a.levels != 1u || slice >= a.layers) return false;
  t.res = a.res.Get();
  t.base = a.base + slice * a.layerStride;
  t.baseLen = a.layerStride;
  t.pitch = a.pitch;
  t.fmt = a.fmt;
  t.endian = a.endian;
  t.w = a.w;
  t.h = a.h;
  t.tsx = a.tsx;
  t.tsy = a.tsy;
  t.dxgi = a.dxgi;
  t.current = slice < a.layerVer.size() && a.layerVer[slice] == pm4ShmVersion(t.base, t.baseLen);
  return true;
}

void D3D12Backend::nbTgtStamp(const NbTgt& t) {
  if (t.kind == 2u) {
    for (NbFront& f : m_nbFronts)
      if (f.base == t.base && f.res.Get() == t.res) f.ver = pm4ShmVersion(f.base, f.span);
    return;
  }
  if (t.kind == 0u) {
    auto sit = m_texShared.find(t.key);
    if (sit != m_texShared.end()) {
      sit->second.contentHash = (1ull << 63) | pm4ShmVersion(t.base, t.baseLen);
#if SR_ORACLE
      if (memres::texIdOn())
        sit->second.texIdOrigin = uint8_t(memres::TexOrigin::kTarget);  // textures_from_resolves: written by identity
#endif
    }
    return;
  }
  auto ait = m_texArrays.find(t.key);
  if (ait != m_texArrays.end() && t.slice < ait->second.layerVer.size()) {
    ait->second.layerVer[t.slice] = pm4ShmVersion(t.base, t.baseLen);
#if SR_ORACLE
    if (memres::texIdOn()) {  // textures_from_resolves (SR_MEM_RES_TEXID=1 only): the layer written by identity
      auto& origin = ait->second.layerOrigin;
      if (origin.size() < ait->second.layerVer.size()) origin.resize(ait->second.layerVer.size(), 0u);
      origin[t.slice] = uint8_t(memres::TexOrigin::kTarget);
    }
#endif
  }
}

#if SR_ORACLE
bool D3D12Backend::nbHeldTarget(uint32_t phys, NbTgt& t) {
  phys &= 0x1FFFFFFFu;
  if (m_nbHeldPage.empty()) return false;
  const uint32_t idx = m_nbHeldPage[phys >> 12];
  if (!idx || idx > m_nbHeld.size() || !m_nbHeld[idx - 1].on) return false;
  const NbHeld& h = m_nbHeld[idx - 1];
  return nbTgtInfo(h.kind, h.key, h.slice, t);
}
#endif

void D3D12Backend::nbMaterialize(uint32_t phys, uint32_t len, uint8_t exceptKind, uint64_t exceptKey,
                                 uint32_t exceptSlice) {
  if (m_nbHeldPage.empty() || !len) return;
  phys &= 0x1FFFFFFFu;
  const uint32_t p0 = phys >> 12;
  const uint32_t p1 = uint32_t(std::min<uint64_t>((uint64_t(phys) + len - 1u) >> 12, (1u << 17) - 1u));
  uint32_t done[8];
  uint32_t nd = 0;
  for (uint32_t p = p0; p <= p1; p++) {
    const uint32_t idx = m_nbHeldPage[p];
    if (!idx) continue;
    bool seen = false;
    for (uint32_t k = 0; k < nd; k++) seen = seen || done[k] == idx;
    if (seen) continue;
    if (nd < 8) done[nd++] = idx;
    if (idx <= m_nbHeld.size()) {
      const NbHeld& h = m_nbHeld[idx - 1];
      if (h.kind == exceptKind && h.key == exceptKey && h.slice == exceptSlice) continue;
    }
    nbMaterializeHeld(idx);
  }
}

int32_t D3D12Backend::nbRingSlot() {
  if (g_nb.srvBase < 0 && m_pm4RtSrvUsed + kNbSrvRing <= pm4RtSrvCapacity()) {
    g_nb.srvBase = int32_t(m_pm4RtSrvUsed);
    m_pm4RtSrvUsed += kNbSrvRing;
  }
  if (g_nb.srvBase < 0) return -1;
  return g_nb.srvBase + int32_t(g_nb.srvNext++ % kNbSrvRing);
}

void D3D12Backend::nbMaterializeHeld(uint32_t idx) {
  if (!idx || idx > m_nbHeld.size()) return;
  NbHeld& h = m_nbHeld[idx - 1];
  if (!h.on) return;
  if (h.auxOn && !h.aux) nativeRecordingFail("materialize-missing-aux", E_UNEXPECTED);
  if (h.depth && !h.stencil) nativeRecordingFail("materialize-missing-stencil", E_UNEXPECTED);
  m_meterCur.materializations++;  // NATIVE METER
  NbTgt t;
  bool ok = false;
  if (nbTgtInfo(h.kind, h.key, h.slice, t) && nbBuildMaterialize(m_device.Get()) && pm4TexLoadDirectInit()) {
    DXGI_FORMAT uavFmt = DXGI_FORMAT_UNKNOWN;
    uint32_t lb = 0, unpack = 0;
    if (!pm4TexDirectFormat(t.dxgi, nullptr, &uavFmt, &lb, &unpack, true)) uavFmt = DXGI_FORMAT_UNKNOWN;
    uint32_t pb = 0;
    sr::gfx::cube::detail::levelSliceStride(0, true, t.w, t.h, t.pitch, 1u, 1u, 1u << lb, true, &pb);
    const uint32_t hGuestAligned = (t.h + 31u) & ~31u;
    const uint64_t span = uint64_t((pb + 31u) & ~31u) * (1u << lb) * hGuestAligned;
    if (uavFmt != DXGI_FORMAT_UNKNOWN && lb >= 1u && lb <= 3u &&
        srEnsure(t.base, uint32_t(std::min<uint64_t>(span, 1ull << 29)))) {
      const int32_t ringSlot = nbRingSlot();
      if (ringSlot >= 0) {
        const uint32_t slot = uint32_t(ringSlot);
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = uavFmt;  // the UINT family view (as the UAV)
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2DArray.MipLevels = 1;
        sv.Texture2DArray.FirstArraySlice = t.slice;
        sv.Texture2DArray.ArraySize = 1;
        publishXlatSrv(t.res, &sv, pm4RtSrvCpu(slot));
        pm4ShmBarrier(m_commandList.Get(), t.res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        srState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // Its own GPU-time segment (SR_PM4_GPU_TIME "materialize", the census 'M' record).
#if SR_ORACLE
        const uint32_t prevCur = m_nbCur;
#endif
        gtMark(GtDraw);
#if SR_ORACLE
        const uint32_t mrec = nbAdd("M base=%08X p%u %ux%u f%u sl%u held=(%u,%u)-(%u,%u)", t.base, t.pitch, t.w, t.h,
                                    t.fmt, t.slice, h.x0, h.y0, h.x1, h.y1);
        m_nbCur = mrec;
#endif
        const uint32_t tsx = t.tsx, tsy = t.tsy;
        const uint32_t wBlocks = t.w * tsx, hBlocks = t.h * tsy;
        const uint32_t c[10] = {1u | ((t.endian & 3u) << 2) | (tsx << 4) | (tsy << 7), 0u, pb, hGuestAligned, wBlocks,
                                hBlocks, 1u, 0u, 0u, t.h};
        const uint32_t x0 = std::min(h.x0, t.w) * tsx, x1 = std::min(h.x1, t.w) * tsx;
        const uint32_t y0 = std::min(h.y0, t.h) * tsy, y1 = std::min(h.y1, t.h) * tsy;
        // SR_NB_OVERHANG: the texels past the texture's last row / column come from the side texture (its row 0
        // = guest row auxY0, its column 0 = guest column 0); the loader addressing covers them (the guest
        // allocation is 32-texel aligned). Inside the texture the texture is the truth.
        const int32_t auxSlot = h.auxOn && h.aux ? nbRingSlot() : -1;
        if (h.auxOn && auxSlot < 0) nativeRecordingFail("materialize-aux-descriptor", E_OUTOFMEMORY);
        uint32_t xEnd = x1, yEnd = y1, auxRow0 = 0u, mainExt = 0u;
        uint32_t cm[10];
        std::memcpy(cm, c, sizeof(cm));
        if (auxSlot >= 0) {
          D3D12_SHADER_RESOURCE_VIEW_DESC av{};
          av.Format = uavFmt;
          av.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
          av.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          av.Texture2DArray.MipLevels = 1;
          av.Texture2DArray.ArraySize = 1;
          publishXlatSrv(h.aux.Get(), &av, pm4RtSrvCpu(uint32_t(auxSlot)));
          pm4ShmBarrier(m_commandList.Get(), h.aux.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          xEnd = std::min(h.x1, std::max(t.w, std::min(h.auxX1, (pb + 31u) & ~31u))) * tsx;
          yEnd = std::min(h.y1, std::max(t.h, std::min(h.auxY1, hGuestAligned))) * tsy;
          auxRow0 = h.auxY0 * tsy;
          mainExt = (t.w * tsx) | ((t.h * tsy) << 16);
          cm[4] = std::max(wBlocks, xEnd);  // size_blocks: through the last overhanging column / row
          cm[5] = std::max(hBlocks, yEnd);
        }
        const uint32_t m[8] = {lb, h.depth ? 3u : unpack, x0, xEnd, y0, yEnd, auxRow0, mainExt};
        int32_t stSlot = -1;
        if (h.depth && h.stencil) {
          stSlot = nbRingSlot();
          if (stSlot < 0) nativeRecordingFail("materialize-stencil-descriptor", E_OUTOFMEMORY);
          if (stSlot >= 0) {
            D3D12_SHADER_RESOURCE_VIEW_DESC ss{};
            ss.Format = DXGI_FORMAT_R8_UINT;
            ss.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            ss.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            ss.Texture2DArray.MipLevels = 1;
            ss.Texture2DArray.ArraySize = 1;
            publishXlatSrv(h.stencil.Get(), &ss, pm4RtSrvCpu(uint32_t(stSlot)));
            pm4ShmBarrier(m_commandList.Get(), h.stencil.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          }
        }
        // step 4 lane C 2i (Astra P1 on the fix): the tables below are xlat-heap handles, and a translated draw's
        // texture bind leaves the native arena's heaps set (pm4ShmLoadTexture / nbResolveToTarget reach here first).
        pm4XlatHeaps();
        m_commandList->SetComputeRootSignature(g_nb.rsMat.Get());
        m_commandList->SetPipelineState(g_nb.psoMat.Get());
        m_commandList->SetComputeRootDescriptorTable(4, pm4RtSrvGpu(uint32_t(stSlot >= 0 ? stSlot : int32_t(slot))));
        m_commandList->SetComputeRootDescriptorTable(5, pm4RtSrvGpu(uint32_t(auxSlot >= 0 ? auxSlot : int32_t(slot))));
        m_commandList->SetComputeRoot32BitConstants(0, 10, cm, 0);
        m_commandList->SetComputeRootUnorderedAccessView(1, m_srBuf->GetGPUVirtualAddress() +
                                                               uint64_t(t.base) * (uint64_t(tsx) * tsy));
        m_commandList->SetComputeRootDescriptorTable(2, pm4RtSrvGpu(slot));
        m_commandList->SetComputeRoot32BitConstants(3, 8, m, 0);
        const uint32_t gxBlocks = 1u << (2u + (5u - lb));  // 4 threads x (32 >> lb) blocks
        m_commandList->Dispatch((cm[4] + gxBlocks - 1u) / gxBlocks, (cm[5] + 31u) / 32u, 1u);
        if (auxSlot >= 0)
          pm4ShmBarrier(m_commandList.Get(), h.aux.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (stSlot >= 0)
          pm4ShmBarrier(m_commandList.Get(), h.stencil.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gtMark(GtNbMat);
#if SR_ORACLE
        m_nbCur = prevCur;
#endif
        m_srUavDirty = true;
        pm4ShmBarrier(m_commandList.Get(), t.res,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ok = true;
        const uint64_t n = ++m_nbMaterializes;
        if (n <= 16u || (n & (n - 1u)) == 0u)
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[NB_TARGET] materialize #%llu base=%08X p%u %ux%u slice=%u rect=(%u,%u)-(%u,%u) lb=%u "
                      "(native resolves %llu)",
                      (unsigned long long)n, t.base, t.pitch, t.w, t.h, t.slice, h.x0, h.y0, h.x1, h.y1, lb,
                      (unsigned long long)m_nbTexResolves);
      }
    }
  }
  if (!ok) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NB_TARGET] materialize FAILED held=%u kind=%u key=%016llX slice=%u; ownership retained",
                idx, unsigned(h.kind), (unsigned long long)h.key, h.slice);
    nativeRecordingFail("materialize-held", E_FAIL);
  }
  // Only a recorded materialization makes these pages current again.
  for (uint16_t& v : m_nbHeldPage)
    if (v == idx) v = 0;
  h.on = false;
  h.auxOn = false;
}

bool D3D12Backend::nbResolveToTarget(const edram::ResolveInfo& ri, const edram::Regs& regs, const Pm4NatOwners& owners,
                                     uint32_t& extStart, uint32_t& extLen) {
  const edram::RtKey owner = owners.a, ownerB = owners.b;
  const bool mixed = !ownerB.empty();
  const resscale::Scale& rs = resscale::get();
  const bool at1x = !rs.scaled();  // SR_NB_TARGETS_1X: the dual write (texture + memory)
  // SR_NB_SCALED_REMAP: an owner of another sample grid or depth-ness, and mixed spans (one colour + one depth
  // owner per tile), through the kernel's remap as pm4NativeResolve does - else the source's own shape only.
  // At 1x the native resolve always remaps.
  const bool remap = natScaledRemapOn() || at1x;
  // Depth (SR_NB_DEPTH_TARGET): a raw k_24_8_FLOAT copy of the source surface itself, read through its plane SRVs.
  const bool depthT = ri.isDepth && nbDepthTargetOn() && ri.raw && owner == ri.srcKey && owner.isDepth() &&
                      owner.format() == 1u && ri.sampleSelect == 0u && !mixed && !at1x;
  if (!nbResTexOn() || !ri.copy || (ri.isDepth && !depthT) || ri.destArray || owner.empty() ||
      (owner.isDepth() && !depthT && !(remap && !ri.isDepth)) || (mixed && !remap))
    return false;
  if ((at1x ? !nbTargets1xOn() : !m_srReady) || !pm4ShmInit() || g_nb.texFailed.load(std::memory_order_acquire))
    return false;
  const edram::RtKey s = ri.srcKey;
  if (!remap && ((owner.msaa() >= edram::kMsaa4X) != (s.msaa() >= edram::kMsaa4X) ||
                 (owner.msaa() >= edram::kMsaa2X) != (s.msaa() >= edram::kMsaa2X)))
    return false;
  // The native resolve's pipelines (and with them the xesl include) first, then the NAT_TEX ones.
  if (!g_nat.ready.load(std::memory_order_acquire)) return false;
  if (!g_nb.texReady.load(std::memory_order_acquire)) {
    if (!g_nb.texStarted.exchange(true)) {
      Microsoft::WRL::ComPtr<ID3D12Device> dev = m_device;
      const char* e = std::getenv("SR_NATIVE_RESOLVE_SYNC");
      if (nbAotOn() && nbBuildTexPipelines(dev.Get(), /*aotOnly=*/true)) {
        // SR_NB_AOT: the shipped blobs, inline (no compile, no warm-up)
      } else if (e && e[0] == '1') {
        nbBuildTexPipelines(dev.Get());
      } else {
        std::thread([dev] { nbBuildTexPipelines(dev.Get()); }).detach();
      }
    }
    if (!g_nb.texReady.load(std::memory_order_acquire)) return false;
  }
  if (!pm4TexLoadDirectInit()) return false;
  XeResolveConsts c{};
  uint32_t gx = 0, gy = 0;
  const int shader = buildResolve(ri, regs, c, gx, gy, extStart, extLen, rs.x, rs.y);
  const int kind = natKindForShader(shader);
  if ((kind != 0 && kind != 1 && kind != 3 && kind != 4 && kind != 5) || !gx || !gy || !extLen) return false;
  const uint32_t destBase = ri.destBase & 0x1FFFFFFFu;
  if (destBase & 0xFFFu) return false;
  const uint32_t bpp = edram::destBytesPerTexel(ri.destFormat);
  if (bpp != 2u && bpp != 4u && bpp != 8u) return false;
  // The target: the cached texture (or array layer) whose identity is the destination's, current now.
  NbTgt tgt;
  bool found = false, sawStale = false;
  DXGI_FORMAT uavFmt = DXGI_FORMAT_UNKNOWN;
  uint32_t lb = 0, unpack = 0;
  // Every current texture with the destination's identity: the first is written by the kernel (and holds
  // the memory), the others - e.g. the same shadow cascade fetched as a 2D texture and as a layer of two
  // stacked fetches - get a copy of the rectangle, so none of them reloads.
  NbTgt seconds[4];
  uint32_t nSeconds = 0;
  auto consider = [&](uint8_t kd, uint64_t key, uint32_t slice) {
    NbTgt t;
    if (!nbTgtInfo(kd, key, slice, t)) return;
    if (t.base != destBase || t.tsx != rs.x || t.tsy != rs.y) return;
    if (((t.pitch + 31u) & ~31u) != ri.destPitch || ri.x0 >= t.w || ri.y0 >= t.h) return;
    if ((ri.x1 > t.w || ri.y1 > t.h) && depthT) return;
    if (ri.x1 > t.w && !(nbOverhangOn() && ri.x1 - t.w < 8u && ri.x1 <= ((t.pitch + 31u) & ~31u))) return;
    if (ri.y1 > t.h && !(nbOverhangOn() && ri.y1 - t.h < 8u && ri.y1 <= ((t.h + 31u) & ~31u))) return;
    const int lsT = loadShaderFor(t.fmt);
    if (depthT ? !(lsT == kLDepthFloat && bpp == 4u)
               : !((lsT == kL16 && bpp == 2u) || (lsT == kL32 && bpp == 4u) || (lsT == kL64 && bpp == 8u)))
      return;
    DXGI_FORMAT uf = DXGI_FORMAT_UNKNOWN;
    uint32_t l = 0, u = 0;
    if (!pm4TexDirectFormat(t.dxgi, nullptr, &uf, &l, &u, true) || (1u << l) != bpp) return;
    if (!t.current) {
      sawStale = true;
      return;
    }
    if (found) {
      if (!depthT && nSeconds < 4u && t.dxgi == tgt.dxgi) seconds[nSeconds++] = t;
      return;
    }
    tgt = t;
    found = true;
    uavFmt = uf;
    lb = l;
    unpack = u;
  };
  {
    auto br = m_texSharedByBase.equal_range(destBase);
    for (auto b = br.first; b != br.second; ++b) consider(0u, b->second, 0u);
    auto ar = m_nbArrayByBase.equal_range(destBase);
    for (auto a = ar.first; a != ar.second; ++a) consider(1u, a->second.first, a->second.second);
  }
  // SR_NB_FRONT (B4): no texture has the identity, but the game presents this buffer - its front target.
  if (!found && !depthT && !at1x && nbFrontOn() && bpp == 4u && (kind == 0 || kind == 1) && nbFrontTarget(ri, destBase) &&
      nbTgtInfo(2u, destBase, 0u, tgt)) {
    found = true;
    uavFmt = DXGI_FORMAT_R32_UINT;
    lb = 2u;
    unpack = 0u;
  }
  if (!found) {
    if (sawStale) m_nbStale++;
    else m_nbNoTarget++;
    return false;
  }
  // The colour owner reads through t1, the depth owner's planes through t3 / t4 (pm4NativeResolve's split).
  const edram::RtKey colourOwner = !owner.isDepth() ? owner : (mixed && !ownerB.isDepth() ? ownerB : edram::RtKey());
  const edram::RtKey depthOwner = owner.isDepth() ? owner : (mixed && ownerB.isDepth() ? ownerB : edram::RtKey());
  Pm4HostRt* rtC = colourOwner.empty() ? nullptr : pm4Rt(colourOwner);
  Pm4HostRt* rtD = depthOwner.empty() ? nullptr : pm4Rt(depthOwner);
  if ((!colourOwner.empty() && (!rtC || rtC->srv < 0)) || (!depthOwner.empty() && (!rtD || rtD->dsrv < 0)))
    return false;
  // A depth target's stencil side texture (the texture's size), prepared before any GPU work.
  Microsoft::WRL::ComPtr<ID3D12Resource> stRes;
  int32_t stSlot = -1;
  if (depthT) {
    const uint32_t sw = tgt.w * rs.x, sh = tgt.h * rs.y;
    for (const NbHeld& e : m_nbHeld)
      if (e.kind == tgt.kind && e.key == tgt.key && e.slice == tgt.slice && e.stencil && e.stencilW == sw &&
          e.stencilH == sh)
        stRes = e.stencil;
    if (!stRes) {
      D3D12_HEAP_PROPERTIES def{};
      def.Type = D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC td{};
      td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      td.Width = sw;
      td.Height = sh;
      td.DepthOrArraySize = 1;
      td.MipLevels = 1;
      td.Format = DXGI_FORMAT_R8_UINT;
      td.SampleDesc.Count = 1;
      td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      if (FAILED(m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   nullptr, IID_PPV_ARGS(&stRes))))
        return false;
      stRes->SetName(L"NB_DEPTH_STENCIL");
    }
    stSlot = nbRingSlot();
    if (stSlot < 0) return false;
    D3D12_UNORDERED_ACCESS_VIEW_DESC su{};
    su.Format = DXGI_FORMAT_R8_UINT;
    su.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    su.Texture2DArray.ArraySize = 1;
    m_device->CreateUnorderedAccessView(stRes.Get(), nullptr, &su, pm4RtSrvCpu(uint32_t(stSlot)));
  }
  // SR_NB_OVERHANG: the rows past the texture, resolved again from the 8-row block that holds its last row; a
  // rectangle past the texture's width resolved again whole (the side texture is the rectangle's columns from 0).
  const bool overX = ri.x1 > tgt.w;
  // (at 1x the texels past the texture go to the memory only: the texture's out-of-bounds stores are dropped)
  const bool overhang = !at1x && (ri.y1 > tgt.h || overX);
  edram::ResolveInfo r2 = ri;
  XeResolveConsts c2{};
  uint32_t gx2 = 0, gy2 = 0;
  DXGI_FORMAT auxTypeless = DXGI_FORMAT_UNKNOWN;
  Microsoft::WRL::ComPtr<ID3D12Resource> auxRes;
  uint32_t auxW = 0, auxH = 0;
  int32_t auxSlot = -1;
  if (overhang) {
    r2.y0 = overX ? ri.y0 : std::max(ri.y0, tgt.h & ~7u);
    uint32_t es2 = 0, el2 = 0;
    if (buildResolve(r2, regs, c2, gx2, gy2, es2, el2, rs.x, rs.y) != shader || !gx2 || !gy2) return false;
    if (!pm4TexDirectFormat(tgt.dxgi, &auxTypeless, nullptr, nullptr, nullptr, true)) return false;
    auxW = std::max(tgt.w, ri.x1) * rs.x;
    auxH = (ri.y1 - r2.y0) * rs.y;
    for (const NbHeld& e : m_nbHeld)  // this target's side texture from an earlier resolve
      if (e.kind == tgt.kind && e.key == tgt.key && e.slice == tgt.slice && e.aux && e.auxW == auxW &&
          e.auxH >= auxH && e.auxFmt == auxTypeless)
        auxRes = e.aux;
    if (!auxRes) {
      D3D12_HEAP_PROPERTIES def{};
      def.Type = D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC td{};
      td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      td.Width = auxW;
      td.Height = auxH;
      td.DepthOrArraySize = 1;
      td.MipLevels = 1;
      td.Format = auxTypeless;
      td.SampleDesc.Count = 1;
      td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      if (FAILED(m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                   IID_PPV_ARGS(&auxRes))))
        return false;  // the memory path writes those rows instead
      auxRes->SetName(L"NB_OVERHANG");
    }
    auxSlot = nbRingSlot();
    if (auxSlot < 0) return false;
  }
#if SR_ORACLE
  // SR_NB_NATIVE_PREHASH: the rebaseline hashes before m_shmMu (pm4ShmResolve's order; see pm4NativeResolve).
  struct PreDrop {
    D3D12Backend* b;
    ~PreDrop() { b->pm4ShmRebaseDrop(); }
  } preDrop{this};
  if (nbNativePrehashOn() && pm4ShmRebaseUnlockedEnabled()) pm4ShmRebaseHash(extStart, extLen);
#endif
  std::lock_guard<std::recursive_mutex> lk(m_shmMu);
  const uint32_t extG = extStart & 0x1FFFFFFFu;
  // Memory another target holds in this extent goes back first (this one keeps what it holds).
  // The owner's pixel grid relative to the source (pm4NativeResolve's remap).
  const auto shX = [](edram::RtKey k) { return k.msaa() >= edram::kMsaa4X ? 1u : 0u; };
  const auto shY = [](edram::RtKey k) { return k.msaa() >= edram::kMsaa2X ? 1u : 0u; };
  uint32_t depthC[24] = {};
  depthC[20] = rs.x;  // SR_NB_SCALED_REMAP: the kernel's scaled tile dimensions
  depthC[21] = rs.y;
  const auto remapOf = [&](edram::RtKey o) {
    return shX(s) | (shY(s) << 4) | (shX(o) << 8) | (shY(o) << 12) | (o.isDepth() ? 0x10000u : 0u) |
           (o.isDepth() != s.isDepth() ? 0x20000u : 0u);
  };
  depthC[6] = owner.format();
  depthC[7] = remapOf(owner);
  depthC[11] = s.pitchTiles();  // retained source-pixel alias correspondence
  if (mixed) {
    depthC[8] = ownerB.format();
    depthC[9] = remapOf(ownerB);
  }
  // depthC[4] (gNatS1Bpt) = 0: a depth owner's planes come through its SRVs (t3 depth, t4 stencil).
  // The base tile's texel origin (buildResolve folds x0 & ~mask, y0 & ~mask into the destination base).
  uint32_t bpbLog2 = 0;
  while ((1u << bpbLog2) < bpp) bpbLog2++;
  const uint32_t mask = (1u << (7u - std::min(2u, bpbLog2))) - 1u;
  // The source pixel of the rectangle's origin: buildResolve also folds the whole EDRAM tiles before
  // it into the base tile (edram_offset_scaled keeps only the in-tile offset), which a host-RT read
  // must add back - gNatSrcFold (depthC[22..23]), the constant the memory path sets too since
  // release/emu-stage3 (Astra round 1 P1; before, this path carried it in texC[4..5] alone and
  // pm4NativeResolve refused such origins).
  const uint32_t lx = ri.msaa >= edram::kMsaa4X ? 1u : 0u, ly = ri.msaa >= edram::kMsaa2X ? 1u : 0u;
  const uint32_t foldX = (((ri.x0 << lx) / 80u) * 80u) >> lx, foldY = (((ri.y0 << ly) / 16u) * 16u) >> ly;
  depthC[22] = foldX * rs.x;
  depthC[23] = foldY * rs.y;
  native_owner::Plan ownerPlan;
  if (!natBuildOwnerPlan(ri, owners.ranges, mixed ? owners.nRanges : 0u, c,
                         depthC[22], depthC[23], gy, !at1x, ownerPlan)) return false;
  // Validate both dispatch domains before publishing either dispatch. The
  // second upload stays next to its binds: a flush may recycle the first one.
  native_owner::Plan ownerPlan2;
  uint32_t depthC2[24]{};
  if (overhang) {
    const uint32_t fold2Y = (((r2.y0 << ly) / 16u) * 16u) >> ly;
    std::memcpy(depthC2, depthC, sizeof depthC2);
    depthC2[23] = fold2Y * rs.y;
    if (!natBuildOwnerPlan(r2, owners.ranges, mixed ? owners.nRanges : 0u, c2,
                           depthC2[22], depthC2[23], gy2, !at1x, ownerPlan2)) return false;
  }
  nbMaterialize(extG, extLen, tgt.kind, tgt.key, tgt.slice);
  // SR_NB_TEX_FANOUT: the first two seconds receive the kernel's stores (texC[7] = how many), the rest a copy.
  const uint32_t nFan = (nbTexFanoutOn() && !depthT) ? std::min(nSeconds, 2u) : 0u;
  const uint32_t texC[8] = {tgt.endian & 3u, unpack, (ri.x0 & ~mask) * rs.x, (ri.y0 & ~mask) * rs.y,
                            0u, 0u, depthT ? 1u : 0u, nFan};
  const uint32_t srcSrv = uint32_t(rtC ? rtC->srv : rtD->dsrv);  // t1 (a depth-only read: any valid table)
  const uint32_t uavSlot = pm4TldUavSlot();
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
  ud.Format = uavFmt;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
  ud.Texture2DArray.FirstArraySlice = tgt.slice;
  ud.Texture2DArray.ArraySize = 1;
  m_device->CreateUnorderedAccessView(tgt.res, nullptr, &ud, pm4RtSrvCpu(uavSlot));
  uint32_t fanSlot[2] = {uavSlot, uavSlot};  // unused tables: the target's own UAV (never written: texC[7])
  for (uint32_t k = 0; k < nFan; k++) {
    fanSlot[k] = pm4TldUavSlot();
    D3D12_UNORDERED_ACCESS_VIEW_DESC fd = ud;
    fd.Texture2DArray.FirstArraySlice = seconds[k].slice;
    m_device->CreateUnorderedAccessView(seconds[k].res, nullptr, &fd, pm4RtSrvCpu(fanSlot[k]));
    pm4ShmBarrier(m_commandList.Get(), seconds[k].res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  if (rtC) pm4Transition(*rtC, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (rtD) pm4Transition(*rtD, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  pm4ShmBarrier(m_commandList.Get(), tgt.res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  // p[9]: the depth planes (a colour dispatch never reads them: any valid table); p[10]: the stencil side
  // texture (colour: the target's own UAV, never written).
  const uint32_t planesSlot = rtD ? uint32_t(rtD->dsrv) : srcSrv;
  const uint32_t stencilSlot = stSlot >= 0 ? uint32_t(stSlot) : uavSlot;
  // At 1x u0 is the shared memory itself (NAT_DUAL), else any valid buffer (the scaled resolve memory).
  if (at1x) {
    const memres::KindScope mrKind(memres::Kind::kResolve);  // mem.resources step 2a: the census's fetch kind
    pm4ShmRequest(extStart, extLen);
    pm4ShmState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  const D3D12_GPU_VIRTUAL_ADDRESS dummyUav = at1x ? m_shm->GetGPUVirtualAddress() : m_srBuf->GetGPUVirtualAddress();
  D3D12_GPU_VIRTUAL_ADDRESS ownerAddress = 0;
  if (!uploadNativeOwnerPlan(ownerPlan, ownerAddress)) return false;
  // Allocation may flush, ending the earlier native image recording leases.
  if (rtC) pm4Transition(*rtC, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (rtD) pm4Transition(*rtD, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  // step 4 lane C 2i (Astra P1 on the fix): a successful target resolve never reaches pm4NativeResolve's re-set, and
  // a translated draw's texture bind leaves the native arena's heaps set. After the owner-plan upload (a flush there
  // re-opens the list with the xlat heaps; this covers the no-flush case).
  pm4XlatHeaps();
  m_commandList->SetComputeRootSignature(g_nb.rsTex.Get());
  m_commandList->SetPipelineState(g_nb.psoTex[kind].Get());
  m_commandList->SetComputeRoot32BitConstants(0, 5, &c, 0);
  m_commandList->SetComputeRootUnorderedAccessView(1, dummyUav);
  m_commandList->SetComputeRootDescriptorTable(2, pm4RtSrvGpu(srcSrv));
  m_commandList->SetComputeRootUnorderedAccessView(3, dummyUav);
  const uint32_t frontPitch = 0;
  m_commandList->SetComputeRoot32BitConstants(4, 1, &frontPitch, 0);
  m_commandList->SetComputeRootShaderResourceView(5, m_shmUpload->GetGPUVirtualAddress());
  m_commandList->SetComputeRoot32BitConstants(6, 24, depthC, 0);
  m_commandList->SetComputeRootConstantBufferView(13, ownerAddress);
  m_commandList->SetComputeRootDescriptorTable(7, pm4RtSrvGpu(uavSlot));
  m_commandList->SetComputeRoot32BitConstants(8, 8, texC, 0);
  m_commandList->SetComputeRootDescriptorTable(9, pm4RtSrvGpu(planesSlot));
  m_commandList->SetComputeRootDescriptorTable(10, pm4RtSrvGpu(stencilSlot));
  m_commandList->SetComputeRootDescriptorTable(11, pm4RtSrvGpu(fanSlot[0]));
  m_commandList->SetComputeRootDescriptorTable(12, pm4RtSrvGpu(fanSlot[1]));
  m_commandList->Dispatch(gx, gy, 1);
  nbRemapWitness("T", ri, owner, ownerB);
  for (uint32_t k = 0; k < nFan; k++)
    pm4ShmBarrier(m_commandList.Get(), seconds[k].res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  if (nFan) m_nbFanouts += nFan;
  if (stRes) {
    D3D12_RESOURCE_BARRIER sb{};
    sb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    sb.UAV.pResource = stRes.Get();
    m_commandList->ResourceBarrier(1, &sb);
  }
  pm4ShmBarrier(m_commandList.Get(), tgt.res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  // As a scaled resolve: the pages are resolution-scaled and their versions move (other textures over
  // them reload - through the materialize funnel). The target is current at the new version.
  // The other current targets of this identity: the rectangle copied from the first (same format family).
  if (nSeconds > nFan) {
    pm4ShmBarrier(m_commandList.Get(), tgt.res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    const D3D12_BOX box{ri.x0 * rs.x, ri.y0 * rs.y, 0u, std::min(ri.x1, tgt.w) * rs.x, std::min(ri.y1, tgt.h) * rs.y,
                        1u};
    for (uint32_t k = nFan; k < nSeconds; k++) {
      const NbTgt& s2 = seconds[k];
      pm4ShmBarrier(m_commandList.Get(), s2.res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
      dl.pResource = s2.res;
      dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dl.SubresourceIndex = s2.slice;
      sl.pResource = tgt.res;
      sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      sl.SubresourceIndex = tgt.slice;
      m_commandList->CopyTextureRegion(&dl, ri.x0 * rs.x, ri.y0 * rs.y, 0, &sl, &box);
      pm4ShmBarrier(m_commandList.Get(), s2.res, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    pm4ShmBarrier(m_commandList.Get(), tgt.res, D3D12_RESOURCE_STATE_COPY_SOURCE,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  if (at1x) {  // SR_NB_TARGETS_1X: the memory is written (exact for every reader) - nothing is held
    m_shmUavDirty = true;
    pm4ShmRangeWrittenByGpu(extStart, extLen);
    m_shmResolves++;
    nbTgtStamp(tgt);
    for (uint32_t k = 0; k < nSeconds; k++) nbTgtStamp(seconds[k]);
    const uint64_t n1 = ++m_nbTexResolves;
    if (n1 <= 16u || (n1 & (n1 - 1u)) == 0u)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[NB_TARGET] native resolve at 1x #%llu kind=%d key=%08X rect=(%u,%u)-(%u,%u) -> %s base=%08X "
                  "slice=%u p%u %ux%u fmt=%u, memory written too (seconds=%u fan=%u)",
                  (unsigned long long)n1, kind, ri.srcKey.v, ri.x0, ri.y0, ri.x1, ri.y1,
                  tgt.kind ? "array" : "texture", tgt.base, tgt.slice, tgt.pitch, tgt.w, tgt.h, tgt.fmt, nSeconds,
                  nFan);
    return true;
  }
  // M7: the GPU-write note follows the last dispatch covering the extent (the overhang's, below, when there is one).
  const bool noteLast = nbGpuNoteLastOn();
  auto noteWritten = [&] {
    srMark(extG, extLen);
    pm4ShmRangeWrittenByGpu(extG, extLen);
    m_shmResolves++;
    m_srResolves++;
    nbTgtStamp(tgt);
    for (uint32_t k = 0; k < nSeconds; k++) nbTgtStamp(seconds[k]);
  };
  if (!noteLast) noteWritten();
  // Hold the extent's pages.
  if (m_nbHeldPage.empty()) m_nbHeldPage.assign(size_t(1) << 17, 0u);
  uint32_t idx = 0;
  for (uint32_t i = 0; i < m_nbHeld.size() && !idx; i++)
    if (m_nbHeld[i].on && m_nbHeld[i].kind == tgt.kind && m_nbHeld[i].key == tgt.key && m_nbHeld[i].slice == tgt.slice)
      idx = i + 1u;
  if (!idx) {
    for (uint32_t i = 0; i < m_nbHeld.size() && !idx; i++)
      if (!m_nbHeld[i].on) idx = i + 1u;
    if (!idx) {
      m_nbHeld.push_back(NbHeld{});
      idx = uint32_t(m_nbHeld.size());
    }
    NbHeld& nh = m_nbHeld[idx - 1];
    if (nh.aux) retireResource(nh.aux);
    if (nh.stencil) retireResource(nh.stencil);
    nh = NbHeld{};
    nh.kind = tgt.kind;
    nh.key = tgt.key;
    nh.slice = tgt.slice;
    nh.x0 = ri.x0;
    nh.y0 = ri.y0;
    nh.x1 = ri.x1;
    nh.y1 = ri.y1;
    nh.on = true;
  } else if (!tgt.current) {
    // A front target (B4) whose memory changed under it (the CPU rewrote it): only this rectangle is valid.
    NbHeld& nh = m_nbHeld[idx - 1];
    nh.x0 = ri.x0;
    nh.y0 = ri.y0;
    nh.x1 = ri.x1;
    nh.y1 = ri.y1;
  } else {
    NbHeld& nh = m_nbHeld[idx - 1];
    nh.x0 = std::min(nh.x0, ri.x0);
    nh.y0 = std::min(nh.y0, ri.y0);
    nh.x1 = std::max(nh.x1, ri.x1);
    nh.y1 = std::max(nh.y1, ri.y1);
  }
  const uint32_t p0 = extG >> 12;
  const uint32_t p1 = uint32_t(std::min<uint64_t>((uint64_t(extG) + extLen - 1u) >> 12, (1u << 17) - 1u));
  for (uint32_t p = p0; p <= p1; p++) m_nbHeldPage[p] = uint16_t(idx);
  if (depthT) {
    NbHeld& nh = m_nbHeld[idx - 1];
    if (nh.stencil && nh.stencil.Get() != stRes.Get()) retireResource(nh.stencil);
    nh.stencil = stRes;
    nh.stencilW = tgt.w * rs.x;
    nh.stencilH = tgt.h * rs.y;
    nh.depth = true;
    m_nbDepthTargets++;
  }
  if (overhang) {
    NbHeld& nh = m_nbHeld[idx - 1];
    if (nh.aux && nh.aux.Get() != auxRes.Get()) retireResource(nh.aux);
    nh.aux = auxRes;
    nh.auxW = auxW;
    nh.auxH = auxH;
    nh.auxFmt = auxTypeless;
    D3D12_UNORDERED_ACCESS_VIEW_DESC au{};
    au.Format = uavFmt;
    au.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    au.Texture2DArray.ArraySize = 1;
    m_device->CreateUnorderedAccessView(nh.aux.Get(), nullptr, &au, pm4RtSrvCpu(uint32_t(auxSlot)));
    // The side texture's row 0 is guest row r2.y0: the origin moves up by it (the uint wrap is exact). The second
    // rectangle's own source fold (gNatSrcFold) rides in its own copy of the depth constants.
    const uint32_t texC2[8] = {tgt.endian & 3u, unpack, (r2.x0 & ~mask) * rs.x,
                               (r2.y0 & ~mask) * rs.y - r2.y0 * rs.y, 0u, 0u, 0u, 0u};
    D3D12_GPU_VIRTUAL_ADDRESS ownerAddress2 = 0;
    if (!uploadNativeOwnerPlan(ownerPlan2, ownerAddress2))
      nativeRecordingFail("native-owner-upload-after-dispatch", E_FAIL);
    if (rtC) pm4Transition(*rtC, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (rtD) pm4Transition(*rtD, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    pm4XlatHeaps();  // the second rectangle: as the first (its owner-plan upload may flush too)
    m_commandList->SetComputeRootSignature(g_nb.rsTex.Get());
    m_commandList->SetPipelineState(g_nb.psoTex[kind].Get());
    m_commandList->SetComputeRoot32BitConstants(0, 5, &c2, 0);
    m_commandList->SetComputeRootUnorderedAccessView(1, dummyUav);
    m_commandList->SetComputeRootDescriptorTable(2, pm4RtSrvGpu(srcSrv));
    m_commandList->SetComputeRootUnorderedAccessView(3, dummyUav);
    m_commandList->SetComputeRoot32BitConstants(4, 1, &frontPitch, 0);
    m_commandList->SetComputeRootShaderResourceView(5, m_shmUpload->GetGPUVirtualAddress());
    m_commandList->SetComputeRoot32BitConstants(6, 24, depthC2, 0);
    m_commandList->SetComputeRootConstantBufferView(13, ownerAddress2);
    m_commandList->SetComputeRootDescriptorTable(7, pm4RtSrvGpu(uint32_t(auxSlot)));
    m_commandList->SetComputeRoot32BitConstants(8, 8, texC2, 0);
    m_commandList->SetComputeRootDescriptorTable(9, pm4RtSrvGpu(planesSlot));
    m_commandList->SetComputeRootDescriptorTable(10, pm4RtSrvGpu(uint32_t(auxSlot)));
    m_commandList->SetComputeRootDescriptorTable(11, pm4RtSrvGpu(uint32_t(auxSlot)));  // texC2[7] = 0: unused
    m_commandList->SetComputeRootDescriptorTable(12, pm4RtSrvGpu(uint32_t(auxSlot)));
    m_commandList->Dispatch(gx2, gy2, 1);
    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = nh.aux.Get();
    m_commandList->ResourceBarrier(1, &ub);
    nh.auxY0 = r2.y0;
    nh.auxY1 = ri.y1;
    nh.auxX1 = std::max(tgt.w, ri.x1);
    nh.auxOn = true;
    m_nbOverhangs++;
  }
  if (noteLast) noteWritten();
  if (tgt.kind == 2u) m_nbFrontResolves++;
  const uint64_t n = ++m_nbTexResolves;
  if (n <= 16u || (n & (n - 1u)) == 0u)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NB_TARGET] native resolve #%llu kind=%d key=%08X rect=(%u,%u)-(%u,%u) -> %s base=%08X slice=%u p%u "
                "%ux%u fmt=%u dxgi=%u seconds=%u fan=%u (no target %llu, stale %llu, materialized %llu, fanned %llu, "
                "overhangs %llu)",
                (unsigned long long)n, kind, ri.srcKey.v, ri.x0, ri.y0, ri.x1, ri.y1, tgt.kind == 2u ? "front" : tgt.kind ? "array" : "texture",
                tgt.base, tgt.slice, tgt.pitch, tgt.w, tgt.h, tgt.fmt, unsigned(tgt.dxgi), nSeconds, nFan,
                (unsigned long long)m_nbNoTarget, (unsigned long long)m_nbStale, (unsigned long long)m_nbMaterializes,
                (unsigned long long)m_nbFanouts, (unsigned long long)m_nbOverhangs);
  return true;
}

// ── iter-467 native-b B4 (SR_NB_FRONT): front buffers as native targets ──────────────────────────────────
// A front target is created for a buffer the scan-out has presented (the swap's base, size and pitch); the first
// resolve into a buffer after its first present takes the memory path. Its texture holds the destination's raw
// dwords at the draw scale (texel (x, y) = the word the scaled scan-out reads for host pixel (x, y)), so the
// materialize (nbMaterializeHeld: R32_UINT, endian 0, unpack 0 = the identity) writes back exactly what the memory
// path would have written.
bool D3D12Backend::nbFrontTarget(const edram::ResolveInfo& ri, uint32_t destBase) {
  const resscale::Scale& rs = resscale::get();
  const NbFrontSeen* seen = nullptr;
  for (const NbFrontSeen& s : m_nbFrontSeen)
    if (s.use && s.base == destBase && s.pitch == ri.destPitch) seen = &s;
  if (!seen || ri.x1 > seen->w || ri.y1 > seen->h) return false;
  auto drop = [&](NbFront& f) {  // its held memory back first, then the texture goes
    for (uint32_t i = 0; i < m_nbHeld.size(); i++)
      if (m_nbHeld[i].on && m_nbHeld[i].kind == 2u && m_nbHeld[i].key == f.base) nbMaterializeHeld(i + 1u);
    if (f.res) retireResource(f.res);
    f.res.Reset();
    f.ver = ~0ull;
  };
  NbFront* f = nullptr;
  for (NbFront& e : m_nbFronts)
    if (e.base == destBase) f = &e;
  if (f && f->res &&
      (f->w != seen->w || f->h != seen->h || f->pitch != seen->pitch || f->tsx != rs.x || f->tsy != rs.y))
    drop(*f);
  if (!f) {
    if (m_nbFronts.size() >= 4u) {  // the least recently used goes
      f = &m_nbFronts[0];
      for (NbFront& e : m_nbFronts)
        if (e.lastUse < f->lastUse) f = &e;
      drop(*f);
    } else {
      m_nbFronts.push_back(NbFront{});
      f = &m_nbFronts.back();
    }
    f->base = destBase;
  }
  if (!f->res) {
    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = seen->w * rs.x;
    td.Height = seen->h * rs.y;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                 IID_PPV_ARGS(&f->res)))) {
      f->res.Reset();
      return false;
    }
    f->res->SetName(L"NB_FRONT");
    f->w = seen->w;
    f->h = seen->h;
    f->pitch = seen->pitch;
    f->tsx = rs.x;
    f->tsy = rs.y;
    f->span = uint32_t(xenos_tiling::tiledSpanBytes32(f->pitch, f->h));
    f->ver = ~0ull;  // nothing valid yet: the first resolve's rectangle starts the hold
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_FRONT] front target base=%08X %ux%u p%u at %ux%u (%zu targets)", f->base,
                f->w, f->h, f->pitch, f->w * f->tsx, f->h * f->tsy, m_nbFronts.size());
  }
  f->lastUse = ++m_nbFrontUse;
  return true;
}

int32_t D3D12Backend::nbFrontScanSrv(uint32_t fbPhys, uint32_t width, uint32_t height, uint32_t pitch) {
  if (!nbFrontOn() || !nbResTexOn() || !m_nbFrontPso) return -1;
  const resscale::Scale& rs = resscale::get();
  const uint32_t base = fbPhys & 0x1FFFFFFFu;
  {  // remember the buffer: a later resolve into it writes its front target
    NbFrontSeen* s = nullptr;
    for (NbFrontSeen& e : m_nbFrontSeen)
      if (e.use && e.base == base) s = &e;
    if (!s) {
      s = &m_nbFrontSeen[0];
      for (NbFrontSeen& e : m_nbFrontSeen)
        if (e.use < s->use) s = &e;
    }
    s->base = base;
    s->w = width;
    s->h = height;
    s->pitch = pitch;
    s->use = ++m_nbFrontUse;
  }
  NbFront* f = nullptr;
  for (NbFront& e : m_nbFronts)
    if (e.base == base && e.res) f = &e;
  if (!f) return -1;
  // The texture is what the memory holds (current) over the whole presented rectangle (the hold).
  bool covered = false;
  for (const NbHeld& h : m_nbHeld)
    if (h.on && h.kind == 2u && h.key == base && h.x0 == 0u && h.y0 == 0u && h.x1 >= width && h.y1 >= height)
      covered = true;
  const bool current = f->ver == pm4ShmVersion(base, f->span);
  if (f->pitch != pitch || f->w < width || f->h < height || f->tsx != rs.x || f->tsy != rs.y || !covered ||
      !current) {
    const uint64_t nf = ++m_nbFrontFallbacks;
    if (nf <= 8u || (nf & (nf - 1u)) == 0u)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[NB_FRONT] scan-out fb=%08X %ux%u from memory: target %ux%u p%u covered=%d current=%d (fallback %llu)",
                  fbPhys, width, height, f->w, f->h, f->pitch, covered ? 1 : 0, current ? 1 : 0,
                  (unsigned long long)nf);
    return -1;
  }
  const int32_t slot = nbRingSlot();
  if (slot < 0) return -1;
  D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = DXGI_FORMAT_R32_UINT;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = 1;
  publishXlatSrv(f->res.Get(), &sv, pm4RtSrvCpu(uint32_t(slot)));
  const uint64_t n = ++m_nbFrontScans;
  if (n <= 4u || (n & (n - 1u)) == 0u)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NB_FRONT] scan-out fb=%08X %ux%u from its front target (%llu scans, %llu front resolves, %llu fallbacks)",
                fbPhys, width, height, (unsigned long long)n, (unsigned long long)m_nbFrontResolves,
                (unsigned long long)m_nbFrontFallbacks);
  return slot;
}
