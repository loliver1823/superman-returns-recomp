// iter-465 viewer lane: offline check of kernel/costume_viewer_model.cpp on the player's disc.
// usage: sr_costume_viewer_test <disc root> <suit> <out.bmp> [clip] [seconds] [yawDeg]
//   suit: retail1 | retail2 | retail3 | bizarro | pack:<pack dir>|<prefix>
// Prints the model statistics and writes a software-rasterized BMP (texture x lambert) of the
// posed suit, so the decode can be judged without the game. Reads only; writes only <out.bmp>.
#include "../../kernel/costume_viewer_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace sr::kernel::cview;

static bool readFile(const std::string& p, std::vector<uint8_t>* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}
static bool nested(const std::vector<uint8_t>& b, const char* n, std::vector<uint8_t>* o) {
  std::string e;
  if (astReadNested(b, n, o, &e)) return true;
  std::fprintf(stderr, "%s: %s\n", n, e.c_str());
  return false;
}
static std::vector<std::vector<uint8_t>> arenasOf(const std::vector<uint8_t>& bundle, const std::vector<uint8_t>& first) {
  std::vector<std::vector<uint8_t>> v{first};
  std::string e;
  astForEachNested(bundle, [&](const std::vector<uint8_t>& p) {
    if (p.size() > 8 && p[0] == 0x89 && p[1] == 'R' && p[2] == 'W' && p[3] == '4' && p != first) v.push_back(p);
    return true;
  }, &e);
  return v;
}

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: %s disc suit out.bmp [clip] [seconds] [yaw]\n", argv[0]); return 2; }
  const std::string disc = argv[1], suit = argv[2], outPath = argv[3];
  const std::string clipName = argc > 4 ? argv[4] : "Hover";
  const double secs = argc > 5 ? std::atof(argv[5]) : 0.0;
  const float yaw = argc > 6 ? float(std::atof(argv[6])) * 3.14159265f / 180.0f : 0.0f;
  std::string err;
  Model model;
  std::vector<uint8_t> top, render, other, body, rss, dds, preload, pother;
  const std::string objtiv = disc + "/DATA/objtiv.AST";
  bool ok = false;
  if (suit.rfind("retail", 0) == 0 || suit == "bizarro") {
    const bool biz = suit == "bizarro";
    const char* topName = biz ? "PLAYABLEBIZ" : "SUPERMENACTORWORLDFLOW";
    const char* model3[] = {"X2_SUPERMAN", "X2_SUPERMANGRAY", "X2_SUPERMAN_ALT"};
    const char* tex3[] = {"X2_SUPERMAN_BOOTSUNDIESCAPE_COL", "", "X2_SUPERMANALT_BOOTSUNDIESCAPE_COL"};
    const int r = biz ? 0 : std::atoi(suit.c_str() + 6) - 1;
    if (!astReadFileEntry(objtiv, topName, &top, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    if (!nested(top, biz ? "PLAYABLEBIZ_RENDER" : "SUPERMENACTORWORLDFLOW_RENDER", &render) ||
        !nested(top, biz ? "PLAYABLEBIZ_OTHER" : "SUPERMENACTORWORLDFLOW_OTHER", &other) ||
        !nested(render, biz ? "X2_BIZARRO" : model3[r < 0 || r > 2 ? 0 : r], &body)) return 1;
    ok = loadBody(arenasOf(render, body), &model, &err);
    if (!ok) { std::fprintf(stderr, "body: %s\n", err.c_str()); return 1; }
    const char* texName = biz ? "X2_BIZARRO_BOOTSUNDIESCAPE_COL" : tex3[r < 0 || r > 2 ? 0 : r];
    if (*texName) {
      if (astReadFileEntry(disc + "/DATA/preload.AST", "PRELOAD_OTHER", &preload, &err) &&
          nested(preload, "SUPERMANBOUND360CAPE", &rss) && nested(other, texName, &dds)) {
        if (!loadCape(rss, dds, &model, &err)) std::fprintf(stderr, "cape: %s\n", err.c_str());
      } else std::fprintf(stderr, "cape sources: %s\n", err.c_str());
    }
  } else if (suit.rfind("pack:", 0) == 0) {
    const size_t c2 = suit.find('|', 5);
    const std::string dir = suit.substr(5, c2 - 5), prefix = suit.substr(c2 + 1);
    if (!readFile(dir + "/" + prefix + "BODY.rx2", &body)) { std::fprintf(stderr, "no body\n"); return 1; }
    ok = loadBody({body}, &model, &err);
    if (!ok) { std::fprintf(stderr, "body: %s\n", err.c_str()); return 1; }
    if (readFile(dir + "/" + prefix + "CAPE_RSS.rss", &rss)) {
      readFile(dir + "/" + prefix + "CAPE_COL.dds", &dds);
      if (!loadCape(rss, dds, &model, &err)) std::fprintf(stderr, "cape: %s\n", err.c_str());
    }
  }
  std::vector<uint8_t> bw, anim, cct, bank;
  if (!astReadFileEntry(disc + "/DATA/baseworl.AST", "BASEWORLDFLOW_ANIM", &anim, &err) ||
      !nested(anim, "SUPERMANCCTPROJECT", &cct) || !nested(anim, "SUPERMAN_FLIGHT", &bank)) {
    std::fprintf(stderr, "anim: %s\n", err.c_str()); return 1;
  }
  if (const char* bf = std::getenv("VIEWER_BANK")) readFile(bf, &bank);
  if (!loadClip(cct, bank, clipName, &model, &err)) std::fprintf(stderr, "clip: %s\n", err.c_str());
  int drivenCount = 0;
  for (auto d : model.clip.driven) drivenCount += d;
  size_t tris = 0, verts = 0;
  for (const auto& m : model.meshes) { tris += m.idx.size() / 3; verts += m.pos.size() / 3; }
  std::printf("joints %d meshes %zu verts %zu tris %zu textures %zu clip frames %d spacing %.2f driven %d\n",
              model.skel.n, model.meshes.size(), verts, tris, model.textures.size(), model.clip.frames,
              model.clip.keySpacing, drivenCount);
  for (const auto& m : model.meshes) {
    const Texture* t = m.texture >= 0 ? &model.textures[size_t(m.texture)] : nullptr;
    std::printf("  %-20s v %6zu t %6zu tex %dx%d\n", m.name.c_str(), m.pos.size() / 3, m.idx.size() / 3, t ? t->w : 0, t ? t->h : 0);
  }
  std::vector<float> sk;
  evalSkin(model, secs, 0.0f, &sk);
  float mn[3], mx[3];
  posedBounds(model, sk, mn, mx);
  std::printf("posed bounds x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f\n", mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
  // software raster: orthographic, Y up, looking down -Z (front), rotated by yaw about Y
  const int W = 600, H = 800;
  std::vector<uint8_t> img(size_t(W) * H * 3, 60);
  std::vector<float> zb(size_t(W) * H, 1e30f);
  const float cx = 0.5f * (mn[0] + mx[0]), cy = 0.5f * (mn[1] + mx[1]), cz = 0.5f * (mn[2] + mx[2]);
  const float ext = std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]}) * 0.55f;
  const float sc = (H * 0.5f) / ext, cyw = std::cos(yaw), syw = std::sin(yaw);
  std::vector<float> v;
  for (const auto& m : model.meshes) {
    skinMesh(m, sk, &v);
    const Texture* t = m.texture >= 0 ? &model.textures[size_t(m.texture)] : nullptr;
    auto proj = [&](size_t i, float* o) {
      const float x = v[i * 8] - cx, y = v[i * 8 + 1] - cy, z = v[i * 8 + 2] - cz;
      const float rx = cyw * x + syw * z, rz = -syw * x + cyw * z;
      o[0] = W * 0.5f + rx * sc; o[1] = H * 0.5f - y * sc; o[2] = -rz;
    };
    for (size_t k = 0; k + 2 < m.idx.size(); k += 3) {
      float P[3][3];
      for (int e = 0; e < 3; ++e) proj(m.idx[k + e], P[e]);
      const float area = (P[1][0] - P[0][0]) * (P[2][1] - P[0][1]) - (P[2][0] - P[0][0]) * (P[1][1] - P[0][1]);
      if (std::fabs(area) < 1e-6f) continue;
      const int x0 = std::max(0, int(std::min({P[0][0], P[1][0], P[2][0]}))), x1 = std::min(W - 1, int(std::max({P[0][0], P[1][0], P[2][0]})) + 1);
      const int y0 = std::max(0, int(std::min({P[0][1], P[1][1], P[2][1]}))), y1 = std::min(H - 1, int(std::max({P[0][1], P[1][1], P[2][1]})) + 1);
      for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
        const float px = x + 0.5f, py = y + 0.5f;
        float w0 = ((P[1][0] - px) * (P[2][1] - py) - (P[2][0] - px) * (P[1][1] - py)) / area;
        float w1 = ((P[2][0] - px) * (P[0][1] - py) - (P[0][0] - px) * (P[2][1] - py)) / area;
        float w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        const float z = w0 * P[0][2] + w1 * P[1][2] + w2 * P[2][2];
        if (z >= zb[size_t(y) * W + x]) continue;
        zb[size_t(y) * W + x] = z;
        const uint32_t i0 = m.idx[k], i1 = m.idx[k + 1], i2 = m.idx[k + 2];
        float n[3], uv[2];
        for (int c = 0; c < 3; ++c) n[c] = w0 * v[i0 * 8 + 3 + c] + w1 * v[i1 * 8 + 3 + c] + w2 * v[i2 * 8 + 3 + c];
        for (int c = 0; c < 2; ++c) uv[c] = w0 * v[i0 * 8 + 6 + c] + w1 * v[i1 * 8 + 6 + c] + w2 * v[i2 * 8 + 6 + c];
        const float nx = cyw * n[0] + syw * n[2], nz = -syw * n[0] + cyw * n[2];
        const float nl = std::sqrt(nx * nx + n[1] * n[1] + nz * nz) + 1e-6f;
        const float lam = 0.35f + 0.65f * std::fabs((nx * 0.3f + n[1] * 0.5f + nz * 0.8f) / nl);
        uint8_t col[3] = {200, 200, 200};
        if (t) {
          float u = uv[0] - std::floor(uv[0]), vv = uv[1] - std::floor(uv[1]);
          const int tx = std::min(t->w - 1, int(u * t->w)), ty = std::min(t->h - 1, int(vv * t->h));
          for (int c = 0; c < 3; ++c) col[c] = t->rgba[(size_t(ty) * t->w + tx) * 4 + c];
        }
        uint8_t* d = &img[(size_t(y) * W + x) * 3];
        for (int c = 0; c < 3; ++c) d[2 - c] = uint8_t(std::min(255.0f, col[c] * lam));
      }
    }
  }
  std::ofstream o(outPath, std::ios::binary);
  const uint32_t rowB = (W * 3 + 3) & ~3u, fsz = 54 + rowB * H;
  uint8_t hdr[54] = {'B', 'M'};
  auto put = [&](int off, uint32_t val) { for (int i = 0; i < 4; ++i) hdr[off + i] = uint8_t(val >> (8 * i)); };
  put(2, fsz); put(10, 54); put(14, 40); put(18, W); put(22, H); hdr[26] = 1; hdr[28] = 24; put(34, rowB * H);
  o.write(reinterpret_cast<char*>(hdr), 54);
  std::vector<uint8_t> row(rowB, 0);
  for (int y = H - 1; y >= 0; --y) { std::memcpy(row.data(), &img[size_t(y) * W * 3], size_t(W) * 3); o.write(reinterpret_cast<char*>(row.data()), rowB); }
  std::printf("wrote %s\n", outPath.c_str());
  return 0;
}
