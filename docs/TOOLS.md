# Tool Plan: Superman Returns Recompilation

Full inventory of tools for the entire project lifecycle — from current state (pre-rendering) through audio, gameplay correctness, performance, and long-term maintenance. Organized by problem category with phase notes for each tool.

> **For the L-series debugging toolkit + how to run the pipelines, see [`TOOLKIT.md`](TOOLKIT.md)**
> (refresh-ghidra, smoke gate, difftest, crash-grind loop, menu-divergence). This file is the broader
> lifecycle inventory + the reference/ground-truth tools.

**Last updated:** 2026-05-16

---

## 1. Reference & Ground Truth

### Xenia + GPU Trace Capture
**Phase:** Now → forever  
**Exists:** Yes — built into Xenia  
**Status:** Cloned at `C:\Tools\xenia` (source only, not built). 636 function chunks indexed into RAG (`--type structs`).  
Run Superman Returns in Xenia with `--trace_gpu_stream`. Records every GPU command (PM4 packets, draw calls, state changes) to a `.trace` file. The trace is the ground truth for what the GPU should receive at every moment. Compare our harness output against it — every rendering bug becomes a diff problem, not a detective problem. Xenia gets Superman to the title menu; our recomp is working on real draw calls (iter148: queueSize=128).

```
xenia.exe --trace_gpu_stream SupermanReturns.xex
```

### Xbox 360 XDK 4802 Headers
**Phase:** Now → forever  
**Exists:** Yes — **done**  
**Status:** Extracted to `C:\Tools\xdk\extracted\XDK\` (4070 files, 457 `.h` headers). Indexed into RAG struct_defs collection via `scripts/setup_rag.py`. Query: `python scripts/query_rag.py --query "..." --type structs`.  

Extraction method: `hayleyxyz/xdk-extractor` pulled the raw CABs from `XDKSetupXenon4802.exe` without running the installer (bypasses XP-only restriction), then 7-zip extracted the LZX CAB.

### Xenia Source Code
**Phase:** Now → forever  
**Exists:** Yes — cloned at `C:\Tools\xenia`  
**Status:** 636 Xenia function chunks (kernel, GPU, APU, CPU) indexed into RAG. Before writing or fixing any stub: `python scripts/query_rag.py --query "<function name>" --type structs`. 10 seconds instead of 30 minutes of Ghidra analysis.

Key directories:
- `src/xenia/kernel/xboxkrnl/` — all kernel exports
- `src/xenia/gpu/` — GPU/rendering pipeline  
- `src/xenia/apu/xma_decoder.cc` — XMA2 audio decoder
- `src/xenia/kernel/xam/` — Xbox app manager

### LibreXenon
**Phase:** Stubs, audio, threading  
**Exists:** Yes — `github.com/Free60Project/libxenon`  
Open source Xbox 360 homebrew SDK. Alternative reference for low-level hardware details (EDRAM, threading primitives, APC queues) when Xenia's implementation is incomplete or unclear.

---

## 2. Static Analysis

### Ghidra + Ghidra MCP
**Phase:** Now → forever  
**Exists:** Yes — active in MCP (`mcp__ghidra__*` tools)  
Decompile any guest function, trace call graphs, find xrefs, inspect memory layout. Primary tool for understanding what a function does.

### Binary Ninja + Binary Ninja MCP
**Phase:** Complex rendering/audio/gameplay logic  
**Exists:** Yes (costs money) — MCP servers free: `fosdickio/binary_ninja_mcp`, `BinAssistMCP`  
Alternative decompiler. Use when Ghidra output is unclear.

### IDA Pro + Hex-Rays
**Phase:** Hard cases only  
**Exists:** Yes (expensive)  
Reserve for the ~5% of functions where Ghidra produces garbage.

### Static Recomp Auditor
**Phase:** Now → rendering complete  
**Exists:** Yes — **done** — `scripts/audit_stubs.py`  
Scans kernel stubs and app sources for known-bad patterns: raw `*(uint32_t*)` reads without bswap, guest VAs used as host pointers, vtable dispatches missing bswap. Currently reports clean — run after any new stub to catch regressions.

```
python scripts/audit_stubs.py [--min-severity LOW|MEDIUM|HIGH] [--all]
```

---

## 3. Dynamic Analysis & Runtime Observation

### Frida + frida-mcp
**Phase:** Now → forever  
**Exists:** Yes — **done** — frida 17.9.9 + frida-mcp 0.1.1 installed  
**Status:** Active in MCP as `mcp__frida-superman__*` tools (project-specific server). Attach to running harness, set hardware watchpoints, read guest memory — all without rebuild.

Tools available: `frida_attach`, `frida_watchpoint`, `frida_get_hits`, `frida_read_memory`, `frida_guest_base`, `frida_eval`.

### WinDbg TTD (Time Travel Debugging)
**Phase:** Now → forever  
**Exists:** Yes — WinDbg installed  
Record a full execution trace. Query it offline, forwards and backwards.

```powershell
# Record a run (run as admin):
& "C:\Program Files\WindowsApps\Microsoft.WinDbg*\TTD\TTD.exe" -out C:\traces -launch "SupermanReturns.exe"

