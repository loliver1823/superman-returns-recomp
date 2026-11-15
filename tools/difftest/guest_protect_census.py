#!/usr/bin/env python3
"""Census of host memory protection, mapping and release in the exe's build inputs (2026-10-14; rewritten after
Astra's round A review of the transport pair's arithmetic admission, tightened after rounds B and C).

    python tools/difftest/guest_protect_census.py --build build [--target SupermanReturns.exe] [--report]

The transport pair (kernel/native_transport.cpp) admits guest stores by arithmetic. That is only sound while
  - the guest arena stays one committed read-write allocation for as long as a native call may touch it (its release
    goes through kernel/guest_context.cpp releaseGuestArena, which retires arithmetic admission first), and
  - only the GPU write tracker makes arena pages non-writable without declaring it to kernel/guest_protect.hpp.
This census checks that over the program as linked, not a hand-picked tree. It is textual: it neither preprocesses
nor parses C++. It enforces the rules below and reports every form they name but cannot resolve as a problem:
  - Inputs: `ninja -t inputs <target>` (every source, object and library the exe is built from, recursively:
    kernel, gfx, app, the generated guest runtime guest/ppc, imgui, the in-tree FFmpeg) plus, for every item with a
    deps entry, its compiler-recorded dependencies from `ninja -t deps` - every one, whatever its suffix. Toolchain and
    SDK files (under Program Files / Windows Kits) only declare the APIs and are skipped (counted). The target must be
    built first: an object without a valid deps entry is a problem.
  - Binaries: every input that is not text (a NUL byte in its first 64 KB) or does not exist, and every library,
    delay-load DLL and /DEF file named by a link-affecting form below, must be classified in BINARIES with a reason.
    One classified as IN_TREE must be an output of this build graph (`ninja -t query`), so its inputs are scanned.
  - Link-affecting forms: link statements' link variables token by token (link_statement); every other statement the
    target is built from, every rule command and every file-level variable as text (link_text_flags). A linker flag
    other than /DEFAULTLIB, /DELAYLOAD, /DEF and BENIGN_LINKER, a link-line driver flag outside DRIVER_OK, an -Xclang
    value other than --dependent-lib and XCLANG_ARG / XCLANG_OK, --linker-option, -fms-runtime-lib, /link and a
    statement whose rule has no command are problems.
  - Pragmas (pragma_forms): comment(lib|linker, ...) in #pragma, __pragma and _Pragma needs a plain literal sequence
    (joined before it is classified); a non-literal _Pragma or a __pragma outside PRAGMA_NAMES is a problem except in
    the body of a classified wrapper (PRAGMA_WRAPPERS), whose every use must pass a pragma in PRAGMA_NAMES.
  - Sites: every reference to a protection, allocation, mapping, section or release API, a symbol resolver
    (GetProcAddress and its kin, whatever its argument) or a watch entry point (API below) - a call, a declaration,
    an address taken, a pointer type, a string naming one - in code with line splices joined and comments removed
    (string literals kept, adjacent literals joined as the compiler joins them, so "Virtual" /* x */ "Protect" counts).
  - Classification: ALLOW maps (file, outermost enclosing function, token) to (exact count, reason). A reference
    with no entry, or a count that differs, is a problem: a new site, a moved site or an indirect use is classified.
  - Funnels: in its own file, each function that changes protection of, or releases, guest memory must have exactly
    one definition as funnel_forms recognises one (name, parameters, the QUALIFIERS, brace); the name in a
    preprocessor directive or in any other file-scope form is a problem. The definition must sit outside any
    preprocessor conditional but a _WIN32 guard and make its declaration (guest_protect declare) or retirement
    (retireArena) its first effective statement - unconditionally, with no preprocessor directive and no API
    reference before it (only void casts, namespace aliases and guard returns may precede it). A same-named function
    in another file is not a funnel; its API references are sites like any other.
  - Gates: the diagnostic gates kernel/guest_protect.hpp lists must still be read by their owners.
Not covered: the implementation inside the binaries BINARIES classifies as external (each reason says why the guest
arena never reaches it); toolchain / SDK headers (declarations, skipped by path); a raw system call made without
naming any API or resolver above; a name assembled by token pasting or at run time; macro expansion in general
(inactive #if branches are scanned like active ones, which can only add sites); linker directives a prebuilt object
or library carries, or the compiler adds by means other than the flags above."""
import argparse
import functools
import posixpath
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEPS = 'C:/sr/deps'  # tools/cloud/sr_cloud.py DEPS: the shared submodule checkouts (FFmpeg, XenonRecomp, ...)
PLATFORM = re.compile(r'(?i)/(?:program files(?: \(x86\))?|windows kits)/')
API = re.compile(r'Virtual(?:Protect|Free|Alloc)\w*|(?:Nt|Zw)(?:Protect|Free|Allocate)VirtualMemory\w*'
                 r'|(?:Nt|Zw)(?:MapViewOfSection|UnmapViewOfSection|CreateSection)\w*|CreateFileMapping\w*'
                 r'|(?:Map|Unmap)ViewOfFile\w*|GetProcAddress\w*|LdrGetProcedureAddress\w*|\bdlsym\b'
                 r'|armRuntimeWriteWatch|disarmRuntimeWriteWatch|installImageWriteWatchdog'
                 r'|startPeriodicImageRestore|protectRange|releaseGuestArena|freeGuestMemory')
KEYWORDS = {'if', 'for', 'while', 'switch', 'catch', 'sizeof', 'return', 'decltype', 'alignof', 'static_assert',
            'defined', '__attribute__', '__declspec', 'noexcept', 'throw', 'new', 'delete', 'operator'}

# (file, outermost enclosing function or <file>, token) -> (references, reason). Filled from the --report of the
# exe's inputs (worker, 2026-10-14); every count is exact. '<file>' is file scope (declarations, a definition's own
# signature); '*' (generated guest code only) counts every function of the file together.
OWN = "the program's own reservation or mapping, never the guest arena"
DECL = 'a declaration or the definition signature'
GUEST_IMPORT = ('recompiled guest code calls the kernel import thunk; kernel/stubs.cpp serves it on the guest heap '
                '(bookkeeping and zeroing in guest memory, no host protection change, allocation or release)')
