#include "input_recorder.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace input_recorder {

static constexpr uint32_t MAGIC = 0x494E5052;  // 'INPR'

#pragma pack(push, 1)
struct FileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t frameCount;
    uint32_t _reserved;
};
// iter-462 version 2: one entry per poll, time-stamped against the anchor.
struct TimedEntry {
    uint32_t ms;           // host ms since the anchor (afterAnchor) or since boot
    uint16_t afterAnchor;  // 1 once the guest requested the main HUD
    uint16_t _pad;
    GamepadState state;
};
#pragma pack(pop)

static_assert(sizeof(FileHeader) == 16, "FileHeader size changed");
static_assert(sizeof(GamepadState) == 12, "GamepadState size changed");
static_assert(sizeof(TimedEntry) == 20, "TimedEntry size changed");

using Clock = std::chrono::steady_clock;

static Mode     s_mode      = Mode::Off;
static FILE*    s_file      = nullptr;
static uint32_t s_callCount = 0;
static uint32_t s_version   = 1;
static std::mutex s_mu;
static Clock::time_point s_boot;
static std::atomic<bool> s_anchored{false};
static Clock::time_point s_anchorAt;

// Replay tapes, loaded into memory on init.
static std::vector<GamepadState> s_tape;     // v1
static std::vector<TimedEntry>   s_timed;    // v2, after-anchor entries only
static size_t s_timedPos = 0;

void init() {
    s_boot = Clock::now();
    const char* recPath = getenv("SR_INPUT_RECORD");
    const char* repPath = getenv("SR_INPUT_REPLAY");

    if (repPath && *repPath) {
        FILE* f = fopen(repPath, "rb");
        if (!f) {
            SR_LOG_BOOT(sr::LogSeverity::Error, "[INPUT_REC] Cannot open replay file: %s", repPath);
            return;
        }
        FileHeader h{};
        if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != MAGIC ||
            (h.version != 1 && h.version != 2)) {
            SR_LOG_BOOT(sr::LogSeverity::Error, "[INPUT_REC] Invalid replay file: %s", repPath);
            fclose(f);
            return;
        }
        s_version = h.version;
        // iter-462: a recording whose process was killed never patched its
        // count - trust the file size for v2 (entries are flushed every 1024).
        if (h.version == 2) {
            const long here = ftell(f);
            fseek(f, 0, SEEK_END);
            const long end = ftell(f);
            fseek(f, here, SEEK_SET);
            if (end > here) h.frameCount = static_cast<uint32_t>((end - here) / long(sizeof(TimedEntry)));
        }
        if (h.version == 1) {
            s_tape.resize(h.frameCount);
            const size_t got = fread(s_tape.data(), sizeof(GamepadState), h.frameCount, f);
            s_tape.resize(got);
        } else {
            std::vector<TimedEntry> all(h.frameCount);
            const size_t got = fread(all.data(), sizeof(TimedEntry), h.frameCount, f);
            all.resize(got);
            for (const TimedEntry& e : all)
                if (e.afterAnchor) s_timed.push_back(e);
        }
        fclose(f);
        s_mode = Mode::Replay;
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[INPUT_REC] Replay mode (v%u): %zu entries from %s%s", s_version,
            s_version == 1 ? s_tape.size() : s_timed.size(), repPath,
            s_version == 2 ? " - passes input through until the main-HUD anchor" : "");
        return;
    }

    if (recPath && *recPath) {
        s_file = fopen(recPath, "wb");
        if (!s_file) {
            SR_LOG_BOOT(sr::LogSeverity::Error, "[INPUT_REC] Cannot open record file: %s", recPath);
            return;
        }
        s_version = 2;
        FileHeader h{MAGIC, 2, 0, 0};
        fwrite(&h, sizeof(h), 1, s_file);
        s_mode = Mode::Record;
        SR_LOG_BOOT(sr::LogSeverity::Info, "[INPUT_REC] Record mode (v2, anchored at main_hud) -> %s", recPath);
    }
}

void shutdown() {
    std::lock_guard<std::mutex> lk(s_mu);
    if (s_mode == Mode::Record && s_file) {
        // Patch frameCount into header now that we know it.
        fseek(s_file, static_cast<long>(offsetof(FileHeader, frameCount)), SEEK_SET);
        fwrite(&s_callCount, sizeof(s_callCount), 1, s_file);
        fclose(s_file);
        s_file = nullptr;
        SR_LOG_BOOT(sr::LogSeverity::Info, "[INPUT_REC] Recorded %u polls", s_callCount);
    }
    s_mode = Mode::Off;
}

void anchor() {
    bool expected = false;
    if (!s_anchored.compare_exchange_strong(expected, true)) return;
    s_anchorAt = Clock::now();
    if (s_mode != Mode::Off)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[INPUT_REC] anchor: the guest requested the main HUD (%s)",
                    s_mode == Mode::Record ? "recording time restarts at 0" : "replay starts");
}

static uint32_t msSince(Clock::time_point t) {
    const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
    return d < 0 ? 0u : static_cast<uint32_t>(d);
}

bool replayTick(uint32_t /*callNum*/, GamepadState& state) {
    if (s_mode != Mode::Replay) return false;
    std::lock_guard<std::mutex> lk(s_mu);
    if (s_version == 1) {
        if (s_callCount < static_cast<uint32_t>(s_tape.size())) {
            state = s_tape[s_callCount++];
        } else if (!s_tape.empty()) {
            state = s_tape.back(); // hold last frame
        }
        return true;
    }
    // v2: pass through until the anchor, then the entry for the elapsed time.
    if (!s_anchored.load(std::memory_order_acquire)) return false;
    const uint32_t now = msSince(s_anchorAt);
    while (s_timedPos + 1 < s_timed.size() && s_timed[s_timedPos + 1].ms <= now) s_timedPos++;
    if (s_timed.empty() || now > s_timed.back().ms + 1000u) {
        state = GamepadState{};  // the tape ended: neutral input, never a held stick
        return true;
    }
    state = s_timed[s_timedPos].state;
    return true;
}

void recordTick(uint32_t /*callNum*/, const GamepadState& state) {
    if (s_mode != Mode::Record) return;
    std::lock_guard<std::mutex> lk(s_mu);
    if (!s_file) return;
    const bool after = s_anchored.load(std::memory_order_acquire);
    TimedEntry e{};
    e.ms = after ? msSince(s_anchorAt) : msSince(s_boot);
    e.afterAnchor = after ? 1u : 0u;
    e.state = state;
    fwrite(&e, sizeof(e), 1, s_file);
    ++s_callCount;
    if ((s_callCount & 1023u) == 0u) fflush(s_file);  // a crash keeps most of the tape
}

Mode getMode() { return s_mode; }

} // namespace input_recorder
