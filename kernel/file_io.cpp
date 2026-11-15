#include "file_io.hpp"
#include "guest_context.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace sr {
namespace kernel {

std::string contentRootForLocations(const std::string& exePath, const std::string& workingDir) {
  namespace fs = std::filesystem;
  std::vector<fs::path> candidates;
  if (!exePath.empty()) {
    const fs::path exeDir = fs::u8path(exePath).parent_path();
    if (!exeDir.empty()) {
      candidates.push_back(exeDir);
      candidates.push_back(exeDir.parent_path());
    }
  }
  if (!workingDir.empty()) candidates.push_back(fs::u8path(workingDir));
  for (const auto& candidate : candidates) {
    if (candidate.empty()) continue;
    std::error_code ec;
    if (!fs::is_directory(candidate / "Superman Returns (USA)", ec)) continue;
    const fs::path absolute = fs::absolute(candidate, ec);
    if (ec) continue;
    const auto utf8 = absolute.lexically_normal().generic_u8string();
    std::string root(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    if (!root.empty() && root.back() != '/') root += '/';
    return root;
  }
  return {};
}

static std::mutex g_testRootMutex;
static std::string g_testRoot;               // setContentRootForTest ("" = discover)
static std::atomic<bool> g_testRootSet{false};   // the player never sets it: one relaxed load per call

void setContentRootForTest(const std::string& root) {
  std::lock_guard<std::mutex> lk(g_testRootMutex);
  g_testRoot = root;
  g_testRootSet.store(!root.empty(), std::memory_order_release);
}

// Discover from the executable, not the source checkout's name or a baked path.
static std::string getContentRoot() {
  if (g_testRootSet.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lk(g_testRootMutex);
    if (!g_testRoot.empty()) return g_testRoot;
  }
  static std::string s_root = [] {
    std::string exe;
#ifdef _WIN32
    wchar_t exePath[32768] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exePath, 32768);
    if (n > 0 && n < 32768) {
      const auto utf8 = std::filesystem::path(exePath).generic_u8string();
      exe.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    }
#endif
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    std::string cwdUtf8;
    if (!ec) {
      const auto utf8 = cwd.generic_u8string();
      cwdUtf8.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    }
    const std::string root = contentRootForLocations(exe, cwdUtf8);
    if (!root.empty())
      SR_LOG_KERNEL_FILE("Content root: \"%s\"", root.c_str());
    return root;
  }();
  return s_root;
}

std::string contentRootDir() { return getContentRoot(); }

// Unified handle counter shared by all kernel object creators.
static std::atomic<uint32_t> g_nextUnifiedHandle{1};

uint32_t allocUnifiedHandle() {
  return g_nextUnifiedHandle.fetch_add(1, std::memory_order_relaxed);
}

namespace {
  std::mutex g_fileMutex;
  std::map<uint32_t, std::FILE*> g_handleToFile;
  std::map<uint32_t, std::string> g_handleToDirPath;
  std::map<uint32_t, std::shared_ptr<std::mutex>> g_handleToIoLock;   // file handles only
  std::map<uint32_t, bool> g_handleSynchronous;                       // iter-463 SR_READ_PENDING
  std::map<uint32_t, void*> g_handleWaitEvent;                        // iter-463 XFile async_event_
  constexpr uint32_t kMaxHandles = 512;

  uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
  }

  template <typename TMap>
  auto findWithEndianFallback(TMap& m, uint32_t handle) {
    auto it = m.find(handle);
    if (it != m.end()) return it;
    uint32_t swapped = bswap32(handle);
    if (swapped != handle) return m.find(swapped);
    return m.end();
  }
}  // namespace

uint32_t fileHandleAlloc(std::FILE* f) {
  if (!f) return 0;
  std::lock_guard<std::mutex> lock(g_fileMutex);
  if (g_handleToFile.size() + g_handleToDirPath.size() >= kMaxHandles) return 0;
  uint32_t h = allocUnifiedHandle();
  g_handleToFile[h] = f;
  g_handleToIoLock[h] = std::make_shared<std::mutex>();
#ifdef _WIN32
  // Xenia XFile::XFile: async_event_ = CreateAutoResetEvent(false) (xfile.cc:23).
  g_handleWaitEvent[h] = CreateEventA(nullptr, FALSE, FALSE, nullptr);
#endif
  return h;
}

void fileHandleSetSynchronous(uint32_t handle, bool synchronous) {
  if (handle == 0) return;
  std::lock_guard<std::mutex> lock(g_fileMutex);
  auto it = findWithEndianFallback(g_handleToFile, handle);
  if (it != g_handleToFile.end()) g_handleSynchronous[it->first] = synchronous;
}

