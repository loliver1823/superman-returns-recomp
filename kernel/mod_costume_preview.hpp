#pragma once
// Fortress COSTUMES screen previews for additive character packs (iter-465 fortress lane).
//
// The retail screen (ui.AST SCRCOSTUMES + components/ThumbViewer) loads each record's
// ThumbFile / FullSizeFile with Apt loadMovie: the retail records name
// artassets/Costumes/thumbs/CostumeThumb_<n>.swf and .../fullsize/CostumeFullSize_<n>.swf,
// which are the ui.AST movies COSTUMETHUMB_<n> (128x128) and COSTUMEFULLSIZE_<n> (512x512):
// one image character, a 128-byte texture header and DXT5 blocks (8in16-swapped, linear).
//
// A mod suit gets its own loose movie in the generated costume registry layer. The movie is
// the player's own disc COSTUMETHUMB_1 / COSTUMEFULLSIZE_1 container with only the image
// payload and the inner resource ids replaced, so no retail bytes are redistributed: the
// pack ships its preview as a plain DXT5 .dds (tools/srmods/costume_preview.py makes it from
// any image), and a suit without one gets the procedural fallback card below.
//
// Content composition only: no guest state, no guest calls.
#include <cstdint>
#include <string>
#include <vector>

namespace sr::kernel {

constexpr int kCostumeThumbDim = 128;
constexpr int kCostumeFullDim = 512;

// Little-endian DXT5 (BC3) blocks, row-major 4x4 blocks: dim*dim bytes.
// Reads a DDS whose pixel format is FourCC 'DXT5' with exactly dim x dim at mip 0.
bool modReadDdsDxt5(const std::string& hostPath, int dim, std::vector<uint8_t>* blocks,
                    std::string* err);

// Encodes RGBA8 (dim*dim*4, rows top-down) to little-endian DXT5 blocks.
void modEncodeDxt5(const uint8_t* rgba, int dim, std::vector<uint8_t>* blocks);

// The generic card shown for a suit whose pack has no preview (procedural, our own art).
void modCostumeFallbackRgba(int dim, std::vector<uint8_t>* rgba);

// tmpl = the disc movie container (COSTUMETHUMB_1 / COSTUMEFULLSIZE_1). Replaces the image
// payload with blocks (byte-swapped 8in16 like the retail payload) and the low/high id word of
// every nested record with innerTag, so two loaded preview movies never share resource ids.
bool modComposeCostumePreviewMovie(const std::vector<uint8_t>& tmpl, int dim,
                                   const std::vector<uint8_t>& blocks, uint32_t innerTag,
                                   std::vector<uint8_t>* out, std::string* err);

// Generated movie stems (<= 27 chars so "<stem>.swf" fits the retail 31-char name buffer).
std::string modCostumeThumbStem(const std::string& costumeId);
std::string modCostumeFullStem(const std::string& costumeId);
// The Apt path a record carries (the retail directory shape; loadMovie keys on the stem).
std::string modCostumeThumbPath(const std::string& stem);
std::string modCostumeFullPath(const std::string& stem);

}  // namespace sr::kernel
