"""Build and exercise a selected native component, retaining bounded evidence.

Run on the configured cloud worktree through sr_cloud.py run (see
docs/DEVELOPMENT_LOOP.md). Execution stays on the cloud worker, including Ninja's
automatic reconfigure. --plan only prints the target selection and works locally.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid

from set_runner import ROOT, RunLock, atomic, digest, no_links, quiet_active, run_child

COMPONENTS = {
    "images": ("sr_native_image_store_test", "sr_native_image_replay"),
    "executor": ("sr_native_record_executor_test",),
    "draw-builder": ("sr_native_draw_builder_test",),
    "pipeline": ("sr_native_pipeline_test",),
    "draw": ("sr_native_draw_test",),
    "texture-views": ("sr_native_texture_views_test",),
    "constants": ("sr_translated_constants_test",),
    "samplers": ("sr_translated_samplers_test",),
    "textures": ("sr_translated_textures_test",),
}


def selection(components):
    if not components or any(x not in COMPONENTS for x in components):
        raise ValueError("Select a known component explicitly; this is not an automatic coverage claim")
    return list(dict.fromkeys(t for c in components for t in COMPONENTS[c]))


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True,
                          text=True, timeout=30, check=True).stdout.strip()


def worker_root(root):
    # The desktop C: bulk-output restriction remains intact. Only the established
    # worker worktree layout can use its C: project archive.
    return root.parent == Path("C:/sr/wt") and root.name not in ("", ".", "..")


def source_identity():
    diff = subprocess.run(["git", "-C", str(ROOT), "diff", "--binary", "--no-ext-diff", "HEAD", "--", "."],
                          capture_output=True, timeout=30, check=True).stdout
    untracked = subprocess.run(["git", "-C", str(ROOT), "ls-files", "--others", "--exclude-standard", "-z"],
                               capture_output=True, timeout=30, check=True).stdout
    files = {}
    for name in untracked.decode("utf-8").split("\0"):
        if name:
            path = ROOT / name
            no_links(path)
            files[name] = digest(path)
    return {"commit": git("rev-parse", "HEAD"), "diff_sha256": hashlib.sha256(diff).hexdigest(),
            "untracked": files}


def preflight(spec, out):
    if (out / "CANCEL").exists() or quiet_active(spec):
        raise ValueError("Cancellation or quiet window is active; no command launched")


def validate_build_source(build):
    entries = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8-sig").splitlines():
        if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
            entries["source"] = line.partition("=")[2]
    if not entries.get("source") or Path(entries["source"]).resolve() != ROOT.resolve():
        raise ValueError("Configured build belongs to another source tree; configure this worktree first")


def cache_program(build, *, configuring=False):
    """Use the configured launcher, or the established worker tool before configure."""
    cache = build / "CMakeCache.txt"
    entries = {}
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8-sig").splitlines():
            if line.startswith(("SCCACHE:BOOL=", "SCCACHE_PROGRAM:FILEPATH=")):
                key, _, value = line.partition("=")
                entries[key] = value
    if not configuring and entries.get("SCCACHE:BOOL", "ON").upper() in ("OFF", "NO", "FALSE", "0"):
        return None
    configured = entries.get("SCCACHE_PROGRAM:FILEPATH")
    if configured and configured.endswith("-NOTFOUND"):
        if not configuring:
            return None
        configured = None
    candidate = Path(configured) if configured else Path("C:/sr/tools/sccache.exe")
    if not candidate.is_absolute():
        raise ValueError("Configured sccache executable must be an absolute path")
    no_links(candidate.absolute())
    if not candidate.is_file():
        raise ValueError("Cannot identify the worker sccache executable before launching a build")
    return candidate.resolve()


def run(args):
    targets = selection(args.component)
    if args.plan:
        print(json.dumps({"components": args.component, "targets": targets,
                          "image_cases": ["retired", "submitted"] if "images" in args.component else [],
                          "scope": "component regression only; checkpoint and gameplay gates remain required"}, indent=2))
        return 0
    worker = worker_root(ROOT)
    if not worker:
        raise ValueError("Execute in a configured cloud worktree; use --plan for local selection")
    build = ROOT / args.build_dir
    if (Path(args.build_dir).is_absolute() or Path(args.build_dir).drive
            or ".." in Path(args.build_dir).parts):
        raise ValueError("Build directory must be relative inside the worktree")
    no_links(build.absolute())
    if not args.configure and not (build / "build.ninja").is_file():
        raise ValueError("Configure the build first; --configure is available on the worker")
    run_id = time.strftime("%Y%m%d_%H%M%S") + "_" + uuid.uuid4().hex[:8]
    out = ROOT / "archive" / "dev_loop" / run_id
    no_links(out.absolute())
    out.mkdir(parents=True)
    (out / "temp").mkdir()
    # One focused loop per worktree; cloud sync must still be serialized by caller.
    state = {"schema": "sr-dev-loop/1", "status": "unknown", "components": args.component,
             "targets": targets, "steps": [], "scope": "component regression; no gameplay acceptance"}
    started = time.monotonic()
    spec = {"cwd": str(ROOT), "quiet_window": str(ROOT / "archive/QUIET_WINDOW"),
            "memory_limit_mb": 49152, "process_limit": 64, "env": {"SR_BUILD_JOBS": "12"}}
    def step(name, argv, timeout, *, cached=False):
        preflight(spec, out)
        directory = out / name
        directory.mkdir()
        resolved = shutil.which(str(argv[0]))
        if not resolved:
            raise ValueError(f"Missing command: {argv[0]}")
        argv = [resolved, *map(str, argv[1:])]
        record = {"name": name, "argv": argv}
        if cached:
            # The worker configure command explicitly turns SCCACHE=ON.
            cache = cache_program(build, configuring=name == "configure")
            if cache is not None:
                record["cache"] = {"executable": str(cache), "pin": digest(cache),
                                   "endpoint": "OS-assigned private loopback port; see stdout",
                                   "idle_timeout_seconds": 0}
                argv = [sys.executable, str(ROOT / "tools/cloud/build_cached.py"),
                        "--cache-exe", str(cache), "--", *argv]
                record["argv"] = argv
        state["steps"].append(record)
        atomic(out / "receipt.json", state)
        result = run_child(argv, directory, spec, out, timeout)
        record.update(result)
        atomic(directory / "receipt.json", record)
        atomic(out / "receipt.json", state)
        print(f"{name}: exit={result['exit_code']} {result['elapsed_seconds']}s", flush=True)
        if result["exit_code"] != 0 or result["reason"] or not result["tree_empty"]:
            raise ValueError(f"{name} failed; see {directory}")
    try:
        with RunLock(ROOT / "archive/dev_loop/.active.lock"):
            state["source"] = source_identity()
            if worker and git("status", "--porcelain", "--untracked-files=no"):
                raise ValueError("Worker snapshot has tracked edits; sync the intended source first")
            if args.configure:
                sys.path.insert(0, str(ROOT / "tools/cloud"))
                import sr_cloud
                # Existing project worker configuration, encoded to avoid shell quoting.
                import base64
                script = sr_cloud.fill(sr_cloud.CONFIGURE, BUILDDIR="'" + args.build_dir.replace("'", "''") + "'", DEFINES="")
                script += "; exit $LASTEXITCODE"
                encoded = base64.b64encode(script.encode("utf-16-le")).decode("ascii")
                step("configure", ["powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded], 1200, cached=True)
            validate_build_source(build)
            build_targets = targets + (["SupermanReturns", "difftest"] if args.integration else [])
            command = ["cmake", "--build", str(build), "--parallel", "12", "--target", *build_targets]
            step("build", command, 1800, cached=True)
            state["executables"] = {t: digest(build / (t + ".exe")) for t in targets}
            for target in targets:
                if target != "sr_native_image_replay":
                    step(target, [str(build / (target + ".exe"))], 60)
            if "images" in args.component:
                for name in ("retired", "submitted"):
                    step("case_" + name, [sys.executable, str(ROOT / "tools/native_image_case.py"),
                         "--exe", str(build / "sr_native_image_replay.exe"),
                         "--case", str(ROOT / "tools/cases/native_images" / (name + ".json")),
                         "--out", str(out / ("evidence_" + name))], 45)
            if state["executables"] != {t: digest(build / (t + ".exe")) for t in targets}:
                raise ValueError("Test binaries changed during the loop")
            if source_identity() != state["source"]:
                raise ValueError("Source changed during the loop")
            state["status"] = "passed"
            return 0
    except BaseException as error:
        state.update(status="failed", error=str(error))
        raise
    finally:
        state["elapsed_seconds"] = round(time.monotonic() - started, 3)
        atomic(out / "receipt.json", state)
        print(f"Evidence: {out}", flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("component", nargs="+", choices=COMPONENTS)
    parser.add_argument("--plan", action="store_true")
    parser.add_argument("--configure", action="store_true", help="cloud: configure before building")
    parser.add_argument("--integration", action="store_true", help="cloud: also compile game and difftest")
    parser.add_argument("--build-dir", default="build")
    args = parser.parse_args(argv)
    try:
        return run(args)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"dev_loop: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