bool fileHandleIsSynchronous(uint32_t handle) {
  if (handle == 0) return true;
  std::lock_guard<std::mutex> lock(g_fileMutex);
  auto it = findWithEndianFallback(g_handleSynchronous, handle);
  return (it != g_handleSynchronous.end()) ? it->second : true;
}

void* fileHandleWaitEvent(uint32_t handle) {
  if (handle == 0) return nullptr;
  std::lock_guard<std::mutex> lock(g_fileMutex);
  auto it = findWithEndianFallback(g_handleWaitEvent, handle);
  return (it != g_handleWaitEvent.end()) ? it->second : nullptr;
}

uint32_t fileHandleAllocDir(const std::string& path) {
  if (path.empty()) return 0;
  std::lock_guard<std::mutex> lock(g_fileMutex);
  if (g_handleToFile.size() + g_handleToDirPath.size() >= kMaxHandles) return 0;
  uint32_t h = allocUnifiedHandle();
  g_handleToDirPath[h] = path;
  return h;
}

std::FILE* fileHandleGet(uint32_t handle) {
  if (handle == 0) return nullptr;
  std::lock_guard<std::mutex> lock(g_fileMutex);
  auto it = findWithEndianFallback(g_handleToFile, handle);
  return (it != g_handleToFile.end()) ? it->second : nullptr;
}

std::string fileHandleGetDir(uint32_t handle) {
  if (handle == 0) return {};
  std::lock_guard<std::mutex> lock(g_fileMutex);
  auto it = findWithEndianFallback(g_handleToDirPath, handle);
  return (it != g_handleToDirPath.end()) ? it->second : std::string{};
}

std::shared_ptr<std::mutex> fileHandleIoLock(uint32_t handle) {
  if (handle == 0) return nullptr;
  std::lock_guard<std::mutex> lock(g_fileMutex);
  auto it = findWithEndianFallback(g_handleToIoLock, handle);
  return (it != g_handleToIoLock.end()) ? it->second : nullptr;
}

void fileHandleClose(uint32_t handle) {
  if (handle == 0) return;
  // Wait for an in-flight read/write on this handle (its I/O lock) before the
  // FILE* goes away. g_fileMutex is NOT held while waiting: a reader holding the
  // I/O lock calls fileHandleGet (g_fileMutex) to re-validate.
  std::shared_ptr<std::mutex> io = fileHandleIoLock(handle);
  std::unique_lock<std::mutex> ioHold;
  if (io) ioHold = std::unique_lock<std::mutex>(*io);
  std::lock_guard<std::mutex> lock(g_fileMutex);
  auto it = findWithEndianFallback(g_handleToFile, handle);
  if (it != g_handleToFile.end()) {
    const uint32_t key = it->first;
    if (it->second) std::fclose(it->second);
    g_handleToFile.erase(it);
    g_handleToIoLock.erase(key);
    g_handleSynchronous.erase(key);
    auto ev = g_handleWaitEvent.find(key);
    if (ev != g_handleWaitEvent.end()) {
#ifdef _WIN32
      if (ev->second) CloseHandle(static_cast<HANDLE>(ev->second));
#endif
      g_handleWaitEvent.erase(ev);
    }
    return;
  }
  auto itd = findWithEndianFallback(g_handleToDirPath, handle);
  if (itd != g_handleToDirPath.end())
    g_handleToDirPath.erase(itd);
}