# Query in WinDbg:
dx @$cursession.TTD.Memory(hostAddr, hostAddr+4, "w")
```

**Caveat:** 10–20x runtime slowdown during recording.

### mcp-windbg
**Phase:** Now → forever  
**Exists:** Yes — **done** — mcp-windbg 0.13.0 installed, in MCP config  
**Status:** Active in MCP as `mcp__mcp-windbg__*` tools. Open crash dumps or live debug sessions, issue WinDbg commands, query TTD traces — all from Claude.

Tools: `open_windbg_dump`, `run_windbg_cmd`, `open_windbg_remote`, `send_ctrl_break`.

### Log SQLite Server
**Phase:** Now → forever  
**Exists:** Yes — **done** — `scripts/log_sqlite.py`  
**Status:** Active in MCP as `mcp__log-sqlite__*` tools. Index any log run, then query by category, guest LR, present number, or free text.

```
python scripts/log_sqlite.py --db logs/sr.db index superman_returns.log
python scripts/log_sqlite.py --db logs/sr.db query -c WATCHPOINT --around-present 5000
python scripts/log_sqlite.py --db logs/sr.db stats
```

MCP tools: `log_index`, `log_query`, `log_around`, `log_watchpoint`.

### Live Harness Telemetry (custom, not yet built)
**Phase:** Now → rendering complete  
**Exists:** No — ~200 lines Python + 50 lines C++ harness changes  
Named pipe from the harness to a Python MCP server. Exposes live: present count, queueSize, pendingSubmits, last guest LR, per-thread state. Lower priority now that log_sqlite covers post-run queries.

### x64dbg
**Phase:** Crash investigation  
**Exists:** Yes — `x64dbg.com`  
Free Windows debugger, lighter weight than WinDbg. Better UI for manual single-step inspection.

---

## 4. Graphics Debugging

### RenderDoc + renderdoc-mcp
**Phase:** First frame → rendering complete  
**Exists:** Yes + MCP exists — `github.com/Linkingooo/renderdoc-mcp`  
**Status:** Not yet installed — waiting for real draw calls. Install when queueSize > 0 consistently. One-time `winget install BaldurKarlsson.RenderDoc`.

**Primary workflow:** Capture a frame, compare against Xenia's GPU trace for the same scene. Takes "something looks wrong" to "this specific vertex buffer has garbage at byte offset 48."

### PIX for Windows
**Phase:** First frame → performance  
**Exists:** Yes — free, `devblogs.microsoft.com/pix`  
Microsoft's D3D12 profiler. Use RenderDoc for correctness, PIX for GPU timing.

### D3D12 Debug Layer
**Phase:** Now → rendering complete  
**Exists:** Yes — active in all `#ifndef NDEBUG` builds (`gfx/d3d12_backend.cpp:69`)  
Catches API misuse: wrong resource states, invalid descriptors, buffer overruns in command lists.

### Xenia GPU Trace Viewer
**Phase:** Now → rendering complete  
**Exists:** Yes — built into Xenia (needs Xenia built)  
Shows Xbox 360 PM4 command buffer content. Use to understand what commands the game issues in Xenia, then compare to what our D3D12 backend receives.

---

## 5. Audio Debugging

### Audacity
**Phase:** Audio phase  
**Exists:** Yes — `audacityteam.org`  
Dump XMA-decoded audio buffers to disk. Visual + audible inspection.

### Xbox 360 XMA2 Reference (Xenia source)
**Phase:** Audio phase  
**Exists:** Yes — `C:\Tools\xenia\src\xenia\apu\xma_decoder.cc` + indexed in RAG  

### XAudio2 Debug Layer
**Phase:** Audio phase  
**Exists:** Yes — built into Windows SDK  

---

## 6. Performance Profiling

### Intel VTune / AMD uProf
**Phase:** Performance phase  
**Exists:** Yes — VTune free for personal use  

### Tracy Profiler
**Phase:** Performance phase  
**Exists:** Yes — free, `github.com/wolfpld/tracy`  

### PIX Timing Captures
**Phase:** Performance phase  
**Exists:** Yes — already listed above  

---

## 7. Memory Safety

### AddressSanitizer (ASAN via MSVC/Clang)
**Phase:** Now → forever  
**Exists:** Yes — CMake option `ASAN=ON` (`cmake -B build -DASAN=ON`)  
Catches heap overflows, use-after-free, stack overflows. Applied only to harness sources (not the 144MB ppc_recomp files).

### Dr. Memory
**Phase:** When ASAN misses something  
**Exists:** Yes — `drmemory.org`  

---

