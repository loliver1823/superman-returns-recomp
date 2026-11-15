#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

namespace sr {
namespace kernel {

// Unified kernel handle allocator: all kernel objects (files, directories, events,
// semaphores, threads) share this counter so handle IDs never collide.
// Xbox 360 has a single per-process handle table; we emulate that with a shared counter.
uint32_t allocUnifiedHandle();

// File handle table for NtCreateFile/NtOpenFile/NtReadFile/NtClose/NtQueryDirectoryFile.
// Handles are small non-zero IDs; table maps to FILE* (files) or directory path (dirs).

// Allocate a new handle for an open FILE*. Returns 0 on failure.
uint32_t fileHandleAlloc(std::FILE* f);

// Allocate a handle for a directory (path to list). Returns 0 on failure.
uint32_t fileHandleAllocDir(const std::string& path);

// Look up FILE* for handle. Returns nullptr if invalid, closed, or a directory handle.
std::FILE* fileHandleGet(uint32_t handle);

// If handle is a directory handle, return its path; otherwise empty string.
std::string fileHandleGetDir(uint32_t handle);

// Close and remove handle. Safe to call with invalid handle. Waits for any
// operation holding the handle's I/O lock (below) before it closes the FILE*.
void fileHandleClose(uint32_t handle);

// iter-463 platform fix (SR_ATOMIC_READ): the per-handle I/O lock, Xenia's
// XFile::file_lock_ (xenia kernel/xfile.cc:105, a std::lock_guard around every
// Read/ReadScatter/Write). Every operation that moves or uses the FILE*'s
// position (NtReadFile, NtQueryInformationFile, NtSetInformationFile, the
// xam_content write paths) holds it, so a positioned read is atomic against
// another thread's read on the same handle. nullptr for a directory handle or
// an unknown handle. After locking, re-check fileHandleGet(): the handle may
// have been closed while the caller waited (fileHandleClose takes this lock,
// fcloses the FILE* and drops the entry, so a FILE* fetched BEFORE locking
// may already be freed - always use the one fetched under the lock).
std::shared_ptr<std::mutex> fileHandleIoLock(uint32_t handle);

// iter-463 platform fix (SR_READ_PENDING): Xenia's XFile::is_synchronous_
// (xboxkrnl_io.cc:94-96: CreateOptions & (FILE_SYNCHRONOUS_IO_ALERT |
// FILE_SYNCHRONOUS_IO_NONALERT)). A handle whose flag was never set reads as
// synchronous, so callers that do not know the options (xam_content) keep the
// old behaviour. Unknown handle -> true.
void fileHandleSetSynchronous(uint32_t handle, bool synchronous);
bool fileHandleIsSynchronous(uint32_t handle);

// iter-463 platform fix (SR_READ_PENDING): the file object is waitable, as in
// Xenia (XFile::GetWaitHandle = async_event_, an auto-reset event Set after
// every read/write, xfile.cc:23, 198). Returns the host event HANDLE (as void*)
// or nullptr for a directory / unknown handle. Owned by the table; closed by
// fileHandleClose.
void* fileHandleWaitEvent(uint32_t handle);

// Map guest path (from OBJECT_ATTRIBUTES) to host path. ObjectAttributes at guest VA r5.
// Returns host path string, or empty if unreadable. Prefix "game:" -> "Superman Returns (USA)/".
std::string resolveGuestPath(uint32_t objectAttributesVA);

// The content root: the folder holding "Superman Returns (USA)/" (next to the
// exe, one level above it, or in the working directory). The install folder's
// name is unrestricted. UTF-8, forward slashes and a trailing '/'; "" if absent.
// Mods live under <content root>/mods/ (mod_overlay.hpp).
std::string contentRootDir();
// The same discovery without caching, exposed for relocation tests.
std::string contentRootForLocations(const std::string& exePath, const std::string& workingDir);
// Tests only: make contentRootDir() / resolveGuestPath() use `root` (UTF-8, forward slashes, trailing '/'); "" undoes
// it. A difftest builds its own disc tree with this (an isolated guest file system, not the repo's or the host's).
void setContentRootForTest(const std::string& root);

}  // namespace kernel
}  // namespace sr
