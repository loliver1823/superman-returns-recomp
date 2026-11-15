"""SDL audio env names for harness launches: SDL2 and SDL3 spell them differently.

Step 3 of docs/RELEASE_PLAN.md (the SDL3 migration, lane sdl3-audio) renamed the variables the harness uses to keep
rolls off the owner's speakers:
    SDL2                    SDL3
    SDL_AUDIODRIVER         SDL_AUDIO_DRIVER               (SDL3 also falls back to the SDL2 name itself)
    SDL_DISKAUDIOFILE       SDL_AUDIO_DISK_OUTPUT_FILE     (no fallback in SDL3: without it the disk driver
                                                            writes sdlaudio.raw into the working directory)
A roll may launch an SDL2 control exe or an SDL3 exe, so every launch sets BOTH names. `mirror_env` fills in
whichever name of a pair is missing from the one that is set; the shared launchers (bg_window.py popen_background,
tools/sdl_audio_env.ps1 via bg_window.ps1 Start-SrGameProcess) apply it to every game launch, and tools that
build gate strings use `disk_gates`. The exe also maps the SDL2 names itself (kernel/apu/audio_system.cpp
[AUDIO_ENV]), so a launcher that misses this still never reaches a real device.

Usage:
    from sdl_audio_env import mirror_env, disk_env, disk_gates
    env = mirror_env(dict(os.environ))          # both names of every pair that is set
    env.update(disk_env(out_file))              # disk driver, both spellings
    "SR_X=1," + disk_gates(out_file)            # 'SDL_AUDIODRIVER=disk,SDL_AUDIO_DRIVER=disk[,...file...]'
Self-test: python tools/sdl_audio_env.py --self-test
"""
import sys

# (SDL2 name, SDL3 name)
PAIRS = (
    ("SDL_AUDIODRIVER", "SDL_AUDIO_DRIVER"),
    ("SDL_DISKAUDIOFILE", "SDL_AUDIO_DISK_OUTPUT_FILE"),
)


NO_FILE = "NUL"   # the disk driver's output when nobody asked for the capture (Windows' null device)


def mirror_env(env):
    """Fill in the missing spelling of each pair from the one that is set (in place; returns env). A pair
    whose two names are both set is left alone, even if they differ (the caller said what it wants).
    The disk driver with no output file named writes NUL: SDL2 and SDL3 both default to sdlaudio.raw in the
    working directory (~1 MB/s of a roll into the checkout; seen 2026-10-18, ~250 MB in the dev checkout)."""
    for old, new in PAIRS:
        if env.get(old) and not env.get(new):
            env[new] = env[old]
        elif env.get(new) and not env.get(old):
            env[old] = env[new]
    driver = (env.get("SDL_AUDIO_DRIVER") or env.get("SDL_AUDIODRIVER") or "").strip().lower()
    if driver == "disk" and not (env.get("SDL_AUDIO_DISK_OUTPUT_FILE") or env.get("SDL_DISKAUDIOFILE")):
        env["SDL_AUDIO_DISK_OUTPUT_FILE"] = NO_FILE
        env["SDL_DISKAUDIOFILE"] = NO_FILE
    return env


def disk_env(out_file=None, driver="disk"):
    """The audio driver (default: SDL's disk driver) and, when given, its output file, in both spellings."""
    env = {"SDL_AUDIODRIVER": driver, "SDL_AUDIO_DRIVER": driver}
    if out_file:
        env["SDL_DISKAUDIOFILE"] = str(out_file)
        env["SDL_AUDIO_DISK_OUTPUT_FILE"] = str(out_file)
    return env


def disk_gates(out_file=None, driver="disk"):
    """disk_env as a 'K=V,K=V' gate string (sr_drive -Gates / scenario gates)."""
    return ",".join("%s=%s" % kv for kv in disk_env(out_file, driver).items())


def mirror_gates(gates):
    """A 'K=V,K=V' gate string with the missing spelling of each pair appended (order kept)."""
    parts = [p for p in (gates or "").split(",") if p.strip()]
    kv = {}
    for p in parts:
        if "=" in p:
            k, v = p.split("=", 1)
            kv[k.strip()] = v.strip()
    extra = mirror_env(dict(kv))
    for k, v in extra.items():
        if k not in kv:
            parts.append("%s=%s" % (k, v))
    return ",".join(parts)


def _self_test():
    fails = []

    def check(ok, name):
        print(("PASS " if ok else "FAIL ") + name)
        if not ok:
            fails.append(name)

    e = mirror_env({"SDL_AUDIODRIVER": "disk"})
    check(e == {"SDL_AUDIODRIVER": "disk", "SDL_AUDIO_DRIVER": "disk", "SDL_AUDIO_DISK_OUTPUT_FILE": "NUL",
                "SDL_DISKAUDIOFILE": "NUL"}, "SDL2 name only -> SDL3 name added; no file named -> NUL (both)")
    e = mirror_env({"SDL_AUDIO_DRIVER": "disk", "SDL_DISKAUDIOFILE": "o/a.raw"})
    check(e["SDL_AUDIO_DISK_OUTPUT_FILE"] == "o/a.raw" and e["SDL_DISKAUDIOFILE"] == "o/a.raw",
          "a named disk file is kept (both spellings)")
    e = mirror_env({"SDL_AUDIO_DRIVER": "wasapi"})
    check("SDL_AUDIO_DISK_OUTPUT_FILE" not in e, "another driver: no file added")
    e = mirror_env({"SDL_AUDIO_DRIVER": "dummy", "SDL_AUDIO_DISK_OUTPUT_FILE": "x.raw"})
    check(e == {"SDL_AUDIO_DRIVER": "dummy", "SDL_AUDIODRIVER": "dummy", "SDL_AUDIO_DISK_OUTPUT_FILE": "x.raw",
                "SDL_DISKAUDIOFILE": "x.raw"}, "SDL3 names only -> SDL2 names added")
    e = mirror_env({"SDL_AUDIODRIVER": "disk", "SDL_AUDIO_DRIVER": "wasapi"})
    check(e["SDL_AUDIODRIVER"] == "disk" and e["SDL_AUDIO_DRIVER"] == "wasapi", "both set: left alone")
    e = mirror_env({"SR_X": "1"})
    check(e == {"SR_X": "1"}, "no audio names: unchanged")
    check(disk_gates("C:\\o\\a.raw") == "SDL_AUDIODRIVER=disk,SDL_AUDIO_DRIVER=disk,SDL_DISKAUDIOFILE=C:\\o\\a.raw,"
          "SDL_AUDIO_DISK_OUTPUT_FILE=C:\\o\\a.raw", "disk_gates: both spellings, file in both")
    check(mirror_gates("SR_A=1,SDL_AUDIODRIVER=disk,SR_B=2") == "SR_A=1,SDL_AUDIODRIVER=disk,SR_B=2,SDL_AUDIO_DRIVER=disk,"
          "SDL_AUDIO_DISK_OUTPUT_FILE=NUL,SDL_DISKAUDIOFILE=NUL", "mirror_gates appends the missing spelling (+ NUL file)")
    check(mirror_gates("SR_A=1") == "SR_A=1", "mirror_gates: nothing to add")
    print("[SDL_AUDIO_ENV_TEST] %d failed" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(_self_test())
    print(__doc__)
