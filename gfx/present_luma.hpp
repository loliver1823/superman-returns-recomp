#pragma once
// Presented-luminance probe (SR_PRESENT_LUMA=1, default OFF; observe-only test instrument).
// Step 4 item X2 (lane D, 2026-10-18): the eye-adaptation acceptance needs the mean luminance of EVERY
// presented frame against wall time at 30 Hz and uncapped. SR_PRESENT_DUMP cannot give it: each dumped
// present waits for its own GPU copy, scans the whole frame and writes a BMP on the render thread, which
// slows the uncapped frame rate that is being measured. This probe never waits: each present queues a
// GPU copy of a few evenly spaced back-buffer rows into a ring of readback buffers, and a later present
// whose copy has completed averages it. Records go to <dir>/present_luma.csv from a writer thread.
//
//   SR_PRESENT_LUMA=1          on (default off)
//   SR_PRESENT_LUMA_ROWS=64    rows sampled per present (evenly spaced, 1..256)
//   SR_PRESENT_LUMA_STEP=2     every Nth pixel of a sampled row (1..16)
//   SR_PRESENT_LUMA_DIR=<dir>  output folder (default SR_PERF_DIR, else archive)
//
// CSV: pn,t_us,luma,w,h,skipped,hold,held_ms,stamp - hold / held_ms = the SR_DIAG_SIM_FREEZE hold (#id, ms so far)
// the present was queued in (0 = the sim was not held), stamp = the render loop's time (render mgr +0x50, ns; it stops
// once the drawn scene is frozen inside a hold) - the X2 freeze-relaxation analysis. pn = this probe's present ordinal (one per dumpThenOverlay call),
// t_us = sr::perf::nowUs() when the copy was queued (the clock of SR_PERF_STATS' frames.csv), luma = mean
// (299 R + 587 G + 114 B) / 1000 of the sampled 8-bit code values (the PIL "L" weights the other luma
// numbers in the tools use), skipped = presents not sampled so far (a ring slot still in flight).
#ifdef _WIN32
#include <d3d12.h>

namespace sr::gfx::presentluma {

bool enabled() noexcept;

// At a present site, after the frame's command lists were submitted to `queue` and before Present;
// `rt` is the swapchain back buffer in D3D12_RESOURCE_STATE_PRESENT (the SR_PRESENT_DUMP position).
void onPresent(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* rt) noexcept;

}  // namespace sr::gfx::presentluma
#endif