WATCH = 'the image watchdog runtime arm / disarm (protection through wdProtect, which declares first)'
ALLOW = {
    ('app/log.cpp', 'init', 'VirtualAlloc'): (3, OWN + ': asynchronous log ring and staging buffers, nullptr allocations'),
    ('app/log.cpp', 'threadName', 'GetProcAddress'): (1, 'kernel32 GetThreadDescription: reads a host thread name'),
    ('app/log.cpp', 'writerMain', 'GetProcAddress'): (1, 'kernel32 SetThreadDescription: names the host log writer'),
    ('app/main.cpp', 'SDL_main', 'VirtualAlloc'): (2, OWN + ': the guest mirror reserve and the 4 GB low guard page'),
    ('app/main.cpp', 'publishPlayerPid', 'MapViewOfFile'): (1, OWN + ': the player-pid named mapping'),
    ('app/main.cpp', 'publishPlayerPid', 'UnmapViewOfFile'): (1, OWN + ': the player-pid named mapping'),
    ('app/main.cpp', 'readPlayerPid', 'MapViewOfFile'): (1, OWN + ': the player-pid named mapping'),
    ('app/main.cpp', 'readPlayerPid', 'UnmapViewOfFile'): (1, OWN + ': the player-pid named mapping'),
    ('deps/FFmpeg/libavutil/file.c', 'av_file_map', 'MapViewOfFile'): (2, OWN + ': FFmpeg maps a file read-only'),
    ('deps/FFmpeg/libavutil/file.c', 'av_file_unmap', 'UnmapViewOfFile'): (1, OWN + ': FFmpeg unmaps its file view'),
    ('gfx/gpu_trace_replay.cpp', 'runGpuTraceReplay', 'VirtualAlloc'):
        (2, 'the replay arena: one MEM_RESERVE|MEM_COMMIT read-write allocation, registered, never protected or freed'),
    ('guest/guest_harness.cpp', '<file>', 'freeGuestMemory'): (1, DECL),
    ('guest/guest_harness.cpp', '<file>', 'installImageWriteWatchdog'): (1, DECL),
    ('guest/guest_harness.cpp', '<file>', 'startPeriodicImageRestore'): (1, DECL),
    ('guest/guest_harness.cpp', 'allocGuestMemory', 'VirtualAlloc'):
        (2, 'the guest arena (flat model): one MEM_RESERVE|MEM_COMMIT read-write allocation'),
    ('guest/guest_harness.cpp', 'allocGuestMemoryUnified', 'MapViewOfFile3'):
        (7, 'the guest arena (unified model): read-write views placed before the arena is registered'),
    ('guest/guest_harness.cpp', 'allocGuestMemoryUnified', 'UnmapViewOfFile'):
        (1, 'the unified model setup failing: before the arena is registered'),
    ('guest/guest_harness.cpp', 'allocGuestMemoryUnified', 'VirtualAlloc2'):
        (8, 'the guest arena (unified model): placeholder reserve and read-write commits before it is registered'),
    ('guest/guest_harness.cpp', 'allocGuestMemoryUnified', 'VirtualFree'):
        (3, 'the unified model setup: placeholder splits and failure release, before the arena is registered'),
    ('guest/guest_harness.cpp', 'freeGuestMemory', 'releaseGuestArena'):
        (1, 'the arena release goes through releaseGuestArena (retires arithmetic admission first)'),
    ('guest/guest_harness.cpp', 'mirrorDemandVeh', 'VirtualAlloc'): (1, OWN + ': a guest-mirror chunk (lazy mirror)'),
    ('guest/guest_harness.cpp', 'runGuestHarness', 'VirtualAlloc'): (1, OWN + ': the guest mirror commit'),
    ('guest/guest_harness.cpp', 'runGuestHarness', 'freeGuestMemory'): (4, 'teardown: the guest returned or faulted'),
    ('guest/guest_harness.cpp', 'runGuestHarness', 'installImageWriteWatchdog'):
        (1, 'boot, before guest code runs: the boot page is a declared range'),
    ('guest/guest_harness.cpp', 'runGuestHarness', 'startPeriodicImageRestore'):
        (1, 'forcing-gated restore; re-protects the boot page through wdProtect'),
    ('guest/ppc/ppc_func_mapping.cpp', '<file>', 'NtAllocateVirtualMemory'): (1, GUEST_IMPORT),
    ('guest/ppc/ppc_func_mapping.cpp', '<file>', 'NtFreeVirtualMemory'): (1, GUEST_IMPORT),
    ('guest/ppc/ppc_recomp_shared.h', '<file>', 'NtAllocateVirtualMemory'): (1, GUEST_IMPORT),
    ('guest/ppc/ppc_recomp_shared.h', '<file>', 'NtFreeVirtualMemory'): (1, GUEST_IMPORT),
    ('guest/ppc_import_glue.cpp', '<file>', 'NtAllocateVirtualMemory'): (1, GUEST_IMPORT),
    ('guest/ppc_import_glue.cpp', '<file>', 'NtFreeVirtualMemory'): (1, GUEST_IMPORT),
    ('guest/ppc_import_glue.cpp', 'PPC_FUNC(__imp__NtAllocateVirtualMemory)', 'NtAllocateVirtualMemory'):
        (1, GUEST_IMPORT),
    ('guest/ppc_import_glue.cpp', 'PPC_FUNC(__imp__NtFreeVirtualMemory)', 'NtFreeVirtualMemory'): (1, GUEST_IMPORT),
    ('guest/ppc/ppc_recomp.123.cpp', '*', 'NtAllocateVirtualMemory'): (1, GUEST_IMPORT),
    ('guest/ppc/ppc_recomp.123.cpp', '*', 'NtFreeVirtualMemory'): (1, GUEST_IMPORT),
    ('guest/ppc/ppc_recomp.57.cpp', '*', 'NtAllocateVirtualMemory'): (14, GUEST_IMPORT),
    ('guest/ppc/ppc_recomp.57.cpp', '*', 'NtFreeVirtualMemory'): (9, GUEST_IMPORT),
    ('kernel/audio_cmdq_obs.cpp', '<file>', 'protectRange'): (1, DECL),
    ('kernel/audio_cmdq_obs.cpp', 'protectRange', 'VirtualProtect'): (1, 'funnel: declares (excluding all) first'),
    ('kernel/audio_cmdq_obs.cpp', 'watcherMain', 'protectRange'): (1, 'SR_PROTECT_ZERO (a diagnostic gate)'),
    ('kernel/audio_cmdq_obs.hpp', '<file>', 'protectRange'): (1, DECL),
    ('kernel/cs_timeout_underflow_fix.cpp', '<file>', 'armRuntimeWriteWatch'): (2, DECL),
    ('kernel/cs_timeout_underflow_fix.cpp', '<file>', 'disarmRuntimeWriteWatch'): (1, DECL),
    ('kernel/cs_timeout_underflow_fix.cpp', 'sub_82486820', 'armRuntimeWriteWatch'): (1, WATCH + '; SR_XMV_WATCH'),
    ('kernel/cs_timeout_underflow_fix.cpp', 'sub_82486820', 'disarmRuntimeWriteWatch'): (1, WATCH + '; SR_XMV_WATCH'),
    ('kernel/cs_timeout_underflow_fix.cpp', 'sub_824925D8', 'armRuntimeWriteWatch'):
        (1, WATCH + '; SR_XMV_EXTRA_WATCH'),
    ('kernel/gpu_ring_alloc_fix.cpp', '<file>', 'armRuntimeWriteWatch'): (1, DECL),
    ('kernel/gpu_ring_alloc_fix.cpp', '<file>', 'disarmRuntimeWriteWatch'): (1, DECL),
    ('kernel/gpu_ring_alloc_fix.cpp', 'runAllocWatchProbe', 'armRuntimeWriteWatch'): (1, WATCH + '; SR_ALLOC_WATCH'),
    ('kernel/gpu_ring_alloc_fix.cpp', 'runAllocWatchProbe', 'disarmRuntimeWriteWatch'):
        (1, WATCH + '; SR_ALLOC_WATCH'),
    ('kernel/gpu_write_watch.cpp', 'protectForValidation', 'VirtualProtect'): (1, 'log text'),
    ('kernel/gpu_write_watch.cpp', 'protectRun', 'VirtualProtect'):
        (2, 'the GPU write tracker: PAGE_READONLY on the 0x8 / 0xA windows, released by its own handler on any store'),
    ('kernel/native_hardware_memory.cpp', 'protectHardwareReservation', 'VirtualProtect'):
        (1, 'funnel: declares the hardware reservation before PAGE_NOACCESS, before guest execution'),
    ('kernel/gpu_hang_wd.cpp', 'writeDumpFiles', 'GetProcAddress'):
        (1, 'kernel32 SetThreadDescription: names the host dump writer'),
    ('kernel/guest_context.cpp', '<file>', 'releaseGuestArena'): (1, DECL),
    ('kernel/guest_context.cpp', 'releaseGuestArena', 'VirtualFree'): (1, 'funnel: retires the arena first'),
    ('kernel/guest_protect.hpp', '<file>', 'releaseGuestArena'): (1, DECL),
    ('kernel/harness.cpp', 'runHarness', 'NtAllocateVirtualMemory'):
        (1, 'the kernel self-test calls the guest export by name (kernel/stubs.cpp; no host call)'),
    ('kernel/host_automation.cpp', 'initAutomation', 'MapViewOfFile'): (1, OWN + ': the automation named mapping'),
    ('kernel/image_write_watchdog.cpp', '<file>', 'armRuntimeWriteWatch'): (1, DECL),
    ('kernel/image_write_watchdog.cpp', '<file>', 'disarmRuntimeWriteWatch'): (1, DECL),
    ('kernel/image_write_watchdog.cpp', '<file>', 'installImageWriteWatchdog'): (1, DECL),
    ('kernel/image_write_watchdog.cpp', '<file>', 'startPeriodicImageRestore'): (1, DECL),
    ('kernel/image_write_watchdog.cpp', 'installImageWriteWatchdog', 'VirtualProtect'): (1, 'log text'),
    ('kernel/image_write_watchdog.cpp', 'wdProtect', 'VirtualProtect'): (1, 'funnel: declares first'),
    ('kernel/native_flush.cpp', 'draw', 'VirtualAlloc'): (1, OWN + ': a native record arena (read-write)'),
    ('kernel/native_flush.cpp', 'flushSegmentless', 'VirtualAlloc'): (1, OWN + ': a native record arena (read-write)'),
    ('kernel/native_flush.cpp', 'open', 'VirtualAlloc'): (1, OWN + ': a native record arena (read-write)'),
    ('kernel/stubs.cpp', '<file>', 'NtAllocateVirtualMemory'): (1, DECL),
    ('kernel/stubs.cpp', '<file>', 'NtFreeVirtualMemory'): (1, DECL),
    ('kernel/stubs.cpp', 'registerStubs', 'NtAllocateVirtualMemory'): (2, 'the guest export registration (by name)'),
    ('kernel/stubs.cpp', 'registerStubs', 'NtFreeVirtualMemory'): (2, 'the guest export registration (by name)'),
    ('kernel/stubs.cpp', 'stub_NtAllocateVirtualMemory', 'NtAllocateVirtualMemory'):
        (10, 'the guest export: guest-heap bookkeeping and log text, no host allocation or protection'),
    ('kernel/stubs.cpp', 'stub_NtFreeVirtualMemory', 'NtFreeVirtualMemory'):
        (2, 'the guest export: recycles or zeroes guest memory, never a host release or decommit'),
    ('kernel/stubs.hpp', '<file>', 'NtAllocateVirtualMemory'): (1, DECL),
    ('kernel/tex_unlock_test.cpp', 'runTests', 'VirtualAlloc'): (4, OWN + ': a self-test reservation'),
    ('kernel/tex_unlock_test.cpp', 'runTests', 'VirtualFree'): (2, OWN + ': a self-test reservation'),
    ('kernel/wffsm_obs.cpp', 'FakeGuest', 'VirtualAlloc'): (2, OWN + ": the observer's fake guest reservation"),
    ('kernel/wffsm_obs.cpp', 'commit', 'VirtualAlloc'): (2, OWN + ": the observer's fake guest reservation"),
    ('kernel/wffsm_obs.cpp', '~FakeGuest', 'VirtualFree'): (1, OWN + ": the observer's fake guest reservation"),
    # Section creation (round B): every mapping object the program creates.
    ('app/main.cpp', 'publishPlayerPid', 'CreateFileMappingW'): (1, OWN + ': the player-pid named mapping'),
    ('deps/FFmpeg/libavutil/file.c', 'av_file_map', 'CreateFileMapping'): (2, OWN + ': FFmpeg maps a file read-only'),
    ('guest/guest_harness.cpp', 'allocGuestMemoryUnified', 'CreateFileMappingW'):
        (1, 'the guest arena (unified model): its 512 MB pagefile section, created before the arena is registered'),
    ('kernel/host_automation.cpp', 'initAutomation', 'CreateFileMappingA'): (1, OWN + ': the automation named mapping'),
    # Symbol resolvers (round B): every resolver call is a site whatever its argument; each resolves one named
    # function that is not a protection, mapping or release API (or, in allocGuestMemoryUnified, the arena setup's).
    ('app/main.cpp', 'SDL_main', 'GetProcAddress'): (2, 'ntdll NtQueryTimerResolution / NtSetTimerResolution'),
    ('app/stdio_guard.cpp', 'nameThread', 'GetProcAddress'): (1, 'kernel32 SetThreadDescription'),
    ('build/vcpkg_installed/x64-windows/include/SDL3/SDL_video.h', '<file>', 'GetProcAddress'):
        (2, "SDL3's declarations of SDL_GL_GetProcAddress / SDL_EGL_GetProcAddress (OpenGL / EGL loaders, unused here)"),
    ('deps/FFmpeg/compat/w32pthreads.h', 'win32_thread_setname', 'GetProcAddress'): (1, 'kernel32 SetThreadDescription'),
    ('gfx/shader_xlat_core.cpp', 'dxcLoad', 'GetProcAddress'): (1, 'dxcompiler DxcCreateInstance'),
    ('guest/guest_harness.cpp', 'allocGuestMemoryUnified', 'GetProcAddress'):
        (2, 'kernelbase VirtualAlloc2 / MapViewOfFile3 for the unified arena setup, before it is registered'),
    ('kernel/cs_timeout_underflow_fix.cpp', 'hostTimerResolutionMs', 'GetProcAddress'): (1, 'ntdll NtQueryTimerResolution'),
    ('kernel/gpu_hang_wd.cpp', 'wdMain', 'GetProcAddress'): (1, 'kernel32 SetThreadDescription'),
    ('kernel/host_profile.cpp', 'profMain', 'GetProcAddress'): (1, 'ntdll NtQueryInformationThread'),
    ('kernel/perf_stats.cpp', 'ownThreads', 'GetProcAddress'): (1, 'ntdll NtQuerySystemInformation'),
    ('third_party/imgui/backends/imgui_impl_dx12.cpp', 'ImGui_ImplDX12_CreateDeviceObjects', 'GetProcAddress'):
        (1, 'd3d12 D3D12SerializeRootSignature'),
}

