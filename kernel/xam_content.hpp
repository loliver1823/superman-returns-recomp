#pragma once
// iter-463 PLATFORM FIX (SR_XAM_CONTENT_FS, default ON; =0 restores every old
// stub exactly): XAM content packages (real save storage) backed by a host
// directory, plus the NT file calls the guest makes inside a mounted package
// ("rmcsave:\\Superman Returns") and RtlNtStatusToDosError (sub-gate
// SR_NTSTATUS_DOSERR, baked default ON 2026-10-18 by step4/audit fix batch 1
// after its natural A/B rolls - it changes GetLastError title-wide, not just
// for saves; =0 = the generic answer; effective only while SR_XAM_CONTENT_FS
// is on).
//
// The console provided this storage; the guest makes every decision (which
// package, which disposition, what bytes). Nothing here calls a guest function
// or writes state the guest did not ask for, so this is NOT forcing.
//
// Reference (BSD-3, semantics ported with attribution): xenia-canary @0b4e6c4
//   src/xenia/kernel/xam/xam_content.cc      (CreateEx/Close/Delete/GetCreator/
//                                              SetThumbnail/CreateEnumerator)
//   src/xenia/kernel/xam/content_manager.cc  (package + header layout)
//   src/xenia/kernel/xam/xam_enum.cc, kernel/xenumerator.cc (XamEnumerate)
//   src/xenia/vfs/virtual_file_system.cc     (OpenFile dispositions)
//   src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc (NtCreateFile/NtWriteFile/
//                                              NtQueryDirectoryFile)
//   src/xenia/kernel/xfile.cc                (QueryDirectory)
//   src/xenia/kernel/xboxkrnl/xboxkrnl_error.cc (RtlNtStatusToDosError table)
// Copyright Ben Vanik and Xenia contributors, BSD-3 licence.
//
// Host layout (Xenia's, so a Xenia content folder drops in):
//   <root>/<XUID %016X>/<TITLE %08X>/<TYPE %08X>/<file_name>/      package
//   <root>/<XUID>/<TITLE>/Headers/<TYPE>/<file_name>.header          0x148 B
//     (XCONTENT_AGGREGATE_DATA: XCONTENT_DATA 0x134, xuid @0x138, title @0x140)
//   <package>/__thumbnail.png
//   <root>/.backup/<XUID>/<TITLE>/<TYPE>/<file_name>/  host-only copy of the
//     package a CREATE_ALWAYS/TRUNCATE replaced (outside the enumerated tree;
//     the guest never reads it - a MANUAL recovery copy)
// A Xenia save drops in once its XUID folder is renamed to 0009000000000001
// (user 0). <root>/0000000000000000/ (common content) is LIST-ONLY: it
// enumerates, but CreateEx/GetCreator resolve under the caller's XUID, so a
// package there can never be opened (same as Xenia).
// <root> = SR_CONTENT_DIR, else (SR_INSTANCE_ID set = an automated roll) a
// fresh per-process scratch %TEMP%/SupermanRecomp/content-<id>-<pid>, else
// <exe dir>/content when <exe dir>/portable.txt exists, else
// %LOCALAPPDATA%/SupermanRecomp/content, else <exe dir>/content.
//
// Contract and evidence: docs/ENGINE_MAP.md (storage manager 0x8299FB18),
// docs/CUSTOM_FRONTEND.md section 8 part B.
//
// Kernel-only header: do NOT include from guest/ (and never add these
// declarations to guest_context.hpp - that header rebuilds all guest TUs).
#include <cstdint>
#include <filesystem>

namespace sr {
namespace kernel {

class ExportRegistry;

// The signed-in profile's XUID (stub_XamUserGetXUID, user 0) and this title's id
// (xex_loader does not expose it; Superman Returns USA = 0x454107ED).
constexpr uint64_t kXamUser0Xuid = 0x0009000000000001ull;
constexpr uint32_t kXamContentTitleId = 0x454107EDu;

// SR_XAM_CONTENT_FS (default ON).
bool xamContentFsEnabled();
// SR_NTSTATUS_DOSERR (baked default ON; =0 disables) AND xamContentFsEnabled().
bool ntStatusDosErrEnabled();
// Test seams: -1 = follow the env, 0 = force off, 1 = force on. With the env
// gate on at registration, "off" makes every export forward to the stub it
// replaced (exact old behaviour) and every Try* hook return false.
void xamContentFsSetEnabledForTest(int mode);
void ntStatusDosErrSetEnabledForTest(int mode);

// xeRtlNtStatusToDosError (xboxkrnl_error.cc:968), pure.
uint32_t ntStatusToDosError(uint32_t status);

// Called as the LAST line of registerStubs(): replaces XamContentCreateEx/
// Close/Flush/Delete/GetCreator/SetThumbnail/CreateEnumerator, XamEnumerate,
// NtWriteFile, NtFlushBuffersFile, NtQueryFullAttributesFile and
// RtlNtStatusToDosError. A no-op when SR_XAM_CONTENT_FS=0 at startup.
void registerXamContentExports(ExportRegistry& reg);

// stubs.cpp hooks (first line of the stub). Each returns false when the gate
// is off or the path/handle is not in a mounted package; the stub then runs
// unchanged. On true, *status is the NTSTATUS to return.
//   NtCreateFile(pH r3, access r4, OA r5, iosb r6, pAlloc r7, attrs r8;
//                share r9, disposition r10, CreateOptions at [r1+0x54])
bool xamContentTryNtCreateFile(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                               uint32_t r7, uint32_t r8, uint32_t* status);
//   NtOpenFile(pH, access, OA, iosb, share, options): disposition FILE_OPEN
bool xamContentTryNtOpenFile(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                             uint32_t r7, uint32_t r8, uint32_t* status);
//   NtSetInformationFile(h, iosb, info, len, class)
bool xamContentTrySetInformationFile(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                                     uint32_t r7, uint32_t r8, uint32_t* status);
//   NtQueryDirectoryFile(h, ev, apc, ctx, iosb, buf; len r9, ANSI mask* r10,
//                        RestartScan at [r1+0x54])
bool xamContentTryQueryDirectoryFile(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                                     uint32_t r7, uint32_t r8, uint32_t* status);
// stub_NtClose, after fileHandleClose: drops an enumerator or a package file
// (delete-on-close applied here). Lock-free when nothing of ours is live.
void xamContentOnNtClose(uint32_t handle);

// The content root (see above). Not created until something is written.
std::filesystem::path xamContentRoot();
// Free/total bytes of the volume holding the content root (its nearest
// existing ancestor). False when the query fails.
bool xamContentSpace(uint64_t* total, uint64_t* avail);

// Test seams: a "restart" - closes every package file handle and forgets all
// mounts, enumerators and known roots (the host tree is untouched).
void xamContentResetForTest();
uint32_t xamContentMountCountForTest();

}  // namespace kernel
}  // namespace sr
