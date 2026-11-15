"""mutate_cube_h1b.py -- prove the H1(b) cube contract tests can FAIL.

The director's proof order for H1(b) puts synthetic fixtures and their
mutations BEFORE any capture. A suite that reports 462/462 proves the
assertions ran, not that any of them discriminates - iter-457 H1(a)-r2 shipped a
test body that stayed green with its production dispatch deleted, and that is
the failure this harness exists to catch.

Each mutant breaks ONE clause of the contract, rebuilds, and runs the suite.
A mutant that still passes means the fixtures are blind to that clause.

r5: the mutants under `T` break gfx/cube_transaction.hpp - the upload-context
lifetime, the versioned publication, the free list and the counters - and are
detected by H1b-15, which drives that unit on a WARP device inside the suite,
or by the pure H1b-5/13/16 tests. A mutant that breaks only the DEVICE
behaviour cannot be caught by a textual check, which is why these exist.

    python tools/mutate_cube_h1b.py

Exit: 0 every mutant is detected / 1 one or more survived.
"""
import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(REPO, 'build', 'SupermanReturns.exe')
R = os.path.join('gfx', 'cube_bind_rule.cpp')
L = os.path.join('gfx', 'cube_layout.hpp')
H = os.path.join('gfx', 'cube_bind_rule.hpp')
G = os.path.join('gfx', 'guest_image.hpp')
T = os.path.join('gfx', 'cube_transaction.hpp')
V = os.path.join('gfx', 'hle_video.cpp')      # H1(b)-P1: the executor's cube arm + preamble writer
NL = chr(10)

