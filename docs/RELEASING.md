# Releasing

> Drafted with AI assistance (Claude) for the project owner.

**No release has been made.** Development paused (2026-10-25) before the first one; this page records how
one was meant to be made, for anyone who picks the project up.

How a release is made and what its notes must say. Releases are free: no paywall, no paid early
access, no donation requirement.

## Before a release

1. **Gates for 1.0** (a 0.x pre-release may ship with these open, and its notes say which are open):
   - the native meter at 0: no PM4 packets interpreted, no EDRAM or XMA emulation, no display
     registers answered by emulation ([`VERIFICATION.md`](VERIFICATION.md) section 6) - met on
     2026-10-17 and again at the 2026-10-21 whole-game runs;
   - the release build from the `release` CMake preset, and its composition audit
     (`tools/player/composition_audit.py`, [`VERIFICATION.md`](VERIFICATION.md) section 10) passing;
   - a full human playthrough on the release build, recorded - not done.
2. **Checks on the exact commit being released:**
   - `python scripts/check_credits.py` and `python scripts/audit_forcing.py` pass;
   - the flicker gate passes (`python tools/perf/flicker_gate.py --exe <release exe> --out <dir>`);
   - a whole-run roll of the release build passes `python tools/check_roll.py`;
   - `python tools/player/release_lint.py <package> --disc "<...>\Superman Returns (USA)"` passes:
     no file or 4 KiB block from the disc, no game-derived data, no secrets.
3. **Licence texts:** the package contains `LICENSE`, `THIRD_PARTY_LICENSES.md` and every
   `LICENSE-*` file the build copies next to the exe, and the FFmpeg source offer.
4. **Scan:** upload the release archive to VirusTotal and keep the report link.

## Release notes template

```
## <version> (<date>)

Free. Download only from this Releases page.

You need your own USA Xbox 360 disc of Superman Returns; this release contains no game files.
<one-paragraph summary, written by the owner>

AI use: nearly all code in this project was written by AI agents (mostly Claude, some Codex) under
the owner's direction; see docs/HOW_IT_WAS_BUILT.md and docs/VERIFICATION.md.

### Changes
- ...

### Native status (per in-game frame, whole-run roll)
| PM4 packets | EDRAM resolves / transfers / loads | XMA register accesses | Display-register answers | Shared-memory page validations |
|---|---|---|---|---|
| <n> | <n> / <n> / <n> | <n> | <n> | <n> |

### Tested
- Hardware: <CPU, GPU, OS>
- Reach: <what was played or rolled, and how>

### Known issues
- ...

### Files
| File | SHA-256 |
|---|---|
| <archive>.zip | <sha256> |

VirusTotal: <report link>
Built from commit <hash>.
```

Compute the hash with `Get-FileHash <archive>.zip -Algorithm SHA256` in PowerShell.
