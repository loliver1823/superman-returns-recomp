"""Windows process trees owned atomically by a kill-on-close Job Object.

Used by set_runner.py. No PID enumeration or shell command construction. A leader
exit is distinct from tree completion. Handles stay owned until close succeeds.
Design reference: REA process ownership; this is an independent ctypes adapter.
"""
from __future__ import annotations

import ctypes as C
from ctypes import wintypes as W
import os
from pathlib import Path
import subprocess
import time


class StartupInfo(C.Structure):
    _fields_ = [("cb", W.DWORD), ("reserved", W.LPWSTR), ("desktop", W.LPWSTR),
                ("title", W.LPWSTR), ("x", W.DWORD), ("y", W.DWORD),
                ("xsize", W.DWORD), ("ysize", W.DWORD), ("xchars", W.DWORD),
                ("ychars", W.DWORD), ("fill", W.DWORD), ("flags", W.DWORD),
                ("show", W.WORD), ("reserved_size", W.WORD), ("reserved2", C.c_void_p),
                ("stdin", W.HANDLE), ("stdout", W.HANDLE), ("stderr", W.HANDLE)]


class StartupInfoEx(C.Structure):
    _fields_ = [("startup", StartupInfo), ("attributes", C.c_void_p)]


class ProcessInfo(C.Structure):
    _fields_ = [("process", W.HANDLE), ("thread", W.HANDLE),
                ("pid", W.DWORD), ("tid", W.DWORD)]


class BasicLimits(C.Structure):
    _fields_ = [("process_time", C.c_longlong), ("job_time", C.c_longlong),
                ("flags", W.DWORD), ("min_working_set", C.c_size_t),
                ("max_working_set", C.c_size_t), ("active_process_limit", W.DWORD),
                ("affinity", C.c_size_t), ("priority", W.DWORD), ("scheduling", W.DWORD)]


class IoCounters(C.Structure):
    _fields_ = [(name, C.c_ulonglong) for name in
                ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]


class ExtendedLimits(C.Structure):
    _fields_ = [("basic", BasicLimits), ("io", IoCounters),
                ("process_memory", C.c_size_t), ("job_memory", C.c_size_t),
                ("peak_process_memory", C.c_size_t), ("peak_job_memory", C.c_size_t)]


class Accounting(C.Structure):
    _fields_ = [("user", C.c_longlong), ("kernel", C.c_longlong),
                ("period_user", C.c_longlong), ("period_kernel", C.c_longlong),
                ("faults", W.DWORD), ("total", W.DWORD),
                ("active", W.DWORD), ("terminated", W.DWORD)]


def _api():
    if os.name != "nt":
        raise RuntimeError("Owned process launch requires native Windows 10 or newer")
    k = C.WinDLL("kernel32", use_last_error=True)
    signatures = {
        "CreateJobObjectW": ([C.c_void_p, W.LPCWSTR], W.HANDLE),
        "SetInformationJobObject": ([W.HANDLE, C.c_int, C.c_void_p, W.DWORD], W.BOOL),
        "QueryInformationJobObject": ([W.HANDLE, C.c_int, C.c_void_p, W.DWORD, C.c_void_p], W.BOOL),
        "TerminateJobObject": ([W.HANDLE, W.UINT], W.BOOL),
        "CloseHandle": ([W.HANDLE], W.BOOL),
        "InitializeProcThreadAttributeList": ([C.c_void_p, W.DWORD, W.DWORD, C.POINTER(C.c_size_t)], W.BOOL),
        "UpdateProcThreadAttribute": ([C.c_void_p, W.DWORD, C.c_size_t, C.c_void_p, C.c_size_t, C.c_void_p, C.c_void_p], W.BOOL),
        "DeleteProcThreadAttributeList": ([C.c_void_p], None),
        "CreateProcessW": ([W.LPCWSTR, W.LPWSTR, C.c_void_p, C.c_void_p, W.BOOL,
                            W.DWORD, C.c_void_p, W.LPCWSTR, C.POINTER(StartupInfoEx), C.POINTER(ProcessInfo)], W.BOOL),
        "ResumeThread": ([W.HANDLE], W.DWORD),
        "WaitForSingleObject": ([W.HANDLE, W.DWORD], W.DWORD),
        "GetExitCodeProcess": ([W.HANDLE, C.POINTER(W.DWORD)], W.BOOL),
        "IsProcessInJob": ([W.HANDLE, W.HANDLE, C.POINTER(W.BOOL)], W.BOOL),
        "GetCurrentProcess": ([], W.HANDLE),
        "GetProcessAffinityMask": ([W.HANDLE, C.POINTER(C.c_size_t), C.POINTER(C.c_size_t)], W.BOOL),
        "SetProcessAffinityMask": ([W.HANDLE, C.c_size_t], W.BOOL),
        "GetPriorityClass": ([W.HANDLE], W.DWORD),
        "SetPriorityClass": ([W.HANDLE, W.DWORD], W.BOOL),
    }
    for name, (args, result) in signatures.items():
        getattr(k, name).argtypes, getattr(k, name).restype = args, result
    return k


def _require(ok, operation):
    if not ok:
        raise OSError(C.get_last_error(), operation)


