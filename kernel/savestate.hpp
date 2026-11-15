#pragma once
// Savestate system: snapshot and restore guest memory + thread contexts.
//
// Usage:
//   savestate::save("C:/traces/t10s.sav");   // call from harness at known-good moment
//   savestate::load("C:/traces/t10s.sav");   // restores guest memory, resets threads
//
// Design notes:
// - Guest memory snapshot: memcpy of the full guest allocation (~0xB4000000 bytes,
//   ~3GB worst case). Only dirty pages are written to disk via a page-level diff.
// - Thread state: PPC register contexts for all live guest threads, saved at the
//   point when all threads are quiesced (paused at a safe point).
// - Restore: overwrite guest memory, replay thread contexts, re-enter the main loop.
//
// Current status: SCAFFOLD — save/load stubs are no-ops. The page-dirty tracking
// and thread quiesce logic need to be implemented before this is functional.
// See docs/TOOLS.md §"Savestate System" for the implementation plan.

#include <cstdint>
#include <string>

namespace savestate {

// Save current guest memory + thread contexts to a file.
// Call from a known-good point (e.g., harness loop at t=10s).
// Thread safety: caller must ensure guest threads are at a safe quiesce point
// (e.g., between frame boundaries, with no active allocations in flight).
//
// Returns true on success, false with error logged on failure.
bool save(const std::string& path);

// Load a previously saved snapshot.
// Overwrites current guest memory and resets all guest thread contexts.
// The harness must be in a state where guest threads can be safely terminated
// and restarted (i.e., called before re-entering the guest main loop).
//
// Returns true on success.
bool load(const std::string& path);

// Returns true if a savestate file exists and appears valid.
bool isValid(const std::string& path);

// Snapshot metadata readable without full load.
struct Info {
    uint32_t version;
    uint64_t guestMemorySize;
    uint32_t threadCount;
    uint64_t presentCount;      // frame # at save time (from D3D_DIAG counter)
    double   elapsedSeconds;    // harness uptime at save time
    char     timestamp[32];     // ISO 8601
};
bool readInfo(const std::string& path, Info& out);

} // namespace savestate
