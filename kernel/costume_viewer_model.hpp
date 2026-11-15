#pragma once
// iter-465 viewer lane: the COSTUME VIEWER's model data (docs/MODS.md "Costume viewer").
//
// Reads a suit's skinned body (an RW4 "xb2" arena: X2_SUPERMAN on the player's disc, or a pack's
// BODY.rx2), its cape (the cloth's RSS skinned surface + its DDS colour map), the Superman CCT
// skeleton and one clip of a SUPERMAN_FLIGHT animation bank, and evaluates the clip into skinning
// matrices. Pure host code: no guest memory, no guest calls, no D3D. Formats were decoded by the
// costume lane's Python tools (rw4/rw4.py, rw4model.py, tex360.py, export_gltf.py, cloth_rss.py;
// sranim/srbank.py, srskel.py; the owner's asset tools, not published); this is a C++ port of the parts
// a viewer needs. Nothing from the disc is written anywhere: everything is read at runtime from the
// player's own files.
//
// Conventions: matrices are 4x4 row-major float[16] for COLUMN vectors (v' = M v); the RW4 skin and
// the CCT store row-vector matrices, transposed on load.
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sr::kernel::cview {

using Mat4 = std::array<float, 16>;

struct Texture {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;   // w*h*4
};

struct Mesh {
  std::string name;
  std::vector<float> pos;      // 3 per vertex, bind (model) space
  std::vector<float> nrm;      // 3 per vertex
  std::vector<float> uv;       // 2 per vertex
  std::vector<uint16_t> joint; // 4 per vertex: model skeleton joint index
  std::vector<float> weight;   // 4 per vertex (sum 1)
  std::vector<uint32_t> idx;   // triangle list
  int texture = -1;            // index into Model::textures
  bool cape = false;           // the cloth surface (two-sided)
};

struct Skeleton {
  int n = 0;
  std::vector<int> parent;
  std::vector<uint32_t> hash;          // FNV-1 32 of the joint name
  std::vector<Mat4> invBind;           // column-vector inverse bind
  std::vector<Mat4> bindLocal;         // column-vector local bind (parent space)
  int find(uint32_t h) const;
};

// One clip sampled on the model skeleton: frames x n x 7 (qx qy qz qw tx ty tz), local, already
// filled with the bind local for every joint the clip does not drive.
struct Clip {
  int frames = 0;
  float keySpacing = 4.0f;     // 30 fps frames per key
  std::vector<float> trs;
  std::vector<uint8_t> driven; // per model joint: 1 = the clip drives it
};

struct Model {
  std::vector<Mesh> meshes;
  std::vector<Texture> textures;
  Skeleton skel;
  Clip clip;
  std::vector<uint8_t> capeDepth;  // per joint: 0 = not a cape bone, k = k-th link of a cape chain
  float boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0};  // posed frame 0
};

uint32_t fnv1_32(const std::string& s);
uint64_t fnv1a64Upper(const std::string& s);

// ── containers ─────────────────────────────────────────────────────────────────────────────
// A BGFA1.05 entry by name (or "#<hex id>"): top level from a file, nested from bytes. Entries
// stored RefPack-compressed are decompressed.
bool astReadFileEntry(const std::string& path, const std::string& name, std::vector<uint8_t>* out,
                      std::string* err);
bool astReadNested(const std::vector<uint8_t>& bgfa, const std::string& name, std::vector<uint8_t>* out,
                   std::string* err);
// Every nested entry's payload (decompressed); fn returns false to stop.
bool astForEachNested(const std::vector<uint8_t>& bgfa,
                      const std::function<bool(const std::vector<uint8_t>&)>& fn, std::string* err);
bool refpackDecompress(const uint8_t* p, size_t n, std::vector<uint8_t>* out);

// A nested entry's absolute offset and size in a container file (both levels uncompressed), for
// reading a big arena lazily.
bool astLocateNested(const std::string& path, const std::string& top, const std::string& nested, uint64_t* offset,
                     uint64_t* size, std::string* err);

// An arena read on demand: its header block in memory, the resource block through `read`
// (arena-relative offsets). Only the rasters a body binds are read from it.
struct LazyArena {
  std::vector<uint8_t> head;
  uint64_t size = 0;
  std::function<bool(uint64_t off, size_t n, uint8_t* dst)> read;
};

// ── parts ──────────────────────────────────────────────────────────────────────────────────
// The body: `arenas[0]` is the model arena; the rest are arenas its rasters may live in (resolved
// by arena id). LOD 0 meshes only. Appends to model->meshes / textures and sets model->skel.
bool loadBody(const std::vector<std::vector<uint8_t>>& arenas, Model* model, std::string* err,
              const std::vector<LazyArena>& lazy = {});
// The shared texture arena retail heads and hands may bind (metropol.AST
// METROPOLISWORLDFLOW_RENDER / METROPOLISWORLDFLOW_TEXTURE), as a lazy arena.
bool openLazyArena(const std::string& containerPath, const std::string& top, const std::string& nested, LazyArena* out,
                   std::string* err);
// The cape: an RSS skinned cloth surface; joints bound by name to the body skeleton.
bool loadCape(const std::vector<uint8_t>& rss, const std::vector<uint8_t>& dds, Model* model, std::string* err);
// A DDS (DXT1/3/5, A8R8G8B8) colour map -> RGBA.
bool decodeDds(const std::vector<uint8_t>& dds, Texture* out, std::string* err);
// The clip `clipName` of an animation bank on the CCT skeleton, mapped onto model->skel by hash.
bool loadClip(const std::vector<uint8_t>& cct, const std::vector<uint8_t>& bank, const std::string& clipName,
              Model* model, std::string* err);

// Skinning matrices (column-vector, 3x4 rows = 12 floats per joint) at `seconds` into the looping
// clip; `sway` (radians) adds a slow procedural swing to the cape chains.
void evalSkin(const Model& model, double seconds, float sway, std::vector<float>* skin3x4);
// CPU skinning of one mesh into interleaved pos3 nrm3 uv2 (8 floats per vertex).
void skinMesh(const Mesh& mesh, const std::vector<float>& skin3x4, std::vector<float>* out);
// Posed bounds over every mesh (for framing).
void posedBounds(const Model& model, const std::vector<float>& skin3x4, float mn[3], float mx[3]);

}  // namespace sr::kernel::cview