class OwnedProcess:
    """One leader and every inherited descendant; stdout/stderr go to owned files."""
    def __init__(self, argv: list[str], cwd: str, env: dict[str, str], stdout: Path,
                 stderr: Path, *, memory_limit_mb: int = 8192, process_limit: int = 64):
        if not argv or any(not isinstance(a, str) or "\0" in a for a in argv):
            raise ValueError("argv must contain non-NUL strings")
        if type(memory_limit_mb) is not int or memory_limit_mb < 32:
            raise ValueError("memory_limit_mb must be an integer >= 32")
        if type(process_limit) is not int or not 1 <= process_limit <= 256:
            raise ValueError("process_limit must be 1..256")
        import msvcrt
        self.k = _api()
        self.job = self.process = None
        self.pid = None
        self._files = []
        self.job = self.k.CreateJobObjectW(None, None)
        _require(self.job, "CreateJobObject")
        try:
            limits = ExtendedLimits()
            # Kill on last handle close; no BREAKAWAY flags. Bound process count and commit.
            limits.basic.flags = 0x2000 | 0x8 | 0x200
            limits.basic.active_process_limit = process_limit
            limits.job_memory = memory_limit_mb * 1024 * 1024
            _require(self.k.SetInformationJobObject(self.job, 9, C.byref(limits), C.sizeof(limits)), "Set job limits")
            observed = ExtendedLimits()
            _require(self.k.QueryInformationJobObject(self.job, 9, C.byref(observed), C.sizeof(observed), None), "Read job limits")
            if observed.basic.flags != limits.basic.flags:
                raise RuntimeError("Job ownership flags differ from requested limits")
            for path, mode in ((os.devnull, "rb"), (stdout, "xb"), (stderr, "xb")):
                self._files.append(open(path, mode))
            handles = (W.HANDLE * 3)(*[msvcrt.get_osfhandle(f.fileno()) for f in self._files])
            for h in handles:
                os.set_handle_inheritable(h, True)
            size = C.c_size_t()
            self.k.InitializeProcThreadAttributeList(None, 2, 0, C.byref(size))
            attrs = C.create_string_buffer(size.value)
            _require(self.k.InitializeProcThreadAttributeList(attrs, 2, 0, C.byref(size)), "Initialize process attributes")
            try:
                jobs = (W.HANDLE * 1)(self.job)
                _require(self.k.UpdateProcThreadAttribute(attrs, 0, 0x2000D, jobs, C.sizeof(jobs), None, None), "Assign job at creation")
                _require(self.k.UpdateProcThreadAttribute(attrs, 0, 0x20002, handles, C.sizeof(handles), None, None), "Restrict inherited handles")
                si = StartupInfoEx()
                si.startup.cb = C.sizeof(si)
                si.startup.flags = 0x100 | 0x1  # USESTDHANDLES | USESHOWWINDOW
                si.startup.show = 0  # SW_HIDE; harness still owns gameplay input isolation.
                si.startup.stdin, si.startup.stdout, si.startup.stderr = handles
                si.attributes = C.cast(attrs, C.c_void_p)
                pi = ProcessInfo()
                # Windows environment names are case-insensitive. Reject ambiguous input.
                folded = set()
                for key, value in env.items():
                    if not key or "=" in key or "\0" in key or "\0" in value or key.upper() in folded:
                        raise ValueError("Invalid or duplicate environment name")
                    folded.add(key.upper())
                block = C.create_unicode_buffer("\0".join(f"{k}={v}" for k, v in sorted(env.items(), key=lambda x: x[0].upper())) + "\0\0")
                command = C.create_unicode_buffer(subprocess.list2cmdline(argv))
                _require(self.k.CreateProcessW(argv[0], command, None, None, True,
                         0x400 | 0x80000 | 0x4 | 0x08000000, block, cwd, C.byref(si), C.byref(pi)), "Launch process inside job")
                self.process, self.pid = pi.process, pi.pid
                try:
                    assigned = W.BOOL()
                    _require(self.k.IsProcessInJob(self.process, self.job, C.byref(assigned)), "Check job membership")
                    _require(assigned.value, "Child not in owned job")
                    mask, system = C.c_size_t(), C.c_size_t()
                    _require(self.k.GetProcessAffinityMask(self.k.GetCurrentProcess(), C.byref(mask), C.byref(system)), "Read caller affinity")
                    _require(self.k.SetProcessAffinityMask(self.process, mask.value), "Preserve caller affinity")
                    priority = self.k.GetPriorityClass(self.k.GetCurrentProcess())
                    _require(priority and self.k.SetPriorityClass(self.process, priority), "Preserve caller priority")
                    if self.k.ResumeThread(pi.thread) == 0xFFFFFFFF:
                        raise OSError(C.get_last_error(), "Resume child")
                finally:
                    self.k.CloseHandle(pi.thread)
            finally:
                self.k.DeleteProcThreadAttributeList(attrs)
                for h in handles:
                    os.set_handle_inheritable(h, False)
        except BaseException:
            if self.job:
                self.k.TerminateJobObject(self.job, 1)
            self.close()
            raise

    def poll(self) -> int | None:
        wait = self.k.WaitForSingleObject(self.process, 0)
        if wait == 258:
            return None
        if wait != 0:
            raise OSError(C.get_last_error(), "Read leader state")
        code = W.DWORD()
        _require(self.k.GetExitCodeProcess(self.process, C.byref(code)), "Read exit status")
        return code.value

    def active_count(self) -> int:
        info = Accounting()
        _require(self.k.QueryInformationJobObject(self.job, 1, C.byref(info), C.sizeof(info), None), "Read descendant count")
        return info.active

    def terminate(self, code: int = 130) -> None:
        _require(self.k.TerminateJobObject(self.job, code), "Terminate owned process tree")

    def settle(self, timeout: float = 10) -> bool:
        deadline = time.monotonic() + timeout
        while self.active_count():
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.05)
        return True

    def close(self) -> None:
        # Last job-handle close is the emergency kill boundary, including owner death.
        if self.job:
            _require(self.k.CloseHandle(self.job), "Close owned job")
            self.job = None
        if self.process:
            _require(self.k.CloseHandle(self.process), "Close leader handle")
            self.process = None
        for file in self._files:
            file.close()
        self._files.clear()