# (name, path, find, replace) - `find` must appear EXACTLY once.
MUTANTS = [
    # ── H1(b)-P1: the APPLIED cube is retained for the recorder preamble ────
    # Detected by the P1/P2/P3 fixtures in kernel/tex_unlock_test.cpp, which
    # drive the REAL executor arm (backend call stood in for) and the REAL
    # recorder, then read the artifact back. A crash or a changed number is
    # not a detection; each must fail a normal assertion.
    ("P1: the cube success callback is deleted", V,
     "                  shadowNoteApplied(c, bytes, (uint32_t)cubeTotal, key);" + NL,
     "                  (void)cubeTotal;" + NL),
    ("P1: the callback is hoisted before the success test (runs on refusal too)", V,
     "              if (idx) {" + NL +
     "                s_texSlotCube[key] = idx;" + NL +
     "                cubeUploaded = true;" + NL,
     "              shadowNoteApplied(c, bytes, (uint32_t)((uint64_t)a.bytes + a.mipBytes), key);" + NL +
     "              if (idx) {" + NL +
     "                s_texSlotCube[key] = idx;" + NL +
     "                cubeUploaded = true;" + NL),
    ("P1: the callback also runs from the refusal arm", V,
     "              } else {" + NL +
     "                s_cubeExecUploadRefused.fetch_add(1, std::memory_order_relaxed);",
     "              } else {" + NL +
     "                shadowNoteApplied(c, bytes, (uint32_t)((uint64_t)a.bytes + a.mipBytes), key);" + NL +
     "                s_cubeExecUploadRefused.fetch_add(1, std::memory_order_relaxed);"),
    ("P1: only the base region is retained", V,
     "shadowNoteApplied(c, bytes, (uint32_t)cubeTotal, key);",
     "shadowNoteApplied(c, bytes, a.bytes, key);"),
    ("P1: the retained command drops its mip region (stored mipBytes zeroed)", V,
     "shadowNoteApplied(c, bytes, (uint32_t)cubeTotal, key);",
     "{ RenderCommand cc = c; cc.textureUpload.mipBytes = 0; shadowNoteApplied(cc, bytes, (uint32_t)cubeTotal, key); }"),
    ("P1: the cube is retained under a different key", V,
     "shadowNoteApplied(c, bytes, (uint32_t)cubeTotal, key);",
     "shadowNoteApplied(c, bytes, (uint32_t)cubeTotal, key ^ 0x1000u);"),
    ("P1: the preamble writer serialises only the command bytes field", V,
     "      rec.payloadBytes = (uint32_t)e.bytes.size();",
     "      rec.payloadBytes = e.cmd.textureUpload.bytes;"),
    # ── the resource ────────────────────────────────────────────────────────
    ("the cube is a volume of depth 6 instead of a 2D array", R,
     "  p.depthOrArraySize = (uint16_t)kCubeFaces;   // SIX SLICES, TEXTURE2D",
     "  p.depthOrArraySize = (uint16_t)1;"),

    ("only one face is planned", R,
     "  p.subresourceCount = kCubeFaces * mipLevels;",
     "  p.subresourceCount = mipLevels;"),

    ("a non-square extent is squared up instead of refused", R,
     "  if (width != height) { p.refuse = CubeRefuse::NonSquare; return p; }",
     "  if (false) { p.refuse = CubeRefuse::NonSquare; return p; }"),

    ("more mips than the extent holds are accepted", R,
     "  if (mipLevels > maxMipsFor(width)) {",
     "  if (false) {"),

    ("a zero extent is accepted", R,
     "  if (!width || !height) { p.refuse = CubeRefuse::ZeroExtent; return p; }",
     "  if (false) { p.refuse = CubeRefuse::ZeroExtent; return p; }"),

    ("a non-cube dimension is planned as a cube", R,
     "  if (dim != Dim::Cube) { p.refuse = CubeRefuse::NotCube; return p; }",
     "  if (false) { p.refuse = CubeRefuse::NotCube; return p; }"),

    # ── subresource order ───────────────────────────────────────────────────
    ("faces and mips are transposed in the subresource index", R,
     "  return face * mipLevels + mip;",
     "  return mip * 6u + face;"),

    # ── the upload ──────────────────────────────────────────────────────────
    ("a five-face cube uploads anyway", R,
     "  if (facesPresent != kCubeFaces) {",
     "  if (false) {"),

    ("a missing represented mip uploads anyway", R,
     "  if (mipsPresent != res.mipLevels) {",
     "  if (false) {"),

    ("a short buffer is copied as if it were whole", R,
     "  if (!bytesExpected || bytesProvided != bytesExpected) {",
     "  if (false) {"),

    ("an upload against a REFUSED resource proceeds", R,
     "  if (!res.ok) { u.refuse = res.refuse; return u; }",
     "  if (false) { u.refuse = res.refuse; return u; }"),

    # ── the three descriptor bands ──────────────────────────────────────────
    ("a cube bind ALSO leaves a live index in the 2D band", R,
     "    case Dim::Cube:" + NL + "      p.bandCube = idxCube;" + NL + "      break;",
     "    case Dim::Cube:" + NL + "      p.bandCube = idxCube;" + NL +
     "      p.band2D = idx2D;" + NL + "      break;"),

    ("a cube bind ALSO leaves a live index in the 3D band", R,
     "    case Dim::Cube:" + NL + "      p.bandCube = idxCube;" + NL + "      break;",
     "    case Dim::Cube:" + NL + "      p.bandCube = idxCube;" + NL +
     "      p.band3D = idx3D;" + NL + "      break;"),

    ("a 2D bind ALSO publishes the 3D band, as it did before H1(b)", R,
     "    case Dim::OneD:" + NL + "    case Dim::TwoD:" + NL + "    default:" + NL +
     "      p.band2D = idx2D;" + NL + "      break;",
     "    case Dim::OneD:" + NL + "    case Dim::TwoD:" + NL + "    default:" + NL +
     "      p.band2D = idx2D;" + NL + "      p.band3D = idx3D;" + NL + "      break;"),

    ("a volume bind publishes the 2D band instead of the 3D band", R,
     "    case Dim::ThreeD:" + NL + "      p.band3D = idx3D;" + NL + "      break;",
     "    case Dim::ThreeD:" + NL + "      p.band2D = idx2D;" + NL + "      break;"),

    ("the cube band is never published at all", R,
     "      p.bandCube = idxCube;",
     "      p.bandCube = 0;"),

    # ── iter-457 r2: PACKED-TAIL PLACEMENT ──────────────────────────────────
    # \u26d4 The director found that r1 refused mip 4 on every real cube, and no
    # fixture saw it because they all stopped at layout totals. These target the
    # placement decision that replaced it.
    ("packed levels are REJECTED instead of placed (the r1 defect, restored)", L,
     "  const bool packedHere = L.packedMips && mip >= L.packedLevel;",
     "  const bool packedHere = false;"),

    ("the packed tail is read at the LEVEL's size instead of its extent", L,
     "  p.tailWidth = L.packedTailExtentXBlocks * blockW;" + NL +
     "  p.tailHeight = L.packedTailExtentYBlocks * blockH;",
     "  p.tailWidth = (width >> tail ? width >> tail : 1u);" + NL +
     "  p.tailHeight = (height >> tail ? height >> tail : 1u);"),

    ("a packed sublevel ignores its Xenos (x, y) and sits at the origin", L,
     "  if (!detail::packedMipOffset(width, height, blockW, blockH, mip," + NL +
     "                               p.xBlocks, p.yBlocks)) {",
     "  p.xBlocks = p.yBlocks = 0;" + NL +
     "  if (false) {"),

    ("a packed sublevel reads its OWN byte offset, not the shared tail's", L,
     "  p.byteOffset = uint64_t(L.mipOffsetBytes[tail]) +" + NL +
     "                 uint64_t(L.mipSliceStride[tail]) * face;",
     "  p.byteOffset = uint64_t(L.mipOffsetBytes[mip < kMaxMipLevels ? mip : 0]) +"
     + NL + "                 uint64_t(L.mipSliceStride[tail]) * face;"),

    ("faces are not strided inside a separately-stored mip level", L,
     "    p.byteOffset = uint64_t(L.mipOffsetBytes[mip]) +" + NL +
     "                   uint64_t(L.mipSliceStride[mip]) * face;",
     "    p.byteOffset = uint64_t(L.mipOffsetBytes[mip]);"),

    ("faces are not strided inside the packed tail", L,
     "  p.byteOffset = uint64_t(L.mipOffsetBytes[tail]) +" + NL +
     "                 uint64_t(L.mipSliceStride[tail]) * face;",
     "  p.byteOffset = uint64_t(L.mipOffsetBytes[tail]);"),

    ("the packed tail extent is never recorded, so placement has no window", L,
     "        L.packedTailExtentXBlocks = xExtent;" + NL +
     "        L.packedTailExtentYBlocks = yExtent;",
     "        L.packedTailExtentXBlocks = 0;" + NL +
     "        L.packedTailExtentYBlocks = 0;"),

    # ── dimension resolution ────────────────────────────────────────────────
    ("a stale volume entry outranks the cube table", R,
     "  if (hasCube) return Dim::Cube;" + NL + "  if (hasVolume) return Dim::ThreeD;",
     "  if (hasVolume) return Dim::ThreeD;" + NL + "  if (hasCube) return Dim::Cube;"),

    ("cube keys are never recognised as cubes", R,
     "  if (hasCube) return Dim::Cube;",
     "  if (false) return Dim::Cube;"),

    # ── iter-457 r3: packed_level==0 two-tail, index commit, submit check ──
    ("packed_level==0 mip 0 is read as an ordinary image at the origin", L,
     "  if (mip == 0 && !(L.packedMips && L.packedLevel == 0)) {",
     "  if (mip == 0) {"),

    ("Close HRESULT is ignored (r2 published after a failed Close)", H,
     "  if (failed(hrClose)) return SubmitStatus::CloseFailed;",
     "  if (false) return SubmitStatus::CloseFailed;"),

    ("a wait timeout is treated as success (r2 returned true after 10s)", H,
     "  if (!waitOk) return SubmitStatus::WaitFailed;",
     "  if (false) return SubmitStatus::WaitFailed;"),

    # ── iter-457 r3: submit failure category propagation ────────────────────
    ("CloseFailed collapses to ByteCountMismatch (the r2 collapse)", H,
     "    case SubmitStatus::CloseFailed:  return CubeRefuse::CloseFailed;",
     "    case SubmitStatus::CloseFailed:  return CubeRefuse::ByteCountMismatch;"),

    ("SignalFailed collapses to ByteCountMismatch", H,
     "    case SubmitStatus::SignalFailed: return CubeRefuse::SignalFailed;",
     "    case SubmitStatus::SignalFailed: return CubeRefuse::ByteCountMismatch;"),

    # ── iter-457 r3: guest_image.hpp tiled-address and endian drift ─────────
    # ⚠ guest_image.hpp is CRLF — use \r\n in multi-line anchors (the harness
    # reads with newline='', preserving the raw bytes).
    ("tiled addressing uses a linear offset instead (wrong address)", G,
     "        const int32_t off = xenos_tiling::tiled2D(int32_t(bx), int32_t(by),\r\n"
     "                                                  blockPitch, bpbLog2);",
     "        const int32_t off = int32_t(size_t(by) * blockPitch + bx)\r\n"
     "                         * int32_t(blockBytes);"),

    ("endian swap is skipped entirely", G,
     "  if (endian) {",
     "  if (false) {"),

    # ── iter-457 r5: THE FREE LIST (pure; H1b-5 / H1b-13) ───────────────────
    ("slotAcquire hands out an index without removing it from the free list", T,
     "      freeMask &= ~(1u << i);",
     "      (void)freeMask;"),

    ("the white dummy index 0 is in the free list", T,
     "inline constexpr uint32_t kFreeMaskAll = 0xFEu;   // bits 1..7",
     "inline constexpr uint32_t kFreeMaskAll = 0xFFu;"),

    ("exhaustion hands out the first real index again (aliasing)", T,
     "  why = CB::CubeRefuse::NoSlot;" + NL +
     "  return CB::kCubeDummyIndex;   // degrade to the dummy, never alias a live cube",
     "  why = CB::CubeRefuse::None;" + NL +
     "  return CB::kCubeFirstRealIndex;"),

    ("slotRelease accepts index 0 into the free list", T,
     "  if (idx >= CB::kCubeFirstRealIndex && idx <= CB::kCubeLastRealIndex)" + NL +
     "    freeMask |= (1u << idx);",
     "  freeMask |= (1u << idx);"),

    # ── iter-457 r5: UPLOAD-CONTEXT LIFETIME (device; H1b-15) ───────────────
    ("a Signal failure files the context in RETIRED instead of the LOST list", T,
     "    if (fault == Fault::Signal) {" + NL +
     "      quarantine(std::move(ctx), std::move(keep));" + NL +
     "      return CB::SubmitStatus::SignalFailed;",
     "    if (fault == Fault::Signal) {" + NL +
     "      retire(std::move(ctx), std::move(keep), fv);" + NL +
     "      return CB::SubmitStatus::SignalFailed;"),

    ("a Wait failure returns the context to ACTIVE (Reset under executing work)", T,
     "      if (fault == Fault::Wait) {" + NL +
     "        retire(std::move(ctx), std::move(keep), fv);" + NL +
     "        return CB::SubmitStatus::WaitFailed;",
     "      if (fault == Fault::Wait) {" + NL +
     "        { std::lock_guard<std::mutex> lk(m_mu); m_active = std::move(ctx); }" + NL +
     "        return CB::SubmitStatus::WaitFailed;"),

    ("an Event failure releases the context immediately (no retire)", T,
     "    if (fault == Fault::Event) {" + NL +
     "      retire(std::move(ctx), std::move(keep), fv);" + NL +
     "      return CB::SubmitStatus::EventFailed;",
     "    if (fault == Fault::Event) {" + NL +
     "      return CB::SubmitStatus::EventFailed;"),

    ("a Close failure quarantines the never-executed context (leaks the pool)", T,
     "    if (fault == Fault::Close) return CB::SubmitStatus::CloseFailed;",
     "    if (fault == Fault::Close) { quarantine(std::move(ctx), std::move(keep));"
     " return CB::SubmitStatus::CloseFailed; }"),

    ("drain ignores the completion value and frees pending work", T,
     "    return done >= r.fenceValue;   // the ONLY completion test drain() applies",
     "    return true;"),

    ("drain requires the fence to pass BEYOND the signalled value", T,
     "    return done >= r.fenceValue;   // the ONLY completion test drain() applies",
     "    return done > r.fenceValue;"),

    ("the quarantined (lost) context is reused by the next acquire", T,
     "    if (!ctx) {" + NL + "      ctx = std::make_shared<UploadContext>();",
     "    if (!ctx) {" + NL +
     "      std::lock_guard<std::mutex> lk(m_mu);" + NL +
     "      if (!m_lost.empty()) { ctx = m_lost.back().ctx; m_lost.pop_back(); }" + NL +
     "    }" + NL +
     "    if (!ctx) {" + NL + "      ctx = std::make_shared<UploadContext>();"),

    ("a pending (retired) context is reused by the next acquire", T,
     "    if (!ctx) {" + NL + "      ctx = std::make_shared<UploadContext>();",
     "    if (!ctx) {" + NL +
     "      std::lock_guard<std::mutex> lk(m_mu);" + NL +
     "      if (!m_retired.empty()) { ctx = m_retired.back().ctx; m_retired.pop_back(); }" + NL +
     "    }" + NL +
     "    if (!ctx) {" + NL + "      ctx = std::make_shared<UploadContext>();"),

    # ── iter-457 r5: VERSIONED PUBLICATION (device; H1b-15) ─────────────────
    ("a failed transaction keeps its candidate slot (failures consume slots)", T,
     "    auto refuse = [&](CB::CubeRefuse r) -> uint32_t {" + NL + "      giveBackSlot();",
     "    auto refuse = [&](CB::CubeRefuse r) -> uint32_t {" + NL + "      (void)giveBackSlot;"),

    ("a same-key refresh rewrites the key's PUBLISHED slot", T,
     "    const uint32_t publishIdx = candidateIdx;   // NEVER the key's current slot",
     "    const uint32_t publishIdx = slotForKey(rq.key) ? slotForKey(rq.key) : candidateIdx;"),

    ("the displaced pair is freed at once instead of retired behind the graphics fence", T,
     "        m_retired.push_back({it->second.res, it->second.idx, rq.graphicsFenceNow});",
     "        slotRelease(m_freeMask, it->second.idx);"),

    ("drainGraphics releases old pairs regardless of the fence", T,
     "      if (m_retired[i].graphicsFence <= completedGraphicsFence) {",
     "      if (true) {"),

    # ── iter-457 r6: THE EXACT REQUEST CONTRACT (pure H1b-17 + device H1b-15i)
    ("the Present-ownership guard is removed (r5 verdict correction 1)", T,
     "  if (!rq.renderThreadOwnsPresent) return CB::CubeRefuse::PresentNotOwned;",
     "  if (false) return CB::CubeRefuse::PresentNotOwned;"),

    ("a subresource extent mismatch is copied instead of refused", T,
     "    if (s.w != ew || s.h != eh) return CB::CubeRefuse::ExtentMismatch;",
     "    if (false) return CB::CubeRefuse::ExtentMismatch;"),

    ("an out-of-range subresource index is accepted", T,
     "    if (s.index >= subCount) return CB::CubeRefuse::IndexOutOfRange;",
     "    if (false) return CB::CubeRefuse::IndexOutOfRange;"),

    ("a duplicated subresource index is accepted", T,
     "    if (mark) return CB::CubeRefuse::IndexDuplicate;",
     "    if (false) return CB::CubeRefuse::IndexDuplicate;"),

    ("a missing subresource index is accepted (an incomplete cube uploads)", T,
     "    if (!seen[i]) return CB::CubeRefuse::IndexMissing;",
     "    if (false) return CB::CubeRefuse::IndexMissing;"),

    ("a wrong block-row count is copied instead of refused", T,
     "    if (s.rows != rows) return CB::CubeRefuse::RowCountMismatch;",
     "    if (false) return CB::CubeRefuse::RowCountMismatch;"),

    ("a wrong row footprint is copied instead of refused", T,
     "    if (s.rowBytes != rowBytes) return CB::CubeRefuse::RowBytesMismatch;",
     "    if (false) return CB::CubeRefuse::RowBytesMismatch;"),

    ("undersized backing data is copied instead of refused", T,
     "    if (s.lin.size() != size_t(rows) * rowBytes) return CB::CubeRefuse::BackingMismatch;",
     "    if (false) return CB::CubeRefuse::BackingMismatch;"),

    ("BC1 is given BC3's 16-byte block (wrong compressed footprint)", T,
     "    case DXGI_FORMAT_BC1_UNORM:          blockDim = 4; bytesPerBlock = 8; return true;",
     "    case DXGI_FORMAT_BC1_UNORM:          blockDim = 4; bytesPerBlock = 16; return true;"),

    ("BC3 is treated as an uncompressed 1x1-block format", T,
     "    case DXGI_FORMAT_BC3_UNORM:          blockDim = 4; bytesPerBlock = 16; return true;",
     "    case DXGI_FORMAT_BC3_UNORM:          blockDim = 1; bytesPerBlock = 16; return true;"),

    ("an unknown DXGI format is accepted with a guessed geometry", T,
     "    default: return false;" + NL +
     "  }" + NL +
     "}" + NL +
     "" + NL +
     "// r6: THE EXACT REQUEST CONTRACT, pure (no device)",
     "    default: blockDim = 1; bytesPerBlock = 4; return true;" + NL +
     "  }" + NL +
     "}" + NL +
     "" + NL +
     "// r6: THE EXACT REQUEST CONTRACT, pure (no device)"),

    ("the request field defaults to CLAIMING Present ownership", T,
     "  bool renderThreadOwnsPresent = false;",
     "  bool renderThreadOwnsPresent = true;"),

    ("an injected mid-recording failure continues and publishes a partial cube", T,
     "        m_pool.abortRecording(std::move(ctx));" + NL +
     "        return refuse(CB::CubeRefuse::CloseFailed);",
     "        continue;"),

    # ── iter-457 r5: THE COUNTERS AND THE SUMMARY (H1b-15h / H1b-16) ────────
    ("attempts are not counted", T,
     "  void beginAttempt() { m_attempted.fetch_add(1, std::memory_order_relaxed); }",
     "  void beginAttempt() {}"),

    ("uploads are not counted", T,
     "    m_uploaded.fetch_add(1, std::memory_order_relaxed);" + NL + "    return publishIdx;",
     "    return publishIdx;"),

    ("refusals are not counted", T,
     "    m_refused.fetch_add(1, std::memory_order_relaxed);" + NL +
     "    if (why == CB::CubeRefuse::NoSlot)",
     "    if (why == CB::CubeRefuse::NoSlot)"),

    ("NoSlot refusals are not counted", T,
     "      m_noSlot.fetch_add(1, std::memory_order_relaxed);",
     "      (void)0;"),

    ("the conservation check accepts attempted != uploaded + refused", T,
     "  return s.attempted == s.uploaded + s.refused && s.noSlot <= s.refused &&",
     "  return s.noSlot <= s.refused &&"),

    ("the conservation check ignores a lost index", T,
     "         s.freeSlots + s.liveCubes + s.retiredCubes == CB::kCubeLastRealIndex;",
     "         true;"),
]


