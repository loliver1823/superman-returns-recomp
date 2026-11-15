#include "savestate.hpp"
#include "guest_context.hpp"
#include "../app/log.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

// ── File format ───────────────────────────────────────────────────────────────
//
// .sav file layout:
//   [Header]           fixed-size struct (see below)
//   [MemoryRegion[N]]  one per dirty page range (address + size + data)
//   [ThreadContext[M]] one per live guest thread (PPC register set)
//
// The format is intentionally simple: no compression, no encryption.
// Page-dirty tracking is TODO — currently saves full guest memory (slow but correct).

namespace savestate {

static constexpr uint32_t MAGIC   = 0x53415645; // 'SAVE'
static constexpr uint32_t VERSION = 1;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint32_t version;
    uint64_t guestMemorySize;
    uint32_t threadCount;
    uint64_t presentCount;
    double   elapsedSeconds;
    char     timestamp[32];
    uint8_t  _pad[8];
};
#pragma pack(pop)

static_assert(sizeof(Header) == 76, "Header size changed — update version");

// ── Internal helpers ──────────────────────────────────────────────────────────

static bool writeHeader(FILE* f, const Header& h) {
    return fwrite(&h, sizeof(h), 1, f) == 1;
}

static bool readHeader(FILE* f, Header& h) {
    if (fread(&h, sizeof(h), 1, f) != 1) return false;
    if (h.magic != MAGIC) return false;
    if (h.version != VERSION) return false;
    return true;
}

static void fillTimestamp(char* buf, size_t len) {
    time_t now = time(nullptr);
    struct tm tm_info;
#if defined(_WIN32)
    gmtime_s(&tm_info, &now);
#else
    gmtime_r(&now, &tm_info);
#endif
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_info);
}

// ── Public API ────────────────────────────────────────────────────────────────

bool save(const std::string& path) {
    // TODO: quiesce all guest threads before snapshotting.
    // For now this is a best-effort snapshot taken from the calling thread.
    // Thread safety: call between frame boundaries when guest threads are idle.

    uint8_t* base = sr::kernel::getGuestBase();
    size_t   size = sr::kernel::getGuestSize();

    if (!base || size == 0) {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[SAVESTATE] Cannot save: guest memory not initialized");
        return false;
    }

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[SAVESTATE] Cannot open for write: %s", path.c_str());
        return false;
    }

    Header h{};
    h.magic           = MAGIC;
    h.version         = VERSION;
    h.guestMemorySize = static_cast<uint64_t>(size);
    h.threadCount     = 0;       // TODO: serialize live thread contexts
    h.presentCount    = 0;       // TODO: read from D3D present counter
    h.elapsedSeconds  = 0.0;     // TODO: read from harness uptime
    fillTimestamp(h.timestamp, sizeof(h.timestamp));

    if (!writeHeader(f, h)) {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[SAVESTATE] Failed to write header");
        fclose(f);
        return false;
    }

    // Write full guest memory (TODO: page-dirty tracking to reduce size/time)
    size_t written = fwrite(base, 1, size, f);
    fclose(f);

    if (written != size) {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[SAVESTATE] Incomplete write: %zu / %zu bytes", written, size);
        return false;
    }

    SR_LOG_BOOT(sr::LogSeverity::Info, "[SAVESTATE] Saved %zu MB → %s",
                size / (1024 * 1024), path.c_str());
    return true;
}

bool load(const std::string& path) {
    uint8_t* base = sr::kernel::getGuestBase();
    size_t   size = sr::kernel::getGuestSize();

    if (!base || size == 0) {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[SAVESTATE] Cannot load: guest memory not initialized");
        return false;
    }

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[SAVESTATE] Cannot open: %s", path.c_str());
        return false;
    }

    Header h{};
    if (!readHeader(f, h)) {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[SAVESTATE] Invalid savestate file (bad magic/version)");
        fclose(f);
        return false;
    }

    if (h.guestMemorySize != static_cast<uint64_t>(size)) {
        SR_LOG_BOOT(sr::LogSeverity::Error,
            "[SAVESTATE] Size mismatch: file has %llu bytes, current guest has %zu bytes",
            (unsigned long long)h.guestMemorySize, size);
        fclose(f);
        return false;
    }

    // TODO: terminate all guest threads before restoring memory.
    // TODO: restore thread contexts after memory restore.

    size_t read = fread(base, 1, size, f);
    fclose(f);

    if (read != size) {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[SAVESTATE] Incomplete read: %zu / %zu bytes", read, size);
        return false;
    }

    SR_LOG_BOOT(sr::LogSeverity::Info, "[SAVESTATE] Loaded %zu MB from %s (saved at %s, present #%llu)",
                size / (1024 * 1024), path.c_str(),
                h.timestamp, (unsigned long long)h.presentCount);
    return true;
}

bool isValid(const std::string& path) {
    savestate::Info info{};
    return readInfo(path, info);
}

bool readInfo(const std::string& path, Info& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    Header h{};
    bool ok = readHeader(f, h);
    fclose(f);
    if (!ok) return false;
    out.version         = h.version;
    out.guestMemorySize = h.guestMemorySize;
    out.threadCount     = h.threadCount;
    out.presentCount    = h.presentCount;
    out.elapsedSeconds  = h.elapsedSeconds;
    memcpy(out.timestamp, h.timestamp, sizeof(out.timestamp));
    return true;
}

} // namespace savestate
