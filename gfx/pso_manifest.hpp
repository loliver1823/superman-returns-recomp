#pragma once
// Step 4 item 2k(3b) (lane D, 2026-10-20): the PIPELINE MANIFEST - the translated pipelines a session created, kept
// between runs so a later boot can create them before a draw asks (gfx/pso_warm.cpp, SR_PSO_WARM).
//
// One file: a shader table (each bytecode once, by its content hash - NativeShaderCode::hash) and the pipeline
// descriptions in first-use order, every field of NativePipelineDesc except the root signature (the warm-up
// supplies the translated root it was recorded under). Content-keyed like the cache itself: an entry whose
// shaders a later translator no longer emits just never meets a draw (no special case for the alpha-test pixel
// variants either - an AT pipeline is its own entry with the variant's bytecode). The file carries its own
// bytecode, so reading it needs no shader-cache lookup. Malformed input is rejected whole (never partly used).
//
// Standalone (no logging, no device): tools/difftest/pso_manifest_test.cpp links it with native_pipeline.cpp.
#include "native_pipeline.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sr::gfx::psom {

constexpr uint32_t kVersion = 1;
constexpr size_t kMaxEntries = 65536;
// Astra (10:5x) #3: the most a manifest file may be (read and parsed); a larger one is ignored whole.
constexpr uint64_t kMaxFileBytes = 64ull << 20;

// A pipeline's identity without its root signature: the five shader content hashes + every state field (the same
// fields NativePipelineCache keys on). Equal identities = the same pipeline for the manifest's merge.
uint64_t identity(const NativePipelineDesc& desc);

// The file image of `descs` (their root signatures are ignored; shaders stored once each). fix12: within the reader's
// limits - an entry parse() would reject (no vertex shader, a shader over 64 MB, more than 32 inputs, a semantic of 0
// or over 255 bytes, state that does not read back) is left out (`skipped`); an image over kMaxEntries, the shader
// limit or kMaxFileBytes is not made: {} and `why`.
std::vector<uint8_t> serialize(const std::vector<const NativePipelineDesc*>& descs, std::string* why = nullptr,
                               size_t* skipped = nullptr);
// The descriptions of a file image (root signatures left empty; equal shaders share one NativeShaderCode).
// false = malformed or another version (`why` says which); `out` is then empty.
bool parse(std::span<const uint8_t> bytes, std::vector<NativePipelineDesc>& out, std::string* why = nullptr);
// `existing` (in its order) followed by each of `added` whose identity is not already there, at most kMaxEntries.
std::vector<const NativePipelineDesc*> merge(const std::vector<NativePipelineDesc>& existing,
                                             const std::vector<const NativePipelineDesc*>& added);

// false = missing, unreadable, or larger than maxBytes (`why` says which; nothing is read past the cap).
bool readFile(const std::string& path, std::vector<uint8_t>& out, std::string* why = nullptr,
              uint64_t maxBytes = kMaxFileBytes);
// Written to an instance-unique temporary (<path>.<pid>.<tick>.tmp, created exclusively), then renamed over <path> (a
// reader never sees a half-written manifest; two writers never share a temporary).
bool writeFileAtomic(const std::string& path, std::span<const uint8_t> bytes);
// Astra (10:5x) #4: the cross-process reread-merge-replace. Under an exclusive lock on <path>.lock (LockFileEx, waited
// at most lockMs): the file as it is NOW (another instance may have added entries since this one booted), then `base`
// (this instance's own boot-time read - kept even if another build removed the file), then `added`, merged by
// identity, written atomically. false = the lock or the write failed, or the merged image is over the reader's
// limits (the previous file is then kept as it is) - `why`; the caller retries at its next write.
struct MergeResult {
  size_t onDisk = 0, total = 0, bytes = 0, skipped = 0;   // skipped: entries over the reader's limits, left out
};
bool mergeIntoFile(const std::string& path, const std::vector<NativePipelineDesc>& base,
                   const std::vector<const NativePipelineDesc*>& added, MergeResult* result, std::string* why,
                   uint32_t lockMs = 3000);

}  // namespace sr::gfx::psom