# Binary inputs: key -> (kind, reason). Keys are normalized paths ('build/...', 'app/...') of inputs that are not text
# or do not exist, 'lib:<name>.lib' for link-line / #pragma / --dependent-lib libraries named without a path, and
# 'dll:<name>' for /DELAYLOAD. IN_TREE: an output of this build graph (verified with ninja -t query), so its own
# inputs are among those scanned. EXTERNAL: implementation not scanned; the reason says why the guest arena never
# reaches it.
IN_TREE, EXTERNAL = 'in-tree', 'external'
SYSTEM = ('import library of a Windows system DLL: it binds calls the scanned code makes (each such call site is '
          'classified above); the DLL changes protection of, or releases, only memory it allocated itself')
STAMP = 'a CMake custom-command output (no linked code; what it generates is scanned as text)'
BINARIES = {
    'app/app_icon.ico': (EXTERNAL, 'the window icon: resource data, no code'),
    'build': (EXTERNAL, 'the build directory itself (a CMake directory dependency), no code'),
    'build/CMakeFiles/sr_port_features': (IN_TREE, STAMP),
    'build/CMakeFiles/sr_version_header': (IN_TREE, STAMP),
    'build/guest/CMakeFiles/direct_calls_guard': (IN_TREE, STAMP),
    'build/ffmpeg_xenia.dll': (IN_TREE, 'the in-tree FFmpeg (XMA decode): built here, its sources are scanned'),
    'build/ffmpeg_xenia/ffmpeg_xenia.lib': (IN_TREE, 'the import library of the in-tree ffmpeg_xenia.dll'),
    'dll:ffmpeg_xenia.dll': (IN_TREE, 'the delay-load name of build/ffmpeg_xenia.dll'),
    'def:CMakeFiles/ffmpeg_xenia.dir/exports.def':
        (EXTERNAL, 'the export list CMake writes for ffmpeg_xenia.dll at link time (WINDOWS_EXPORT_ALL_SYMBOLS in '
                   'cmake/ffmpeg_xenia.cmake) from the scanned objects: the names of their own symbols, no code'),
    'build/guest/SupermanReturnsGuest.lib': (IN_TREE, 'the recompiled guest runtime (guest/ppc): built here, scanned'),
    'build/sr_imgui.lib': (IN_TREE, 'Dear ImGui: built here, scanned'),
    'build/xenos_xlat.lib': (IN_TREE, 'the Xenos shader translator: built here, scanned'),
    'build/vcpkg_installed/x64-windows/lib/SDL3.lib':
        (EXTERNAL, 'SDL3 (vcpkg overlay port 3.4.18, prebuilt import library of SDL3.dll): window, input and audio; '
                   'SDL allocates its own memory and is never given the guest arena to protect, map or free'),
    'lib:msvcrt.lib': (EXTERNAL, 'the C/C++ runtime (startup objects and import library, with the ucrt / vcruntime it '
                                 'pulls in): its own heap and stacks, never the guest arena'),
    'lib:oldnames.lib': (EXTERNAL, 'the CRT POSIX-name aliases: forwarding stubs only'),
    'lib:delayimp.lib': (EXTERNAL, "the delay-load helper (static): it may unprotect the exe's own delay-import "
                                   'table, never guest memory'),
    'lib:uuid.lib': (EXTERNAL, 'static GUID data, no code'),
}
for _dll in ('advapi32', 'bcrypt', 'cfgmgr32', 'comdlg32', 'd3d12', 'd3dcompiler', 'dbghelp', 'dxgi', 'gdi32', 'imm32',
             'kernel32', 'ole32', 'oleaut32', 'psapi', 'setupapi', 'shell32', 'synchronization', 'user32', 'windowscodecs', 'winmm',
             'winspool', 'xinput'):
    BINARIES['lib:%s.lib' % _dll] = (EXTERNAL, SYSTEM)

