"""Run a bounded JSON command set with durable evidence and Windows tree ownership.

See docs/INVESTIGATION_WORKFLOW.md for the schema and game-slot integration.
Exit 0: complete/valid; 1: command failure; 2: invalid/unknown; 130: cancelled;
5: a step exited 5 (sr_drive: another copy of the exe runs; nothing killed); 6: the stuck hold (below).
No shell interpolation, build policy changes, or gameplay acceptance is implied.

The stuck hold (Fable ruling 13, 2026-10-20): SR_STUCK_HOLD (the spec's env, else this process's) names a hold
file that the close's stuck watch writes when it catches a stuck load. It must be an ABSOLUTE path with no
{placeholder}; anything else exits 2 before any launch. While it exists before a step, the set
launches nothing more (exit 6, <run>/STOP.json). If it appears during a step, that step's timeout stops counting,
its owned tree (the held game) stays alive and the game slot stays held (heartbeats go on) until the hold file is
deleted (the probe is done: the tree is killed), a CANCEL, a quiet window, or the tree's own exit; then the set
exits 6. A step exiting 5 or 6 also stops the set (exit 5 / 6, STOP.json). The job stays kill-on-close, so a held
game never outlives this runner (or an outer bound that kills it, e.g. Invoke-AllyRun -TimeoutSec).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid

from owned_process import OwnedProcess

ROOT = Path(__file__).resolve().parents[1]
QUIET = ROOT.parent / "native" / "archive" / "v1_native" / "QUIET_WINDOW"
MAX_INPUT_BYTES = 64 * 1024**3
MAX_STATE_BYTES = 64 * 1024**2


def digest(path):
    before = path.stat()
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
        raise ValueError(f"Input changed while hashing: {path}")
    return {"sha256": h.hexdigest(), "bytes": after.st_size}


def check_output_drive(out, root):
    # The ROG and VM keep the project on C:. Their own archive is valid;
    # the desktop must still keep project evidence off its system drive.
    if out.drive.upper() == "C:" and not (
        root.drive.upper() == "C:" and out.is_relative_to(root / "archive")
    ):
        raise ValueError("Project run outputs belong on the project/archive drive, not C:")


def no_links(path):
    for item in (path, *path.parents):
        if item.exists() and (item.is_symlink() or getattr(item.lstat(), "st_file_attributes", 0) & 0x400):
            raise ValueError(f"Link/reparse path is not a pinned input/output: {item}")


def inventory(paths):
    result, total, entries = {}, 0, 0
    def walk_error(error):
        raise error
    for path in sorted(set(paths), key=str):
        no_links(path)
        if not path.exists():
            raise ValueError(f"Missing input: {path}")
        files = [path]
        if path.is_dir():
            files, directories = [], []
            for directory, dirs, names in os.walk(path, followlinks=False, onerror=walk_error):
                for name in dirs + names:
                    no_links(Path(directory) / name)
                directories.extend(str((Path(directory) / name).relative_to(path)) for name in dirs)
                files.extend(Path(directory) / name for name in sorted(names))
                if entries + len(files) + len(directories) > 100000:
                    raise ValueError("Input inventory exceeds 100000 files")
            result[str(path)] = {"directory": sorted(str(p.relative_to(path)) for p in files), "subdirectories": sorted(directories)}
            entries += len(directories)
        entries += len(files) + 1
        if entries > 100000:
            raise ValueError("Input inventory exceeds 100000 entries")
        for file in files:
            if not file.is_file():
                raise ValueError(f"Not a regular file: {file}")
            total += file.stat().st_size
            if total > MAX_INPUT_BYTES:
                raise ValueError("Input inventory exceeds 64 GiB")
            result[str(file)] = digest(file)
        if path.is_dir():
            after_files, after_dirs = [], []
            for directory, dirs, names in os.walk(path, followlinks=False, onerror=walk_error):
                after_files.extend(str((Path(directory) / name).relative_to(path)) for name in names)
                after_dirs.extend(str((Path(directory) / name).relative_to(path)) for name in dirs)
                for name in dirs + names:
                    no_links(Path(directory) / name)
            if sorted(after_files) != result[str(path)]["directory"] or sorted(after_dirs) != result[str(path)]["subdirectories"]:
                raise ValueError("Input directory changed while hashing")
    return result


def atomic(path, value):
    data = json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n"
    if len(data.encode("utf-8")) > MAX_STATE_BYTES:
        raise ValueError("Evidence JSON exceeds 64 MiB; previous receipt is preserved")
    pending = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with pending.open("x", encoding="utf-8", newline="\n") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(pending, path)
    finally:
        pending.unlink(missing_ok=True)


def read_json(path, max_bytes=MAX_STATE_BYTES):
    if path.stat().st_size > max_bytes:
        raise ValueError(f"JSON too large: {path}")
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f"Duplicate JSON key: {key}")
            result[key] = value
        return result
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=pairs,
                      parse_constant=lambda x: (_ for _ in ()).throw(ValueError(f"Invalid number: {x}")))


def fields(obj, allowed, label):
    if not isinstance(obj, dict) or set(obj) - set(allowed):
        raise ValueError(f"Invalid/unknown fields in {label}")


def number(value, low, high, name):
    if type(value) not in (int, float) or not math.isfinite(value) or not low <= value <= high:
        raise ValueError(f"{name} must be {low}..{high}")
    return value


def strings(value, name, *, empty=True):
    if not isinstance(value, list) or (not empty and not value) or any(not isinstance(x, str) or not x or "\0" in x for x in value):
        raise ValueError(f"{name} must be an array of nonempty strings")
    return value


def expand(value, out, step=None):
    for key, val in {"python": sys.executable, "repo": ROOT, "run": out, "step": step or out}.items():
        value = value.replace("{" + key + "}", str(val))
    return value


def quiet_active(spec):
    return QUIET.exists() or Path(spec["quiet_window"]).exists()


def hold_path(spec):
    """The stuck-hold file (Fable ruling 13): the spec's env SR_STUCK_HOLD, else this process's; None when unset.
    One value serves both sides: the child gets this exact string and the runner watches it. So it must be an
    absolute path with no {placeholder} (run_child expands those for the child only) and no surrounding blanks;
    anything else - relative, drive-relative, {run}/H - is refused before any launch (Astra on stuck-hold-set, P1:
    the child would hold on one file while the runner watched another, timed out and killed the held game)."""
    value = next((v for k, v in spec.get("env", {}).items() if k.upper() == "SR_STUCK_HOLD"), None)
    if value is None:
        value = os.environ.get("SR_STUCK_HOLD", "")
    if not value.strip():
        return None
    if "{" in value or "}" in value or value != value.strip() or not Path(value).is_absolute():
        raise ValueError(f"SR_STUCK_HOLD must be an absolute path with no placeholders: {value!r} "
                         "(the child and the runner must watch the same file)")
    return Path(value)


def stop_record(out, state, code, reason, step=None, **extra):
    """<run>/STOP.json (the latest stop) and state["stops"] (every stop of this run folder)."""
    record = {"schema": "set-runner-stop/1", "status": "stopped", "exit": code, "reason": reason, "step": step,
              "hold_file": state.get("stuck_hold"), "at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
              "not_launched": [s for s in state.get("_pending", []) if s != step], **extra}
    state.setdefault("stops", []).append(record)
    atomic(out / "STOP.json", record)
    return record


def absolute(value, base, out):
    if not isinstance(value, str) or not value or "\0" in value:
        raise ValueError("Paths must be nonempty strings")
    path = Path(expand(value, out))
    path = path if path.is_absolute() else base / path
    no_links(path.absolute())
    return path.resolve()


def command(argv, cwd, out, step):
    result = [expand(x, out, step) for x in argv]
    exe = Path(result[0])
    resolved = str((cwd / exe).resolve()) if not exe.is_absolute() and (cwd / exe).is_file() else shutil.which(result[0])
    if not resolved or not Path(resolved).is_file() or Path(resolved).suffix.lower() not in (".exe", ".com"):
        raise ValueError(f"Command must resolve to a native executable: {result[0]}")
    result[0] = str(Path(resolved).resolve())
    return result


def validate(raw, base, out):
    fields(raw, ("version", "cwd", "inputs", "env", "steps", "game_slot", "quiet_window", "memory_limit_mb", "process_limit"), "spec")
    if raw.get("version") != 1 or type(raw.get("version")) is not int:
        raise ValueError("version must be 1")
    cwd = absolute(raw.get("cwd", "{repo}"), base, out)
    if not cwd.is_dir():
        raise ValueError("cwd must exist")
    env = raw.get("env", {})
    if not isinstance(env, dict) or any(not isinstance(k, str) or not k or "=" in k or "\0" in k or not isinstance(v, str) or "\0" in v for k, v in env.items()) or len({k.upper() for k in env}) != len(env):
        raise ValueError("env must contain unambiguous string values")
    for name, low, high, default in (("memory_limit_mb",32,1048576,8192),("process_limit",1,256,64)):
        if type(raw.get(name, default)) is not int:
            raise ValueError(f"{name} must be an integer")
        number(raw.get(name, default), low, high, name)
    steps = raw.get("steps")
    if not isinstance(steps, list) or not 1 <= len(steps) <= 1000:
        raise ValueError("steps must contain 1..1000 commands")
    names, normalized_steps = set(), []
    inputs = [absolute(x, base, out) for x in strings(raw.get("inputs", []), "inputs")]
    inputs.extend((Path(sys.executable), Path(__file__).resolve(), ROOT / "tools" / "owned_process.py"))
    for step in steps:
        fields(step, ("id", "argv", "timeout_seconds", "outputs", "cancel_argv", "grace_seconds", "reuse", "max_age_seconds"), "step")
        step = dict(step)
        name = step.get("id")
        if not isinstance(name, str) or not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}", name) or name.lower() in names:
            raise ValueError("Step ids must be unique Windows-safe identifiers")
        if name.upper().split(".")[0] in {"CON","PRN","AUX","NUL",*(f"COM{i}" for i in range(1,10)),*(f"LPT{i}" for i in range(1,10))}:
            raise ValueError("Reserved step id")
        names.add(name.lower())
        for key in ("argv", "cancel_argv"):
            if key not in step and key == "cancel_argv":
                continue
            argv = command(strings(step.get(key), key, empty=False), cwd, out, out / "placeholder")
            inputs.append(Path(argv[0]))
            # Keep step/run placeholders, but never resolve the executable through PATH again.
            step[key] = [argv[0], *step[key][1:]]
            if len(argv) > 1 and Path(argv[1]).suffix.lower() in (".py", ".ps1"):
                inputs.append(absolute(argv[1], cwd, out))
        number(step.get("timeout_seconds"), .1, 86400, "timeout_seconds")
        number(step.get("grace_seconds", 0), 0, 30, "grace_seconds")
        if step.get("reuse", "verified") not in ("verified", "never"):
            raise ValueError("reuse must be verified or never")
        if "max_age_seconds" in step:
            number(step["max_age_seconds"], .1, 2592000, "max_age_seconds")
        outputs = strings(step.get("outputs", []), "outputs")
        for name in outputs:
            p = Path(name)
            if p.is_absolute() or p.drive or ":" in name or ".." in p.parts or "{" in name or "}" in name or name.endswith((".", " ")):
                raise ValueError("outputs must be plain relative paths inside the step directory")
            if not p.parts or p.parts[0].lower() in ("receipt.json", "stdout.txt", "stderr.txt", "cancel"):
                raise ValueError("Output name collides with runner evidence")
        normalized_steps.append(step)
    slot = raw.get("game_slot")
    if slot is not None:
        fields(slot, ("owner", "wait_seconds", "min_free_gb"), "game_slot")
        if not re.fullmatch(r"[a-zA-Z0-9_-]{1,64}", slot.get("owner", "")):
            raise ValueError("game_slot.owner must be a safe identifier")
        number(slot.get("wait_seconds", 300), 1, 86400, "slot wait_seconds")
        number(slot.get("min_free_gb", 8), 1, 1024, "min_free_gb")
        inputs.append(ROOT / "tools" / "game_slot.py")
    hold_path(dict(env=env))   # refuse a hold the child and the runner would read differently (before any launch)
    quiet = absolute(raw.get("quiet_window", str(QUIET)), base, out)
    for path in inputs:
        if path == out or path in out.parents or out in path.parents:
            raise ValueError("Inputs and output directory must not overlap")
    normalized = dict(raw, steps=normalized_steps, cwd=str(cwd), inputs=[str(x) for x in sorted(set(inputs), key=str)], quiet_window=str(quiet))
    return normalized


class RunLock:
    def __init__(self, path):
        self.path, self.file = path, None
    def __enter__(self):
        import msvcrt
        self.file = self.path.open("a+b")
        self.file.seek(0)
        if not self.file.read(1):
            self.file.write(b"0")
            self.file.flush()
        self.file.seek(0)
        try:
            msvcrt.locking(self.file.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            self.file.close()
            raise ValueError("Run already has an active owner") from None
        return self
    def __exit__(self, *exc):
        import msvcrt
        self.file.seek(0)
        msvcrt.locking(self.file.fileno(), msvcrt.LK_UNLCK, 1)
        self.file.close()


def run_child(argv, directory, spec, out, timeout, *, cancellation=True, heartbeat=None, hold=None, on_hold=None):
    env = dict(os.environ)
    for key, value in spec.get("env", {}).items():
        for old in list(env):
            if old.upper() == key.upper():
                del env[old]
        env[key] = expand(value, out, directory)
    env.update(TEMP=str(out / "temp"), TMP=str(out / "temp"))
    child = OwnedProcess(argv, spec["cwd"], env, directory / "stdout.txt", directory / "stderr.txt",
                         memory_limit_mb=spec.get("memory_limit_mb", 8192), process_limit=spec.get("process_limit",64))
    reason, clean, held = None, False, {}
    started = time.monotonic()
    try:
        while child.active_count():
            if cancellation and ((out / "CANCEL").exists() or quiet_active(spec)):
                reason = "cancelled" if (out / "CANCEL").exists() else "quiet_window"
                break
            if hold is not None and hold.exists():
                reason = "held"
                break
            if time.monotonic() - started >= timeout:
                reason = "timeout"
                break
            if heartbeat:
                heartbeat()
            time.sleep(.05)
        if reason == "held":
            # The stuck hold: no timeout, nothing killed, the slot kept, until the hold ends (docstring).
            held_at = time.monotonic()
            held.update(held_after_seconds=round(held_at - started, 3), hold_end="tree_exited")
            if on_hold:
                on_hold(child.pid, held["held_after_seconds"])
            while child.active_count():
                if (out / "CANCEL").exists() or quiet_active(spec):
                    held["hold_end"] = "cancelled" if (out / "CANCEL").exists() else "quiet_window"
                    break
                if not hold.exists():
                    held["hold_end"] = "hold_cleared"
                    break
                if heartbeat:
                    heartbeat()
                time.sleep(.25)
            held["held_seconds"] = round(time.monotonic() - held_at, 3)
            if child.active_count():
                child.terminate(6)
        elif reason:
            # A cancel helper is itself owned and bounded. Never shell-interpolate.
            step = spec.get("_step", {})
            if step.get("cancel_argv") and step.get("grace_seconds", 0) > 0:
                grace_deadline = time.monotonic() + step["grace_seconds"]
                stopdir = directory / "cancel"
                stopdir.mkdir()
                stopped = run_child(command(step["cancel_argv"], Path(spec["cwd"]), out, directory), stopdir,
                                    dict(spec, _step={}), out, step["grace_seconds"], cancellation=False)
                atomic(stopdir / "receipt.json", stopped)
                while child.active_count() and time.monotonic() < grace_deadline:
                    time.sleep(.05)
            if child.active_count():
                child.terminate(130 if reason == "cancelled" else 124)
        clean = child.settle(10)
        if not clean:
            raise RuntimeError("Owned process tree did not settle; game slot retained")
        return {"pid": child.pid, "exit_code": child.poll(), "reason": reason, "tree_empty": True,
                "elapsed_seconds": round(time.monotonic() - started, 3), **held}
    finally:
        try:
            if not clean:
                child.terminate(1)
                clean = child.settle(10)
        finally:
            child.close()
        if not clean:
            raise RuntimeError("Process cleanup unverified; game slot retained")


def output_inventory(directory, outputs):
    paths = [directory / "stdout.txt", directory / "stderr.txt"] + [directory / x for x in outputs]
    for path in paths:
        no_links(path)
        if directory.resolve() not in path.resolve().parents:
            raise ValueError("Output escaped attempt directory")
    return inventory(paths)


def execute(out, resume):
    with RunLock(out / ".run.lock"):
        spec = read_json(out / "spec.json")
        state = read_json(out / "state.json")
        if digest(out / "spec.json") != state["spec_digest"]:
            raise ValueError("Saved specification changed")
        if inventory([Path(x) for x in spec["inputs"]]) != state["inputs"]:
            raise ValueError("Pinned inputs changed; create a fresh run")
        if state.get("slot_state") in ("acquiring", "held", "unknown"):
            # No PID killing or implicit stale-lock release during resume. The old
            # owner may have died between the external lock write and its receipt.
            raise ValueError("Previous game-slot lifecycle is unresolved; inspect saved slot_owner and game_slot status, then recover through game_slot and start a fresh run")
        if (out / "CANCEL").exists():
            if resume:
                (out / "CANCEL").unlink()
            else:
                raise ValueError("Cancellation requested before start")
        if quiet_active(spec):
            raise ValueError("Quiet window is active; no commands launched")
        hold = hold_path(spec)
        state["stuck_hold"] = str(hold) if hold else None
        if hold and hold.exists():
            state["status"] = "held"
            state["_pending"] = [s["id"] for s in spec["steps"]]
            stop_record(out, state, 6, "stuck_hold_present_before_start")
            state.pop("_pending", None)
            atomic(out / "state.json", state)
            print(f"set_runner: SR_STUCK_HOLD {hold} exists - a stuck game is held; nothing launched (exit 6)", file=sys.stderr)
            return 6
        reusable, prior_invalidated = set(), False
        for step in spec["steps"]:
            old = state["steps"].get(step["id"])
            if old and old["status"] == "passed":
                if output_inventory(Path(old["directory"]), step.get("outputs", [])) != old["outputs"]:
                    raise ValueError(f"Saved outputs changed for {step['id']}")
                age = time.time() - old.get("completed_at", 0)
                if (not prior_invalidated and step.get("reuse", "never" if spec.get("game_slot") else "verified") == "verified"
                        and 0 <= age <= step.get("max_age_seconds", float("inf"))):
                    reusable.add(step["id"])
                    continue
            prior_invalidated = True
        if len(reusable) == len(spec["steps"]):
            state["status"] = "passed"
            atomic(out / "state.json", state)
            return 0
        slot = spec.get("game_slot")
        held, cleanup_verified = False, True
        # Unique actual owner avoids reentrant acquisition by another run with the same label.
        owner = (slot["owner"] + "-" + uuid.uuid4().hex[:12]) if slot else None
        state["slot_owner"] = owner
        state["slot_state"] = "not_requested" if not slot else "pending"
        def slot_command(action, extra=()):
            directory = out / ("slot-" + action + "-" + uuid.uuid4().hex[:12])
            directory.mkdir()
            result = run_child([sys.executable, str(ROOT / "tools" / "game_slot.py"), action, "--owner", owner, *extra],
                               directory, dict(spec, _step={}), out,
                               slot.get("wait_seconds",300) + 5 if action == "acquire" else 10,
                               cancellation=action == "acquire")
            atomic(directory / "receipt.json", result)
            return result
        try:
            if slot:
                # Recovery after owner death requires game_slot to inspect its real lock/process state.
                state.update(slot_state="acquiring", slot_retained=True)
                atomic(out / "state.json", state)
                acquired = slot_command("acquire", ["--poll", ".5", "--max-hours", str(slot.get("wait_seconds",300)/3600),
                                                    "--min-free-gb", str(slot.get("min_free_gb",8)), "--note", str(out)])
                held = acquired["exit_code"] == 0 and acquired["reason"] is None
                if not held:
                    state["slot_state"] = "unknown"
                    raise ValueError("Game slot acquisition failed; see slot logs. Existing lock was not forced.")
                state["slot_state"] = "held"
            state.update(status="running", owner_pid=os.getpid())
            atomic(out / "state.json", state)
            last_heartbeat = time.monotonic()
            def heartbeat():
                nonlocal last_heartbeat
                if held and time.monotonic() - last_heartbeat >= 20:
                    result = slot_command("heartbeat")
                    if result["exit_code"] != 0 or result["reason"]:
                        raise RuntimeError("Game slot heartbeat failed")
                    last_heartbeat = time.monotonic()
            for index, step in enumerate(spec["steps"]):
                old = state["steps"].get(step["id"])
                if step["id"] in reusable:
                    continue
                if (out / "CANCEL").exists() or quiet_active(spec):
                    state["status"] = "cancelled"
                    return 130
                state["_pending"] = [s["id"] for s in spec["steps"][index:] if s["id"] not in reusable]
                if hold and hold.exists():
                    state["status"] = "held"
                    stop_record(out, state, 6, "stuck_hold_present_before_step", None, next_step=step["id"])
                    print(f"set_runner: SR_STUCK_HOLD {hold} exists before step {step['id']}; nothing more launched (exit 6)", file=sys.stderr)
                    return 6
                if inventory([Path(x) for x in spec["inputs"]]) != state["inputs"]:
                    raise ValueError("Pinned inputs changed during run")
                attempt = (old.get("attempt", 0) if old else 0) + 1
                directory = out / step["id"] / f"attempt-{attempt:04d}"
                directory.mkdir(parents=True)
                argv = command(step["argv"], Path(spec["cwd"]), out, directory)
                record = {"status": "running", "attempt": attempt, "directory": str(directory), "argv": argv}
                state["steps"][step["id"]] = record
                atomic(out / "state.json", state)
                cleanup_verified = False
                def on_hold(pid, after, step_id=step["id"], step_dir=directory):
                    # a live record while the game is held: the operator sees the set is holding, not hung
                    atomic(out / "STOP.json", {"schema": "set-runner-stop/1", "status": "holding", "step": step_id,
                                               "directory": str(step_dir), "leader_pid": pid, "held_after_seconds": after,
                                               "hold_file": state.get("stuck_hold"),
                                               "at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                                               "ends_on": "the hold file deleted (the tree is then killed), CANCEL, "
                                                          "a quiet window, or the tree's own exit"})
                try:
                    result = run_child(argv, directory, dict(spec, _step=step), out, step["timeout_seconds"], heartbeat=heartbeat,
                                       hold=hold, on_hold=on_hold)
                    cleanup_verified = result["tree_empty"]
                    record.update(result)
                    record["status"] = "failed"
                    record["outputs"] = output_inventory(directory, step.get("outputs", [])) if result["exit_code"] == 0 and not result["reason"] else inventory([directory / "stdout.txt", directory / "stderr.txt"])
                    if result["exit_code"] == 0 and not result["reason"]:
                        if inventory([Path(x) for x in spec["inputs"]]) != state["inputs"]:
                            raise ValueError("Pinned inputs changed during command")
                        record["status"] = "passed"
                except BaseException as exc:
                    record.update(status="unknown", error=str(exc))
                    raise
                finally:
                    record["completed_at"] = time.time()
                    atomic(directory / "receipt.json", record)
                atomic(out / "state.json", state)
                if record["status"] != "passed":
                    code = result["exit_code"]
                    if result["reason"] == "held" or code in (5, 6):
                        stop = 6 if result["reason"] == "held" or code == 6 else 5
                        state["status"] = "held" if stop == 6 else "refused"
                        why = "stuck_hold_during_step" if result["reason"] == "held" else "step_exit_%d" % code
                        stop_record(out, state, stop, why, step["id"], step_exit=code, directory=str(directory),
                                    **{k: result[k] for k in ("hold_end", "held_seconds", "held_after_seconds") if k in result})
                        print(f"set_runner: step {step['id']} stopped the set ({why}, exit {stop})", file=sys.stderr)
                        return stop
                    state["status"] = result["reason"] or "failed"
                    return 130 if result["reason"] in ("cancelled", "quiet_window") else 1
            state["status"] = "passed"
            return 0
        except BaseException as exc:
            state.update(status="unknown", error=str(exc))
            raise
        finally:
            if held and cleanup_verified:
                try:
                    released = slot_command("release")
                    if released["exit_code"] != 0 or released["reason"]:
                        raise RuntimeError("Game slot release failed")
                    state["slot_state"] = "released"
                except BaseException as exc:
                    state.update(status="unknown", slot_state="unknown", slot_retained=True, error=str(exc))
                    atomic(out / "state.json", state)
                    raise
            if held and not cleanup_verified:
                state["slot_state"] = "unknown"
            state["slot_retained"] = state["slot_state"] in ("acquiring", "held", "unknown")
            state.pop("_pending", None)
            atomic(out / "state.json", state)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("validate", "run"):
        p = sub.add_parser(name)
        p.add_argument("--spec", type=Path, required=True)
        p.add_argument("--out", type=Path, required=True)
    for name in ("resume", "cancel", "status"):
        sub.add_parser(name).add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        no_links(args.out.absolute())
        out = args.out.resolve()
        if args.command in ("validate", "run"):
            if out.exists():
                raise ValueError("Output must be a new directory")
            check_output_drive(out, ROOT)
            spec = validate(read_json(args.spec, max_bytes=4 * 1024**2), args.spec.resolve().parent, out)
            pins = inventory([Path(x) for x in spec["inputs"]])
            if args.command == "validate":
                print(json.dumps({"valid":True,"steps":len(spec["steps"]),"pinned_entries":len(pins)}))
                return 0
            out.mkdir(parents=True)
            (out / "temp").mkdir()
            atomic(out / "spec.json", spec)
            atomic(out / "state.json", {"version":1,"run_id":uuid.uuid4().hex,"status":"pending",
                                       "spec_digest":digest(out / "spec.json"),"inputs":pins,"steps":{}})
            return execute(out, False)
        if args.command == "resume":
            return execute(out, True)
        if args.command == "cancel":
            if not (out / "state.json").is_file():
                raise ValueError("Not a run directory")
            with (out / "CANCEL").open("a", encoding="utf-8") as f:
                f.write("cancel requested\n")
            print("Cancellation requested; status remains pending until owned-tree cleanup completes")
            return 0
        print(json.dumps(read_json(out / "state.json"), indent=2))
        return 0
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as exc:
        print(f"set_runner: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