## 8. Testing & Regression Prevention

### Savestate System
**Phase:** Now → forever  
**Exists:** Yes — **done** — `kernel/savestate.cpp`  
Serialize/restore full guest memory state at a known-good moment. Controlled via `SR_SAVESTATE_SAVE=<path>` and `SR_SAVESTATE_LOAD=<path>` env vars. Cuts feedback loop from 36s → ~3s once a stable save point exists.

### Input Recording & Replay
**Phase:** Gameplay correctness phase  
**Exists:** Yes — **done** — `kernel/input_recorder.cpp`  
Records XInput state every frame to a binary `.inpr` file. Replay deterministically to reproduce gameplay bugs.

```
SR_INPUT_RECORD=run.inpr ./SupermanReturns.exe   # record
SR_INPUT_REPLAY=run.inpr ./SupermanReturns.exe   # replay
```

### Screenshot Regression Tests
**Phase:** Stable rendering → complete  
**Exists:** No — ~100 lines Python  
Automated pixel comparison against reference frames. Build after first stable title screen.

### Automated Crash Regression Suite
**Phase:** Now → forever  
**Exists:** No (RAG indexes fixes but doesn't run them)  
Set of known-crash scenarios as savestates + expected behaviour. Run after every significant change.

---

## 9. MCP & AI Infrastructure

### Guest VA ↔ Source Index
**Phase:** Now → forever  
**Exists:** Yes — **done** — `scripts/va_to_source.py`  
**Status:** 34,279 functions + 150,913 branch labels indexed in `.cache/va_index.db` (rebuilt
iter-455i, 135 source files). Active in MCP as `mcp__va-to-source__*`.
**⛔ Authoritative ONLY while fresh.** iter-455i found the index five weeks stale and
mis-attributing every line-level lookup in both directions. It now stores a
`source_fingerprint` of the generated-source inventory; `lookup`/`reverse_lookup` **fail
closed** when it does not match (or when the db predates the repair), and rebuilds are
written to a temp db and swapped atomically. Verify with
`python scripts/va_to_source.py --selftest`; repair with `--build`. When it refuses, use the
nearest preceding `PPC_FUNC_IMPL` marker in the checked-in generated source instead.

```
python scripts/va_to_source.py 0x824DB158
# -> ppc_recomp.61.cpp:107281  sub_824DB158
```

MCP tools: `va_to_source`, `source_to_va`, `build_va_index`.

### Log SQLite Server
See section 3 — already covered.

### XDK Documentation RAG + Xenia RAG
**Phase:** Now → forever  
**Exists:** Yes — **done**  
- 457 XDK 4802 headers indexed (`C:\Tools\xdk\extracted\XDK\include`)
- 636 Xenia function chunks indexed (kernel, GPU, APU, CPU)
- Re-index: `python scripts/setup_rag.py --includes-dir C:/Tools/xdk/extracted/XDK/include`
- Re-index Xenia: `python scripts/index_xenia.py`
- Query: `python scripts/query_rag.py --query "VdSwap" --type structs`

---

## 10. Build Infrastructure

### Precompiled Headers
**Phase:** Now  
**Exists:** Yes — **done** — `CMakeLists.txt:157`  
Applied to all harness sources (`kernel/*.cpp`, `app/*.cpp`). Not applied to ppc_recomp (different header set, already parallelized).

### sccache (compiler cache)
**Phase:** Now  
**Exists:** Yes — active — `CMakeLists.txt:51`  
Auto-detected if installed (`winget install Mozilla.sccache`). Caches compiled object files; unchanged files are instant cache hits.

### Unity Builds
**Phase:** When build times are a bottleneck  
**Exists:** CMake `UNITY_BUILD` support since 3.16 — one-line change  

---

## Current Status Summary

| Category | Done | Remaining |
|---|---|---|
| Reference | XDK headers, Xenia source, Xenia clone | Xenia binary build (for trace viewer) |
| Static analysis | Ghidra MCP, static auditor | — |
| Dynamic / runtime | Frida MCP, mcp-windbg, log SQLite, VA index | Live telemetry pipe (low priority) |
| Graphics debugging | D3D12 debug layer | RenderDoc (install when draws work) |
| Testing | Savestate, input recorder | Screenshot regression, crash regression suite |
| RAG / AI infra | XDK headers, Xenia impls, crash fixes | — |
| Build | PCH, sccache | — |

**Everything in the "Now" and "Next week" priority columns is done. Next phase tools (RenderDoc, screenshot regression) unlock when queueSize is consistently > 0.**

---

## One Harness Change That Unlocked Everything

Exporting the guest base address as a named global — **done** in `kernel/guest_context.hpp`:

```cpp
extern "C" __declspec(dllexport) uint8_t* g_debugGuestBase;
```

Required by Frida, WinDbg, and any tool that translates guest VA → host address. The `mcp__frida-superman__frida_guest_base` tool reads this directly.