# Functions that change protection of, or release, guest memory: the call that must come first.
FUNNELS = {
    ('kernel/native_hardware_memory.cpp', 'protectHardwareReservation'): 'declare',
    ('kernel/image_write_watchdog.cpp', 'wdProtect'): 'declare',
    ('kernel/audio_cmdq_obs.cpp', 'protectRange'): 'declare',
    ('kernel/guest_context.cpp', 'releaseGuestArena'): 'retireArena',
}
# Pragma wrapper macros: (file, macro) -> reason. A _Pragma whose argument is not a plain literal sequence, or a
# __pragma whose first word is not in PRAGMA_NAMES, is accepted only in the replacement body of one of these
# function-like #defines; every use of the wrapper anywhere must then pass a pragma named in PRAGMA_NAMES.
PRAGMA_WRAPPERS = {
    ('deps/FFmpeg/libavutil/macros.h', 'AV_PRAGMA'): 'FFmpeg: #define AV_PRAGMA(s) _Pragma(#s)',
    ('deps/XenonRecomp/thirdparty/simde/hedley.h', 'HEDLEY_PRAGMA'):
        'hedley (simde): _Pragma(#value), or __pragma(value) for MSVC',
    ('deps/XenosRecomp/thirdparty/fmt/include/fmt/base.h', 'FMT_PRAGMA_IMPL'): 'fmt: _Pragma(#x)',
}
# Pragmas that add no linker input (comment(lib / linker) is the one that does): the only first words a wrapper's
# argument or a __pragma may have.
PRAGMA_NAMES = {'GCC', 'clang', 'omp', 'simd', '_CRI', 'warning', 'pack', 'push_macro', 'pop_macro', 'message',
                'runtime_checks', 'check_stack', 'strict_gs_check'}
WRAPPER_NAMES = sorted({name for _, name in PRAGMA_WRAPPERS})
PRAGMA_HINT = re.compile(r'(?i)pragma|' + '|'.join(WRAPPER_NAMES))
# Diagnostic gates in guest_protect.hpp -> the file whose own predicate reads it.
OWNERS = {'SR_PROTECT_ZERO': 'kernel/audio_cmdq_obs.cpp', 'SR_IMAGE_RO': 'kernel/image_write_watchdog.cpp',
          'SR_WRITE_WATCH_VA': 'kernel/image_write_watchdog.cpp',
          'SR_XMV_WATCH': 'kernel/cs_timeout_underflow_fix.cpp',
          'SR_XMV_EXTRA_WATCH': 'kernel/cs_timeout_underflow_fix.cpp',
          'SR_ALLOC_WATCH': 'kernel/gpu_ring_alloc_fix.cpp'}


# A preprocessing number (C++ pp-number): its apostrophes are digit separators (1'000, 0xFF'FF, 0b1010'1010), never
# the opener of a character literal.
PP_NUMBER = re.compile(r"\.?[0-9](?:[eEpP][+-]|'[0-9A-Za-z_]|[0-9A-Za-z_.])*")


def number_end(text, i):
    """The end of the pp-number that starts at i, or None: none starts there, or the digit belongs to an identifier
    (u8, x1) or to a number already under way."""
    if text[i] not in '0123456789.' or (i > 0 and (text[i - 1].isalnum() or text[i - 1] in '_.\'')):
        return None
    m = PP_NUMBER.match(text, i)
    return m.end() if m else None


@functools.lru_cache(maxsize=None)
def views(text):
    """(comments blanked, comments and literals blanked): two views of text, the same length, newlines kept. A
    number is taken whole, so a digit separator never opens a character literal."""
    keep, code, i, n = [], [], 0, len(text)

    def blank(s):
        return ''.join(ch if ch == '\n' else ' ' for ch in s)
    while i < n:
        c = text[i]
        number = number_end(text, i) if c in '0123456789.' else None
        if number:
            keep.append(text[i:number]); code.append(text[i:number]); i = number
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            keep.append(blank(text[i:j])); code.append(blank(text[i:j])); i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            keep.append(blank(text[i:j])); code.append(blank(text[i:j])); i = j
        elif c == 'R' and text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            k = text.find('(', i + 2)
            j = text.find(')' + text[i + 2:k] + '"', k) if k > 0 else -1
            j = n if j < 0 else j + (k - i - 2) + 2
            keep.append(text[i:j]); code.append(blank(text[i:j])); i = j
        elif c in '"\'':
            # a literal never spans a newline here; an unterminated one stops at it
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' and j + 1 < n and text[j + 1] != '\n' else 1
            if j < n and text[j] == c:
                j += 1
            keep.append(text[i:j]); code.append(blank(text[i:j])); i = j
        else:
            keep.append(c); code.append(c); i += 1
    return ''.join(keep), ''.join(code)


TRAILING = re.compile(r'\s*(?:const|noexcept(?:\s*\([^()]*\))?|override|final|->\s*[\w:<>\s*&,]+)\s*$')


def function_label(header):
    """The function a brace block belongs to, from the code between the previous ; { } and the brace, or None: the
    identifier in front of the header's final parenthesised group (preprocessor lines dropped, trailing const /
    noexcept / override / final / return type stripped). A lambda (']' before its parameters), a control statement or
    an initializer is not a function."""
    h = '\n'.join(l for l in header.split('\n') if not l.lstrip().startswith('#')).strip()
    init = re.match(r'^(.*?\))\s*:(?!:)', h, re.S)  # a constructor's member initializer list
    if init and '?' not in h:
        h = init.group(1)
    while True:
        stripped = TRAILING.sub('', h)
        if stripped == h:
            break
        h = stripped
    if not h.endswith(')'):
        return None
    depth, i = 0, len(h) - 1
    while i >= 0:
        depth += {')': 1, '(': -1}.get(h[i], 0)
        if depth == 0:
            break
        i -= 1
    if i < 0:
        return None
    m = re.search(r'(~?\w+)\s*$', h[:i])
    if not m or m.group(1) in KEYWORDS:
        return None
    name, args = m.group(1), h[i + 1:-1].strip()
    if name.isupper() and re.fullmatch(r'\w+', args):  # PPC_FUNC(sub_x) and its kin
        return '%s(%s)' % (name, args)
    return name


ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v'}


def decode(body):
    """A literal's characters as the compiler reads them (\\x, octal, \\u and the simple escapes decoded)."""
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c != '\\' or i + 1 >= len(body):
            out.append(c); i += 1
            continue
        e = body[i + 1]
        m = re.match(r'x([0-9A-Fa-f]+)|u([0-9A-Fa-f]{4})|U([0-9A-Fa-f]{8})|([0-7]{1,3})', body[i + 1:])
        if m:
            out.append(chr(int(next(g for g in m.groups() if g), 8 if m.group(4) else 16) & 0x10FFFF))
            i += 1 + m.end()
        else:
            out.append(ESCAPES.get(e, e)); i += 2
    return ''.join(out)


def string_literals(keep):
    """[(start, end, characters)] for every string literal in keep (comments already blanked): ordinary ones with an
    optional u8 / u / U / L prefix, and raw ones; character literals and numbers (digit separators included) are
    skipped."""
    out, i, n = [], 0, len(keep)
    while i < n:
        c = keep[i]
        number = number_end(keep, i) if c in '0123456789.' else None
        if number:
            i = number
            continue
        if c == "'":
            j = i + 1
            while j < n and keep[j] not in "'\n":
                j += 2 if keep[j] == '\\' else 1
            i = j + 1
            continue
        if c != '"':
            i += 1
            continue
        prefix = re.search(r'(?:u8|[uUL])?R?$', keep[max(0, i - 3):i]).group(0)
        start = i - len(prefix)
        if start > 0 and (keep[start - 1].isalnum() or keep[start - 1] == '_'):
            prefix, start = '', i  # an identifier ending in R / L / u: not a prefix
        if prefix.endswith('R'):
            open_ = keep.find('(', i + 1)
            delim = keep[i + 1:open_] if open_ > 0 else None
            close = keep.find(')' + delim + '"', open_) if delim is not None and len(delim) <= 16 else -1
            if close < 0:
                out.append((start, n, keep[i + 1:]))
                break
            out.append((start, close + len(delim) + 2, keep[open_ + 1:close]))
            i = close + len(delim) + 2
            continue
        j = i + 1
        while j < n and keep[j] not in '"\n':
            j += 2 if keep[j] == '\\' else 1
        out.append((start, j + 1, decode(keep[i + 1:j])))
        i = j + 1
    return out