// Helpers for reading big-endian guest memory
static uint32_t readBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
static uint16_t readBE16(const uint8_t* p) {
  return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

// OBJECT_ATTRIBUTES (Xbox 360, big-endian):
//   Offset 0: ULONG Length (4)
//   Offset 4: HANDLE RootDirectory (4)
//   Offset 8: POBJECT_STRING ObjectName (4, pointer)
//   Offset 12: ULONG Attributes (4)
// ANSI_STRING / OBJECT_STRING (big-endian):
//   Offset 0: USHORT Length (2)
//   Offset 2: USHORT MaximumLength (2)
//   Offset 4: PCHAR Buffer (4, pointer)
static bool readGuestAnsiString(uint32_t namePtrVA, uint16_t& outLength, uint32_t& outBufferVA) {
  uint8_t* nameHost = guestVAToHost(namePtrVA);
  if (!nameHost || namePtrVA == 0) return false;
  // Guest memory is big-endian (PPC_STORE_U16/U32 write bswap'd values).
  // stub_RtlInitAnsiString also writes BE to match.
  // Read as big-endian, same as PPC_LOAD would.
  outLength = readBE16(nameHost + 0);
  outBufferVA = readBE32(nameHost + 4);
  {
    static int s_asDump = 0;
    if (++s_asDump <= 10)
      SR_LOG_KERNEL_FILE("ANSI_STRING @0x%08X: len=%u buf=0x%08X",
        namePtrVA, outLength, outBufferVA);
  }
  return true;
}

std::string resolveGuestPath(uint32_t objectAttributesVA) {
  uint8_t* objHost = guestVAToHost(objectAttributesVA);
  if (!objHost) return {};
  // Diagnostic: dump raw OBJECT_ATTRIBUTES bytes
  {
    static int s_dump = 0;
    if (++s_dump <= 5) {
      SR_LOG_KERNEL_FILE("resolveGuestPath: OA @0x%08X bytes: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
        objectAttributesVA,
        objHost[0], objHost[1], objHost[2], objHost[3],
        objHost[4], objHost[5], objHost[6], objHost[7],
        objHost[8], objHost[9], objHost[10], objHost[11],
        objHost[12], objHost[13], objHost[14], objHost[15]);
    }
  }
  // Xbox 360 OBJECT_ATTRIBUTES (no Length field, from Xenia/SDK):
  //   Offset 0: HANDLE RootDirectory (4)
  //   Offset 4: PANSI_STRING ObjectName (4, pointer)
  //   Offset 8: ULONG Attributes (4)
  // Guest memory is big-endian — read with readBE32.
  uint32_t namePtrVA = readBE32(objHost + 4);
  if (namePtrVA == 0 || !isGuestVAInRange(namePtrVA)) {
    static int s_noName = 0;
    if (++s_noName <= 3)
      SR_LOG_KERNEL_FILE("resolveGuestPath: OA=0x%08X namePtrVA=0x%08X (invalid)", objectAttributesVA, namePtrVA);
    return {};
  }
  uint16_t len;
  uint32_t bufVA;
  if (!readGuestAnsiString(namePtrVA, len, bufVA) || len == 0 || bufVA == 0) {
    static int s_badStr = 0;
    if (++s_badStr <= 3)
      SR_LOG_KERNEL_FILE("resolveGuestPath: OA=0x%08X namePtr=0x%08X len=%u bufVA=0x%08X", objectAttributesVA, namePtrVA, len, bufVA);
    return {};
  }
  uint8_t* bufHost = guestVAToHost(bufVA);
  if (!bufHost || len > 2048) return {};
  std::string path(reinterpret_cast<char*>(bufHost), len);
  // Log raw path for debugging
  {
    static int s_rawPathLog = 0;
    if (++s_rawPathLog <= 20)
      SR_LOG_KERNEL_FILE("resolveGuestPath: raw=\"%s\"", path.c_str());
  }
  // Replace backslashes with forward slashes first for consistent parsing
  for (char& c : path) if (c == '\\') c = '/';
  // Normalize: strip \??\ prefix
  while (path.size() >= 2 && path.substr(0, 2) == "/?") path = path.substr(2);
  while (!path.empty() && path[0] == '/') path = path.substr(1);
  // Xbox 360 DVD drive mapping: "d:/" or "D:/" -> disc content folder
  // On Xbox 360, D:\ is the DVD/disc drive containing game data
  if (path.size() >= 3 && (path[0] == 'd' || path[0] == 'D') && path[1] == ':' && path[2] == '/') {
    path = "Superman Returns (USA)/" + path.substr(3);
  }
  // "game:" or "game/" -> disc content folder
  else if (path.size() >= 5 && (path.substr(0, 5) == "game:" || path.substr(0, 5) == "game/"))
    path = "Superman Returns (USA)/" + path.substr(5);
  else if (path.size() >= 4 && path.substr(0, 4) == "game")
    path = "Superman Returns (USA)/" + path.substr(4);
  // Xbox-style bare paths (canary: DATA, default.xex) -> disc folder
  else if (path == "DATA" || path == "default.xex" ||
      (path.size() > 5 && path.substr(0, 5) == "DATA/") ||
      (path.size() > 11 && path.substr(0, 11) == "default.xex/"))
    path = "Superman Returns (USA)/" + path;
  // Make path absolute so file I/O works regardless of CWD
  const std::string& root = getContentRoot();
  if (!root.empty() && !path.empty() && path[0] != '/' && path.find(':') == std::string::npos)
    path = root + path;
  return path;
}

}  // namespace kernel
}  // namespace sr