def write_text(path, text):
    """Write `text` to `path` with newline='' and PROVE it landed.

    r7: one battery aborted with OSError(22, 'Invalid argument') while
    writing a mutant's source back - and the `finally` restore hit the same
    transient error, so the tree was left MUTATED and the last exe was a
    mutant build. A harness that rewrites production source must not give up
    on the first refused open: retry briefly, then verify by reading back, and
    only then move on. A restore that still fails raises loudly."""
    last = None
    for attempt in range(40):
        try:
            with open(path, 'w', encoding='utf-8', newline='') as f:
                f.write(text)
            if open(path, encoding='utf-8', newline='').read() == text:
                return
            last = RuntimeError('read-back mismatch after write: ' + path)
        except OSError as e:
            last = e
        time.sleep(0.25)
    raise RuntimeError('could not restore/write %s after 40 attempts: %r'
                       % (path, last))


def build():
    """(ok, log). Retries once - a transient Windows link failure (the prior
    suite still holding the exe) is indistinguishable from a real compile
    error in the output, and scoring it as a defect would be a lie."""
    for attempt in (1, 2):
        r = subprocess.run(['cmake', '--build', 'build/'], cwd=REPO,
                           capture_output=True, text=True, encoding='utf-8',
                           errors='replace')
        log = (r.stdout or '') + (r.stderr or '')
        if r.returncode == 0:
            return True, log
        if attempt == 1:
            time.sleep(3.0)
    return False, log