def literal_runs(keep):
    """[(start, end, joined characters)]: adjacent string literals (only whitespace between them, comments already
    blanked) joined as the compiler joins them."""
    runs = []
    for start, end, chars in string_literals(keep):
        if runs and not keep[runs[-1][1]:start].strip():
            runs[-1] = (runs[-1][0], end, runs[-1][2] + chars)
        else:
            runs.append((start, end, chars))
    return runs


def references(text):
    """[(outermost enclosing function or '<file>', token, offset)] for every API reference in text: in code outside
    string literals, and in every run of adjacent literals decoded and joined (so a name spelled across literals or
    with escapes counts once, at the run's start)."""
    keep, code = views(text)
    runs = literal_runs(keep)
    outside = list(keep)
    for start, end, _ in runs:
        outside[start:end] = ' ' * (end - start)
    hits = [(m.start(), m.group(0)) for m in API.finditer(''.join(outside))]
    hits += [(start, m.group(0)) for start, _, chars in runs for m in API.finditer(chars)]
    hits.sort()
    if not hits:
        return []
    out, stack, boundary, h = [], [], -1, 0
    for i, ch in enumerate(code):
        while h < len(hits) and hits[h][0] <= i:
            outer = next((s for s in stack if s), None)
            out.append((outer or '<file>', hits[h][1], hits[h][0]))
            h += 1
        if h >= len(hits):
            break
        if ch == '{':
            stack.append(function_label(code[boundary + 1:i]))
            boundary = i
        elif ch == '}':
            if stack:
                stack.pop()
            boundary = i
        elif ch == ';':
            boundary = i
    return out


BALANCED = r'\((?:[^()]|\((?:[^()]|\([^()]*\))*\))*\)'
QUALIFIERS = (r'\s*(?:(?:const|volatile|override|final)\b\s*|&&\s*|&\s*|noexcept\s*(?:' + BALANCED + r')?\s*'
              r'|throw\s*' + BALANCED + r'\s*|->\s*[\w:<>*&\s,]+?(?=[{;]))*')
DEF_TAIL = re.compile(QUALIFIERS + r'\{')
DECL_TAIL = re.compile(QUALIFIERS + r';')


def directive_spans(code):
    """[(start, end)] of every preprocessor directive's logical line (backslash continuations included)."""
    return [(m.start(), m.end()) for m in re.finditer(r'(?m)^[ \t]*#(?:[^\n]*\\\n)*[^\n]*', code)]


def funnel_forms(code, name):
    """(definitions [(definition start, body start, body end)], problems) for function `name` in code. A definition is
    the name, its balanced parameter list, the trailing qualifiers and exception specification a definition may carry
    (const, volatile, &, &&, noexcept, noexcept(...), throw(...), override, final, a trailing return type) and its
    brace. The name inside a preprocessor directive (a macro body) is a problem, never a definition; outside any
    function body, a form that is neither a definition nor a declaration (`;`) is a problem: unresolved forms fail."""
    directives = directive_spans(code)
    defs, problems = [], []
    occurrences = [m for m in re.finditer(r'\b' + re.escape(name) + r'\b', code)]
    if not occurrences:
        return defs, problems
    # function bodies, to tell calls from file-scope forms
    stack, boundary, inside, k = [], -1, {}, 0
    for i, ch in enumerate(code):
        while k < len(occurrences) and occurrences[k].start() <= i:
            inside[occurrences[k].start()] = any(stack)
            k += 1
        if k >= len(occurrences):
            break
        if ch == '{':
            stack.append(function_label(code[boundary + 1:i]) is not None)
            boundary = i
        elif ch == '}':
            if stack:
                stack.pop()
            boundary = i
        elif ch == ';':
            boundary = i
    for m in occurrences:
        if any(a <= m.start() < b for a, b in directives):
            problems.append('%s appears in a preprocessor directive (a macro body is never accepted as its definition)'
                            % name)
            continue
        if inside.get(m.start()):
            continue  # a call or a reference inside a function body
        paren = re.compile(r'\s*\(').match(code, m.end())
        if not paren:
            problems.append('%s is referenced at file scope in a form the census does not resolve' % name)
            continue
        depth, i = 0, paren.end() - 1
        while i < len(code):
            depth += {'(': 1, ')': -1}.get(code[i], 0)
            if depth == 0:
                break
            i += 1
        tail = DEF_TAIL.match(code, i + 1)
        if tail:
            depth, j = 0, tail.end() - 1
            while j < len(code):
                depth += {'{': 1, '}': -1}.get(code[j], 0)
                if depth == 0:
                    defs.append((m.start(), tail.end() - 1, j))
                    break
                j += 1
        elif not DECL_TAIL.match(code, i + 1):
            problems.append('%s has a file-scope form the census does not resolve (neither a definition nor a '
                            'declaration)' % name)
    return defs, problems


PLATFORM_GUARD = re.compile(r'#\s*(?:if\s+defined\s*\(?\s*_WIN32\s*\)?|ifdef\s+_WIN32)\s*$')


def conditionals(code, offset):
    """The preprocessor conditionals open at `offset` (their opening lines, outermost first); an #else / #elif
    branch counts as a conditional of its own (it is not the platform guard's active branch)."""
    stack = []
    for m in re.finditer(r'(?m)^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b.*$', code[:offset]):
        kind = m.group(1)
        if kind in ('if', 'ifdef', 'ifndef'):
            stack.append(m.group(0).strip())
        elif kind in ('elif', 'else') and stack:
            stack[-1] = m.group(0).strip()
        elif kind == 'endif' and stack:
            stack.pop()
    return stack


HARMLESS = re.compile(r'\s*(?:\(void\)\s*\w+\s*;|namespace\s+\w+\s*=\s*[\w:]+\s*;)')


def first_call(code, span, required):
    """Offset after the statement that makes `required` the first effective statement of the body, or None."""
    i = span[0] + 1
    while True:
        m = HARMLESS.match(code, i)
        if m:
            i = m.end()
            continue
        g = re.compile(r'\s*if\s*\(').match(code, i)
        if g:  # a guard: if (...) return ...;
            depth, j = 1, g.end()
            while j < span[1] and depth:
                depth += {'(': 1, ')': -1}.get(code[j], 0)
                j += 1
            r = re.compile(r'\s*return\b[^;{}]*;').match(code, j)
            if not r:
                return None
            i = r.end()
            continue
        break
    end = code.find(';', i)
    if end < 0 or end > span[1]:
        return None
    statement = code[i:end]
    m = re.search(r'(?:[\w:]+::)?' + required + r'\s*\(', statement)
    if not m:
        return None
    prefix = statement[:m.start()]
    # unconditional: at most a declaration of the result in front of the call
    if not re.fullmatch(r'\s*(?:(?:const\s+)?[\w:<>]+\s+\w+\s*=\s*)?', prefix):
        return None
    return end + 1


def normalize(path, build):
    p = str(path).replace('\\', '/')
    if not re.match(r'^[A-Za-z]:/|^/', p):
        p = (Path(build) / p).resolve().as_posix()
    root = ROOT.as_posix()
    if p.lower().startswith(root.lower() + '/'):
        return p[len(root) + 1:]
    if p.lower().startswith(DEPS.lower() + '/'):
        return 'deps/' + p[len(DEPS) + 1:]
    return p


def ninja(build, *args):
    return subprocess.run(['ninja', '-C', str(build), '-t'] + list(args), capture_output=True, text=True,
                          errors='replace', check=True).stdout


def is_text(path):
    """A file whose first 64 KB hold no NUL byte (UTF-16 text counts as binary: classified, not scanned)."""
    with open(path, 'rb') as f:
        return b'\0' not in f.read(65536)


def ninja_statements(build):
    """{output path: (rule, {variable: value})} for every build statement in build.ninja (outputs before the rule's
    ':'), {rule: text of its command / rspfile_content} from the rule files build.ninja includes, and {file variable:
    value} for the top-level variables of build.ninja and those files."""
    statements, rules, top, current = {}, {}, {}, None
    text = (Path(build) / 'build.ninja').read_text(encoding='utf-8', errors='replace')
    for inc in re.findall(r'(?m)^include\s+(\S+)', text) + ['build.ninja']:
        rule = None
        source = text if inc == 'build.ninja' else \
            (Path(build) / inc).read_text(encoding='utf-8', errors='replace')
        for line in source.splitlines():
            if line.startswith('rule '):
                rule = line[5:].strip()
                rules[rule] = ''
            elif rule and re.match(r'^\s+(command|rspfile_content)\s*=', line):
                rules[rule] += ' ' + line.split('=', 1)[1]
            elif not line.startswith((' ', '\t')):
                rule = None
                if re.match(r'^\w+\s*=', line):
                    top[inc + ' ' + line.split('=', 1)[0].strip()] = line.split('=', 1)[1]
    for line in text.splitlines():
        if line.startswith('build '):
            parts = re.split(r'(?<!\$):', line[6:], maxsplit=1)
            head, rest = parts[0], parts[1] if len(parts) > 1 else ''
            outputs = [o.replace('$ ', ' ').replace('$:', ':') for o in re.split(r'(?<!\$) ', head) if o and o != '|']
            current = (rest.split()[0] if rest.split() else '', {})
            for o in outputs:
                statements[o] = current
        elif current is not None and re.match(r'^\s+\w+\s*=', line):
            key, value = re.match(r'^\s+(\w+)\s*=\s?(.*)$', line).groups()
            current[1][key] = value
        elif not line.strip():
            current = None
    return statements, rules, top


