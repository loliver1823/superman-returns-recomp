"""step 6 I3-B (bindless, Fable ruling 44): source mutants of the bindless path, each killed by ITS named fixture mode.

For each mutant: apply it to gfx/d3d12_backend.cpp (exactly one match, else unusable), build difftest, and run the
mutant's named mode (tools/difftest/native_bindless_run.py's run_mode). KILLED = that mode FAILS (its own assertion);
anything else there (a crash, a hang, an API / device failure, a production refusal it did not name) is
infrastructure (no verdict, the run is unusable). The source is restored byte
for byte after each mutant, and the unmutated fixture must PASS every mode at the end. Runs in the worker tree:

    python tools/difftest/native_bindless_mutants.py --build-dir build [--only M3,M6]

Exit 0 = every mutant killed by its mode and the final run PASS; 1 = a survivor or the final run failed; 2 = unusable.
"""
import argparse
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import native_bindless_run  # noqa: E402

SRC = pathlib.Path("gfx/d3d12_backend.cpp")
# name: (the source text, its mutant, the fixture mode that must catch it). Multi-line texts use "\n"; they are
# matched with the checkout's own line endings.
MUTANTS = {
    # freeze M1: slot exhaustion takes a slot bound in the previous frame (the old rule) ...
    "M1a": ("exhaustVictim(c, m_fenceValues[m_frameIndex], m_texPinned, m_texPinnedN, &victim)",
            "exhaustVictim(c, texFenceNow(), m_texPinned, m_texPinnedN, &victim)", "exhaust-victim"),
    # ... or reuses the victim's slot without waiting for its frame
    "M1b": ("    if (need > completed) {\n      ++m_i3bExhaustWaits;",
            "    if (false) {\n      ++m_i3bExhaustWaits;", "exhaust-victim"),
    # freeze M3: a raw ring slot rewritten without waiting for the submission that read it
    "M3": ("        if (completed < segment.submission) {", "        if (false) {", "raw-ring-gpu"),
    # freeze M5: the selection bounds rule removed (the dimension rule alone remains)
    "M5": ("    if (index >= count || uint64_t(base) + index >= m_xlatSlotDim.size())",
           "    if (uint64_t(base) + index >= m_xlatSlotDim.size())", "selection-range"),
    # freeze M6 (Fable 44 (1)): the submission-time generation check removed
    "M6": ("    if (m_i3bSlotGen[slot] != generation && ++m_i3bBadOpen <= 16)",
           "    if (false && ++m_i3bBadOpen <= 16)", "rewrite-before-submit"),
    # the write-time in-flight check removed
    "M7": ("  if (FAILED(m_nativeImages.completionSnapshot(completed)) || completed >= m_i3bSlotSubmitted[slot]) return;\n"
           "  if (++m_i3bBadInFlight",
           "  return;\n  if (++m_i3bBadInFlight", "rewrite-in-flight"),
    # a raw view reused across recordings (the recording-id test removed)
    "M8": ("  if (m_rawLastRec != m_clOpens || m_rawLastResource != slice.resource()",
           "  if (m_rawLastResource != slice.resource()", "raw-ring-same-recording"),
    # copy-on-write without retiring the old slot
    "M9": ("    m_texSlotRetire.retire(oldSlot, texFenceNow(), std::move(oldResource));\n  } else if (oldResource)",
           "    (void)oldResource;\n  } else if (oldResource)", "cow-gpu"),
    # the per-draw heap set made once per recording (a future bind memo that missed a foreign heap change): the
    # first draw still has the heaps, only the restore after the change is gone
    "M10": ("  commands->SetDescriptorHeaps(2, heaps);\n  commands->SetGraphicsRootDescriptorTable(5, heap);",
            "  static uint64_t heapsRec = ~0ull;\n"
            "  if (heapsRec != m_clOpens) { heapsRec = m_clOpens; commands->SetDescriptorHeaps(2, heaps); }\n"
            "  commands->SetGraphicsRootDescriptorTable(5, heap);", "heap-change-gpu"),
}


def run(cmd):
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(cmd).returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    exe = str(pathlib.Path(a.build_dir) / "difftest.exe")
    original = SRC.read_bytes()
    crlf = b"\r\n" in original  # the checkout's line endings

    def text(t):
        return (t.replace("\n", "\r\n") if crlf else t).encode()

    only = [m for m in a.only.split(",") if m]
    build = ["cmake", "--build", a.build_dir, "-j", "12", "--target", "difftest"]
    chosen = [(n, m) for n, m in MUTANTS.items() if not only or n in only]
    for name, (old, _, mode) in chosen:
        if original.count(text(old)) != 1 or mode not in native_bindless_run.MODES:
            print(f"UNUSABLE {name}: the target text occurs {original.count(text(old))} times / mode {mode}")
            return 2
    survivors, unusable = [], []
    try:
        for name, (old, new, mode) in chosen:
            SRC.write_bytes(original.replace(text(old), text(new)))
            if run(build):
                print(f"UNUSABLE {name}: the mutant does not build")
                return 2
            status, detail = native_bindless_run.run_mode(exe, mode)
            verdict = {"fail": "killed", "ok": "SURVIVED", "infra": "UNUSABLE"}[status]
            print(f"{verdict} {name} by {mode}: {detail}", flush=True)
            if status == "ok":
                survivors.append(name)
            elif status == "infra":
                unusable.append(name)
    finally:
        SRC.write_bytes(original)
    if run(build) or run([sys.executable, "tools/difftest/native_bindless_run.py", exe]):
        print("native_bindless_mutants: FAIL (the unmutated fixture does not pass)")
        return 1
    if unusable:
        print(f"native_bindless_mutants: UNUSABLE {unusable} (infrastructure, no verdict) survivors={survivors}")
        return 2
    print(f"native_bindless_mutants: {'PASS' if not survivors else 'FAIL'} survivors={survivors}")
    return 0 if not survivors else 1


if __name__ == "__main__":
    sys.exit(main())