def run_suite():
    """(passed, failed, crashed)."""
    env = dict(os.environ)
    env['SR_HLE_TEX_UNLOCK_TEST'] = '1'
    env.setdefault('SR_MODS', '0')  # iter-463: never mount the player's mods (docs/MODS.md)
    env.setdefault('SR_DEVTOOLS', '0')  # iter-463 phase 2: nor enable a dev tool (docs/DEVTOOLS.md)
    try:
        r = subprocess.run([EXE], cwd=REPO, capture_output=True, text=True,
                           encoding='utf-8', errors='replace', env=env,
                           timeout=300)
    except subprocess.TimeoutExpired:
        return 0, 0, True
    out = (r.stdout or '') + (r.stderr or '')
    m = re.search(r'\[TEXUNLOCK_TEST\] (\d+)/(\d+) passed', out)
    if not m:
        return 0, 0, True
    p, t = int(m.group(1)), int(m.group(2))
    return p, t - p, False


def main() -> int:
    paths = sorted({p for _n, p, _f, _r in MUTANTS})
    originals = {p: open(os.path.join(REPO, p), encoding='utf-8',
                         newline='').read() for p in paths}

    built, log = build()
    if not built:
        print('REFUSED: the clean tree does not build')
        for line in log.splitlines():
            if 'error' in line.lower():
                print('   | ' + line.strip()[:150])
        return 1
    p0, f0, crashed0 = run_suite()
    print('control: %d passed, %d FAILED, crashed=%s' % (p0, f0, crashed0))
    if f0 or crashed0 or not p0:
        print('REFUSED: the control run must be green before mutating')
        return 1

    ok = True
    print(NL + '%d mutants:' % len(MUTANTS) + NL)
    try:
        for name, path, find, repl in MUTANTS:
            src = originals[path]
            if src.count(find) != 1:
                print('  FAIL  %-62s anchor appears %d times (expected 1)'
                      % (name[:62], src.count(find)))
                ok = False
                continue
            full = os.path.join(REPO, path)
            write_text(full, src.replace(find, repl, 1))
            built, blog = build()
            if not built:
                print('  FAIL  %-62s DID NOT COMPILE (proves nothing)'
                      % name[:62])
                for line in blog.splitlines():
                    low = line.lower()
                    if 'error' in low or 'cannot open' in low:
                        print('          | ' + line.strip()[:150])
                ok = False
            else:
                p, f, crashed = run_suite()
                if crashed:
                    print('  FAIL  %-62s crashed (proves nothing)' % name[:62])
                    ok = False
                elif f == 0:
                    print('  FAIL  %-62s SURVIVED (%d/%d, 0 failed)'
                          % (name[:62], p, p + f))
                    ok = False
                else:
                    print('  PASS  %-62s -> %d failed' % (name[:62], f))
            write_text(full, src)
    finally:
        # Restore EVERY file even if one refuses; a leftover mutant is the
        # worst outcome this harness can produce, so it is loud, not silent.
        failed = []
        for p, src in originals.items():
            try:
                write_text(os.path.join(REPO, p), src)
            except RuntimeError as e:
                failed.append(str(e))
        for msg in failed:
            print('  RESTORE FAILED  ' + msg)
        ok = ok and not failed
        restored, rlog = build()
        print(NL + 'restored and rebuilt:', 'ok' if restored else 'BUILD FAILED')
        if not restored:
            # A run that cannot put the source back has no verdict to give.
            ok = False
            for line in rlog.splitlines():
                if 'error' in line.lower():
                    print('   | ' + line.strip()[:150])

    print(NL + ('H1(b) CUBE MUTATION: every mutant is detected by a running suite'
                if ok else
                'H1(b) CUBE MUTATION: SOME MUTANTS SURVIVED - the suite is blind '
                'to them'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