def lib_key(name):
    name = name.strip().strip('"').lower()
    return 'lib:' + (name if name.endswith('.lib') else name + '.lib')


def def_key(path):
    """'def:<path>' for a /DEF module-definition file (slashes and '.' segments normalized, as written otherwise)."""
    return 'def:' + posixpath.normpath(path.strip().strip('"').replace('\\', '/'))


BENIGN_LINKER = re.compile(r'(?i)^[/-](?:subsystem|manifest\w*|stack|debug|implib|pdb|version|machine|incremental|opt|'
                           r'dynamicbase|nxcompat|largeaddressaware|nologo|cetcompat|nodefaultlib)(?::\S*)?$')


def linker_flag(flag, where):
    """({binary key: how}, problems) for one linker flag: /DEFAULTLIB, /DELAYLOAD and /DEF name inputs to classify;
    the flags in BENIGN_LINKER add none; any other linker flag is a problem (/alternatename, /include, /export, ...
    are not modelled)."""
    f = flag.strip().strip('"')
    # Exact retention directives of the scanned log unit: the CRT TLS directory
    # and that unit's own callback pointer. No arbitrary /INCLUDE is admitted.
    if where == 'app/log.cpp' and f.lower() in ('/include:_tls_used', '/include:sr_log_tls_callback'):
        return {}, []
    m = re.match(r'(?i)^[/-]defaultlib:(.+)$', f)
    if m:
        return {lib_key(m.group(1)): (None, '/DEFAULTLIB in ' + where)}, []
    m = re.match(r'(?i)^[/-]delayload:(.+)$', f)
    if m:
        return {'dll:' + m.group(1).lower(): (None, '/DELAYLOAD in ' + where)}, []
    m = re.match(r'(?i)^[/-]def:(.+)$', f)
    if m:
        return {def_key(m.group(1)): (None, '/DEF in ' + where)}, []
    if BENIGN_LINKER.match(f):
        return {}, []
    return {}, ['%s: linker flag %r is not modelled by the census' % (where, f)]


LINK_VARS = ('FLAGS', 'LINK_FLAGS', 'LINK_LIBRARIES', 'LINK_PATH', 'ARCH_FLAGS', 'LANGUAGE_COMPILE_FLAGS')
DRIVER_OK = re.compile(r'^-(?:O\w*|D\S+|U\S+|g\w*|fuse-ld=\S+|nostartfiles|nostdlib|shared|std=\S+|W(?!l,)\S*|L\S+|'
                       r'm64)$')
# -Xclang values a compile statement may pass: one that takes the next -Xclang value (a path) as its argument, and
# ones that add no linker input. --dependent-lib names a library; any other -Xclang value is a problem.
XCLANG_ARG = {'-include', '-include-pch'}
XCLANG_OK = {'-emit-pch'}
LINKISH = re.compile(r'(?i)(--dependent-lib=\S+|--linker-option\S*|-fms-runtime-lib\S*|-Wl,\S+|-Xlinker\s+\S+|'
                     r'(?<!\S)/link(?!\S)|(?<![\w.])[/-](?:defaultlib|delayload|def|alternatename|include|export|merge|'
                     r'section|entry):\S+)')


def link_text_flags(text, where):
    """({binary key: how}, problems) for the link-affecting forms in free text (a compile statement's variables, a
    rule's command, a file variable): every -Xclang value (--dependent-lib names a library, XCLANG_ARG / XCLANG_OK
    are accepted, anything else is a problem), and anywhere in the text --dependent-lib, -Wl, -Xlinker and the
    /DEFAULTLIB-style linker flags (linker_flag); --linker-option, -fms-runtime-lib and /link are not modelled."""
    found, problems = {}, []
    tokens = [t.strip('"') for t in re.findall(r'"[^"]*"|\S+', text)]
    i = 0
    while i < len(tokens):
        t, i = tokens[i], i + 1
        if t != '-Xclang':
            continue
        value = tokens[i] if i < len(tokens) else ''
        i += 1
        if value.startswith('--dependent-lib='):
            continue  # the scan below names the library
        if value in XCLANG_ARG and i + 1 < len(tokens) and tokens[i] == '-Xclang':
            i += 2
        elif value not in XCLANG_OK:
            problems.append('%s: -Xclang %r is not modelled by the census' % (where, value))
    for m in LINKISH.finditer(text):
        token = m.group(1)
        if token.startswith('--dependent-lib='):
            found[lib_key(token.split('=', 1)[1])] = (None, '--dependent-lib in ' + where)
            continue
        if re.match(r'(?i)--linker-option|-fms-runtime-lib|/link$', token):
            problems.append('%s: %r is not modelled by the census' % (where, token))
            continue
        flags = token[4:].split(',') if token.startswith('-Wl,') else \
            [token.split(None, 1)[1]] if token.lower().startswith('-xlinker') else [token]
        for f in flags:
            b, p = linker_flag(f, where)
            found.update(b); problems += p
    return found, problems


def link_statement(variables, where):
    """([('path', p)], {binary key: how}, problems) for one link statement's variables, token by token: -l names,
    library paths, -Wl, -Xlinker and -Xclang --dependent-lib forms, /flags; an unmodelled driver flag is a problem.
    Every other variable is read as free text (link_text_flags)."""
    paths, found, problems = [], {}, []
    for var in LINK_VARS:
        tokens = [t.strip('"') for t in re.findall(r'"[^"]*"|\S+', variables.get(var, ''))]
        i = 0
        while i < len(tokens):
            t = tokens[i]
            if t in ('-Xlinker', '-Xclang') and i + 1 < len(tokens):
                nxt = tokens[i + 1]
                i += 2
                if t == '-Xclang':
                    if nxt.startswith('--dependent-lib='):
                        found[lib_key(nxt.split('=', 1)[1])] = (None, '--dependent-lib in ' + where)
                    else:
                        problems.append('%s: -Xclang %r is not modelled by the census' % (where, nxt))
                    continue
                b, p = linker_flag(nxt, where)
                found.update(b); problems += p
                continue
            i += 1
            if t.startswith('-Wl,'):
                for f in t[4:].split(','):
                    b, p = linker_flag(f, where)
                    found.update(b); problems += p
            elif t.startswith('--dependent-lib='):
                found[lib_key(t.split('=', 1)[1])] = (None, '--dependent-lib in ' + where)
            elif t.startswith('-l'):
                found[lib_key(t[2:])] = (None, 'linked into ' + where)
            elif t.startswith('/'):
                b, p = linker_flag(t, where)
                found.update(b); problems += p
            elif t.startswith('-'):
                if not DRIVER_OK.match(t):
                    problems.append('%s: link-line flag %r is not modelled by the census' % (where, t))
            else:
                paths.append(t)
    for var, value in variables.items():
        if var not in LINK_VARS:
            b, p = link_text_flags(value, where + ' ' + var)
            found.update(b); problems += p
    return paths, found, problems


def build_inputs(build, target):
    """The target's build inputs: ({normalized path: absolute path} of text to scan, {binary key: (normalized path or
    None, how it entered)} to classify, [problems], toolchain/SDK files skipped). Every compiler-recorded dependency of
    an input is taken whatever its suffix; every input that is not text, or does not exist, and every library on the
    link lines of the target and of the in-tree DLLs it loads is a binary to classify."""
    listed = [l.strip().lstrip('﻿') for l in ninja(build, 'inputs', target).splitlines() if l.strip()]
    deps, current = {}, None
    for line in ninja(build, 'deps').splitlines():
        if line and not line[0].isspace():
            m = re.match(r'^(.*?): #deps \d+, deps mtime \d+ \((\w+)\)', line)
            current = m.group(1) if m else None
            if current:
                deps[current] = (m.group(2), [])
        elif current and line.strip():
            deps[current][1].append(line.strip())
    problems, candidates = [], {}
    for item in listed:
        entry = deps.get(item)
        if entry:
            if entry[0] != 'VALID':
                problems.append('%s: deps entry %s (build %s before the census)' % (item, entry[0], target))
            for dep in entry[1]:
                candidates.setdefault(dep, 'a recorded dependency of ' + item)
        elif item.endswith(('.obj', '.o')):
            problems.append('%s: no deps entry (build %s before the census)' % (item, target))
        else:
            candidates.setdefault(item, 'an input of ' + target)
    files, binaries, platform = {}, {}, 0
    for item, how in candidates.items():
        p = item.replace('\\', '/')
        absolute = p if re.match(r'^[A-Za-z]:/|^/', p) else (Path(build) / p).resolve().as_posix()
        if PLATFORM.search(absolute):
            platform += 1
            continue
        key = normalize(absolute, build)
        path = Path(absolute)
        if path.is_file() and is_text(path):
            files[key] = absolute
        else:
            binaries[key] = (key, how)
    # Every contributing statement - the target's link, and the compile, link and custom statements of everything it
    # is built from - with its rule's command: link statements token by token, the rest as free text.
    statements, rules, top = ninja_statements(build)
    if target not in statements:
        problems.append('%s: no build statement in build.ninja (cannot read its link line)' % target)
    for var, value in top.items():
        b, p = link_text_flags(value, var)
        for key, how in b.items():
            binaries.setdefault(key, how)
        problems += p
    seen_rules, seen_statements = set(), set()
    for output in [target] + listed:
        if output not in statements or id(statements[output]) in seen_statements:
            continue
        seen_statements.add(id(statements[output]))
        rule, variables = statements[output]
        if rule != 'phony' and rule not in rules:
            problems.append('%s: rule %r has no command in the rule files (cannot read it)' % (output, rule))
        if 'LINKER' in rule:
            paths, found, flag_problems = link_statement(variables, output)
            for name in paths:
                p = name.replace('\\', '/')
                absolute = p if re.match(r'^[A-Za-z]:/|^/', p) else (Path(build) / p).resolve().as_posix()
                key = normalize(absolute, build)
                binaries.setdefault(key, (key, 'linked into ' + output))
        else:
            found, flag_problems = {}, []
            for var, value in variables.items():
                b, p = link_text_flags(value, output + ' ' + var)
                found.update(b); flag_problems += p
        for key, how in found.items():
            binaries.setdefault(key, how)
        problems += flag_problems
        if rule not in seen_rules:
            seen_rules.add(rule)
            b, p = link_text_flags(rules.get(rule, ''), 'rule ' + rule)
            for key, how in b.items():
                binaries.setdefault(key, how)
            problems += p
    return files, binaries, problems, platform


def in_tree(build, key):
    """True when the binary at normalized path `key` is an output of this build graph (built here from inputs the
    census reached), per `ninja -t query`."""
    if not key.startswith('build/') and key != 'build':
        return False
    rel = key[len('build/'):] if key != 'build' else '.'
    try:
        out = ninja(build, 'query', rel)
    except subprocess.CalledProcessError:
        return False
    return bool(re.search(r'(?m)^\s+input: ', out))


def splice(text):
    """Text with its line splices (backslash-newline) joined, as the compiler joins them before anything else."""
    return text.replace('\\\r\n', '').replace('\\\n', '')


def needs_scan(text):
    """Filter on the spliced text, a superset: an API name anywhere, or any string literal at all (a name can be
    spelled across literals, around comments or with escapes; only the exact scan resolves that)."""
    return bool(API.search(text)) or '"' in text


def literal_sequence(text):
    """The joined characters when text is nothing but adjacent string literals (and whitespace), else None."""
    pos, joined = 0, ''
    lits = string_literals(text)
    for start, end, chars in lits:
        if text[pos:start].strip():
            return None
        joined, pos = joined + chars, end
    return joined if lits and not text[pos:].strip() else None


def balanced(text, open_):
    """The text inside the parenthesis at `open_` and the offset after it, or (None, open_)."""
    depth, i = 0, open_
    while i < len(text):
        depth += {'(': 1, ')': -1}.get(text[i], 0)
        if depth == 0:
            return text[open_ + 1:i], i + 1
        i += 1
    return None, open_


def comment_pragma(inner, where):
    """({binary key: how}, problems) for the inside of comment(...): lib and linker need a plain literal sequence."""
    m = re.fullmatch(r'\s*(\w+)\s*(?:,(.*))?', inner or '', re.S)
    if not m or m.group(1).lower() not in ('lib', 'linker', 'user', 'compiler', 'exestr'):
        return {}, ['%s: a comment pragma the census does not resolve' % where]
    kind = m.group(1).lower()
    if kind not in ('lib', 'linker'):
        return {}, []  # user / compiler / exestr: nothing to link
    value = literal_sequence(m.group(2) or '')
    if value is None:
        return {}, ['%s: comment(%s, ...) whose argument is not a plain string literal sequence' % (where, kind)]
    if kind == 'lib':
        return {lib_key(value): (None, 'comment(lib) in ' + where)}, []
    found, problems = {}, []
    for f in re.findall(r'"[^"]*"|\S+', value):
        b, p = linker_flag(f, where)
        found.update(b); problems += p
    return found, problems


def comment_call(keep, code, start, end, where):
    """comment_pragma for keep[start:end] when the code there is exactly `comment ( ... )`, else a problem (code is
    keep with literals blanked, so the parentheses are the real ones)."""
    seg = code[start:end]
    open_ = seg.find('(')
    inner, after = balanced(seg, open_) if open_ >= 0 else (None, 0)
    if inner is None or seg[:open_].strip() != 'comment' or seg[after:].strip():
        return {}, ['%s: a comment pragma the census does not resolve' % where]
    return comment_pragma(keep[start + open_ + 1:start + after - 1], where)


def pragma_forms(keep, code, where):
    """({binary key: how}, problems) for every pragma form in one file (keep: comments blanked; code: literals blanked
    too; line splices already joined):
      - #pragma comment(...) and __pragma(comment(...)) need `comment(lib|linker, <plain literal sequence>)` (or a
        user / compiler / exestr comment); a _Pragma needs a plain literal sequence, read the same way;
      - a __pragma whose first word is not in PRAGMA_NAMES, or a _Pragma whose argument is not a literal sequence, is
        accepted only in the replacement body of a classified wrapper (PRAGMA_WRAPPERS) in this file; either name
        outside a balanced call (an alias) is a problem unless a directive only tests it (#ifdef, defined);
      - every use of a wrapper name must be a call whose argument starts with a word in PRAGMA_NAMES; the name in a
        #define of an unclassified file, outside a call (an alias) or with any other argument is a problem."""
    found, problems = {}, []
    spans = directive_spans(code)

    def add(result):
        found.update(result[0]); problems.extend(result[1])

    def directive(offset):
        return next(((a, b) for a, b in spans if a <= offset < b), None)

    def wrapper_body(offset):
        span = directive(offset)
        d = re.match(r'[ \t]*#[ \t]*define[ \t]+(\w+)\(', code[span[0]:span[1]]) if span else None
        return bool(d) and (where, d.group(1)) in PRAGMA_WRAPPERS and span[0] + d.end() <= offset

    def tested(offset):
        """The name at offset is only tested: #ifdef / #ifndef / #undef NAME, or defined(NAME) in #if / #elif."""
        span = directive(offset)
        line = code[span[0]:offset] if span else ''
        return bool(span) and bool(re.fullmatch(r'[ \t]*#[ \t]*(?:undef|ifdef|ifndef)[ \t]+', line) or
                                   (re.match(r'[ \t]*#[ \t]*(?:if|elif)\b', line) and
                                    re.search(r'\bdefined\s*\(?\s*$', line)))

    def call(m):
        """(offset after the '(', inner code, offset after the ')') of the call at name match m; inner is None when
        the name is not called with a balanced argument list (an alias, a fragment)."""
        paren = re.compile(r'\s*\(').match(code, m.end())
        if not paren:
            return 0, None, 0
        inner, end = balanced(code, paren.end() - 1)
        return paren.end(), inner, end
    for m in re.finditer(r'(?m)^[ \t]*#[ \t]*pragma\b([^\n]*)', code):
        if re.match(r'\s*comment\b', m.group(1)):
            add(comment_call(keep, code, m.start(1), m.end(1), where))
    for m in re.finditer(r'\b__pragma\b', code):
        start, inner, end = call(m)
        first = re.match(r'\s*(\w+)', inner or '')
        if tested(m.start()):
            continue
        if inner is None:
            problems.append('%s: names __pragma outside a balanced call (an alias or a fragment)' % where)
        elif first and first.group(1) == 'comment':
            add(comment_call(keep, code, start, end - 1, where))
        elif not (first and first.group(1) in PRAGMA_NAMES) and not wrapper_body(m.start()):
            problems.append('%s: a __pragma the census does not resolve (%s)' % (where, ' '.join(inner.split())[:60]))
    for m in re.finditer(r'\b_Pragma\b', code):
        start, inner, end = call(m)
        if tested(m.start()):
            continue
        if inner is None:
            problems.append('%s: names _Pragma outside a balanced call (an alias or a fragment)' % where)
            continue
        text = literal_sequence(keep[start:end - 1])
        if text is None:
            if not wrapper_body(m.start()):
                problems.append('%s: a _Pragma whose argument is not a plain string literal sequence' % where)
        elif re.match(r'\s*comment\b', text):
            add(comment_call(text, views(text)[1], 0, len(text), where))
    for name in WRAPPER_NAMES:
        for m in re.finditer(r'\b%s\b' % name, code):
            if tested(m.start()):
                continue
            span = directive(m.start())
            if span and re.fullmatch(r'[ \t]*#[ \t]*define[ \t]+', code[span[0]:m.start()]):
                if (where, name) not in PRAGMA_WRAPPERS:
                    problems.append('%s: defines the pragma wrapper name %s (classified only in its own file)' %
                                    (where, name))
                continue
            _, inner, _ = call(m)
            first = re.match(r'\s*(\w+)', inner or '')
            if inner is None:
                problems.append('%s: names the pragma wrapper %s outside a call' % (where, name))
            elif not first or first.group(1) not in PRAGMA_NAMES:
                problems.append('%s: %s(%s) passes a pragma the census does not know' %
                                (where, name, ' '.join(inner.split())[:60]))
    return found, problems


def classify_binaries(binaries, build):
    """Problems for the binary inputs: each must be classified in BINARIES; an IN_TREE one must be a build output;
    with a build tree, an entry nothing references any more is stale."""
    problems = []
    for key, (_, how) in sorted(binaries.items()):
        entry = BINARIES.get(key)
        if not entry:
            problems.append('%s: unclassified binary input (%s): classify it in BINARIES with a reason' % (key, how))
        elif entry[0] == IN_TREE and build is not None and \
                not in_tree(build, 'build/' + key[4:] if key.startswith('dll:') else key):
            problems.append('%s: classified IN_TREE but not an output of this build graph' % key)
    if build is not None:
        for key in sorted(set(BINARIES) - set(binaries)):
            problems.append('%s: classified binary is no longer an input (remove the entry)' % key)
    return problems


def census(files, binaries=None, build=None):
    """Problems (empty when the census holds) and the per-site references, for {normalized path: text} and, from a
    build tree, its binary inputs ({key: (path, how)}); #pragma comment(lib) in the text adds binaries either way."""
    problems, seen = [], {}
    binaries = binaries if binaries is not None else {}  # the caller's dict gains the #pragma libraries
    files = {path: splice(text) for path, text in files.items()}
    for path in sorted(files):
        text = files[path]
        if PRAGMA_HINT.search(text):
            found, pragma_problems = pragma_forms(*views(text), path)
            for key, how in found.items():
                binaries.setdefault(key, how)
            problems += pragma_problems
        if not needs_scan(text):
            continue
        for func, token, _ in references(text):
            seen[(path, func, token)] = seen.get((path, func, token), 0) + 1
    problems += classify_binaries(binaries, build)
    whole = {}  # (file, token) -> references, for the '*' entries of generated guest code
    for (path, func, token), count in seen.items():
        if (path, '*', token) in ALLOW:
            whole[(path, token)] = whole.get((path, token), 0) + count
    for key, count in sorted(seen.items()):
        if (key[0], '*', key[2]) in ALLOW:
            continue
        allowed = ALLOW.get(key)
        if not allowed:
            problems.append('%s: unclassified reference in %s to %s (x%d)' % (key[0], key[1], key[2], count))
        elif allowed[0] != count:
            problems.append('%s: %s references %s %d times, classified %d (re-classify the site)' %
                            (key[0], key[1], key[2], count, allowed[0]))
    for (path, func, token), (count, _) in sorted(ALLOW.items()):
        if path not in files:
            continue
        found = whole.get((path, token), 0) if func == '*' else seen.get((path, func, token), 0)
        if func == '*' and found != count:
            problems.append('%s: references %s %d times, classified %d (re-classify the file)' %
                            (path, token, found, count))
        elif not found:
            problems.append('%s: classified reference in %s to %s is gone (remove the entry)' % (path, func, token))
    for (path, name), required in FUNNELS.items():
        if path not in files:
            problems.append('%s: funnel %s is not among the build inputs' % (path, name))
            continue
        keep, code = views(files[path])
        found, unresolved = funnel_forms(code, name)
        if unresolved:
            problems += ['%s: %s' % (path, p) for p in unresolved]
            continue
        if len(found) != 1:
            problems.append('%s: %s has %d textual definitions; exactly one is allowed (an inactive copy could pass '
                            'for the compiled one)' % (path, name, len(found)))
            continue
        start, b0, b1 = found[0]
        guards = [c for c in conditionals(code, start) if not PLATFORM_GUARD.match(c)]
        if guards:
            problems.append('%s: %s is defined inside a preprocessor conditional (%s)' % (path, name, guards[-1]))
            continue
        call = re.compile(r'\b' + required + r'\s*\(').search(code, b0, b1)
        if call and re.search(r'(?m)^[ \t]*#', code[b0:call.start()]):
            problems.append('%s: %s has a preprocessor directive before its %s' % (path, name, required))
            continue
        after = first_call(code, (b0, b1), required)
        if after is None:
            problems.append('%s: %s does not make %s its first effective statement, unconditionally' %
                            (path, name, required))
            continue
        early = [m.group(0) for m in API.finditer(keep, b0, after)]
        if early:
            problems.append('%s: %s references %s before its %s' % (path, name, ', '.join(early), required))
    header = files.get('kernel/guest_protect.hpp', '')
    listed = set(re.findall(r'"(SR_[A-Z0-9_]+)"', header.split('kDiagnosticGates', 1)[-1]))
    if listed != set(OWNERS):
        problems.append('kernel/guest_protect.hpp: diagnostic gates %s differ from the census owners %s' %
                        (sorted(listed), sorted(OWNERS)))
    for gate, owner in OWNERS.items():
        if not re.search(r'getenv\s*\(\s*"' + gate + '"', files.get(owner, '')):
            problems.append('%s: no longer reads its diagnostic gate %s (guest_protect.hpp lists it)' % (owner, gate))
    return problems, seen


def run(build, target, report=False):
    """Census of the target's build inputs: (problems, sites, inputs scanned, platform files skipped, binaries)."""
    inputs, binaries, problems, platform = build_inputs(build, target)
    files = {p: Path(a).read_text(encoding='utf-8', errors='replace') for p, a in inputs.items()}
    found, seen = census(files, binaries, build)
    if report:
        for key, count in sorted(seen.items()):
            print('SITE %s | %s | %s | %d' % (key[0], key[1], key[2], count))
        for key, (_, how) in sorted(binaries.items()):
            print('BINARY %s | %s' % (key, how))
    return problems + found, seen, len(files), platform, binaries


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', default='build')
    ap.add_argument('--target', default='SupermanReturns.exe')
    ap.add_argument('--report', action='store_true', help='print every site with its count')
    a = ap.parse_args()
    problems, seen, scanned, platform, binaries = run(a.build, a.target, a.report)
    for p in problems:
        print('CENSUS:', p)
    print('[GUEST_PROTECT_CENSUS] %s: %d text inputs of %s scanned (%d toolchain/SDK files skipped), %d binary inputs '
          'classified, %d sites in %d classified keys, %d problems' %
          ('PASS' if not problems and scanned else 'FAIL', scanned, a.target, platform, len(binaries),
           sum(seen.values()), len(seen), len(problems)))
    return 1 if problems or not scanned else 0


if __name__ == '__main__':
    sys.exit(main())
