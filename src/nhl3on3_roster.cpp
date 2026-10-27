// Applies roster/roster.json on startup. Same edits as tools/roster_mod.py:
// custom players take an original arcade-star slot, and a headshot in
// roster/headshots/ replaces that slot's portrait. Uncompressed portraits
// (unpacked size 0); a RefPack blob larger than its payload crashes the
// switch list.

#include "nhl3on3_roster.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../thirdparty/stb_image.h"

namespace nhl3on3 {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

constexpr const char* kMarkerName = "data0.nhl3on3";
constexpr const char* kStateName = "data0.roster";
constexpr int kTemplateId = 1082;
// Height/weight and fields that travel with the type template (label + build).
constexpr const char* kBody[] = {"PQgy", "WZNs", "jfuZ", "QDTK", "GDhI", "QBpy", "dhKk"};

std::string g_online_id = "stock";

const char* kTeams[] = {
    "ANA", "ATL", "BOS", "BUF", "CGY", "CAR", "CHI", "COL", "CBJ", "DAL", "DET", "EDM", "FLA",
    "LA",  "MIN", "MTL", "NSH", "NJ",  "NYI", "NYR", "OTT", "PHI", "PHX", "PIT", "STL", "SJ",
    "TB",  "TOR", "VAN", "WSH"};
const char* kPositions[] = {"C", "LW", "RW", "D", "G"};
const char* kKeep[] = {
    "Sidney Crosby", "Alexander Ovechkin", "Evgeni Malkin", "Patrick Kane", "Jonathan Toews",
    "Nicklas Lidstrom", "Zdeno Chara", "Henrik Lundqvist", "Martin Brodeur", "Pavel Datsyuk",
    "Jarome Iginla", "Joe Thornton", "Henrik Zetterberg", "Shea Weber", "Anze Kopitar",
    "Vincent Lecavalier", "Ilya Kovalchuk", "Chris Pronger"};
const char* kClaim[] = {
    "Nikolai Antropov", "Sean Avery", "Brad Boyes", "Kristian Huselius", "Alexander Frolov",
    "Andrew Cogliano", "Milan Lucic", "Jeremy Roenick", "Shane Doan", "Nathan Horton",
    "Alex Kovalev", "Daniel Briere", "Jean-Sebastien Giguere", "Rick DiPietro", "Roberto Luongo",
    "Mike Modano", "Joe Sakic", "Dion Phaneuf", "Mike Green", "Thomas Vanek", "Saku Koivu",
    "Jason Spezza", "Eric Staal", "Dany Heatley", "Marian Gaborik"};

constexpr const char* kFirst = "PedH";
constexpr const char* kLast = "RMbQ";
constexpr const char* kBirthplace = "JzFM";
constexpr const char* kId = "Nzao";
constexpr const char* kAsset = "rnOl";
constexpr const char* kKey = "zIBw";
constexpr const char* kFace = "DaPp";
constexpr const char* kTeam = "BSXd";
constexpr const char* kNhl = "WBbd";
constexpr const char* kPos = "aljv";
constexpr const char* kNumber = "tRVs";
constexpr const char* kShoots = "pkRG";
constexpr const char* kYear = "dnFq";
constexpr const char* kMonth = "pLKJ";
constexpr const char* kDay = "iwsK";

struct Player {
  std::string first, last, team, position, type, headshot, replaces, shoots, birth, place;
  int number = 0;
};

struct Image {
  int width = 0, height = 0;
  Bytes rgba;
};

[[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }

std::optional<Bytes> ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return Bytes(std::istreambuf_iterator<char>(in), {});
}

bool WriteFile(const fs::path& path, const uint8_t* data, size_t size) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  return static_cast<bool>(out);
}

uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void StoreBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

uint32_t Crc32Mpeg2(const uint8_t* data, size_t size) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i << 24;
      for (int k = 0; k < 8; ++k) {
        c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
      }
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    c = (c << 8) ^ table[((c >> 24) ^ data[i]) & 0xFF];
  }
  return c;
}

std::string Lower(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (unsigned char c : text) {
    out.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : char(c));
  }
  return out;
}

std::string Trim(std::string text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
  return text;
}

std::string Norm(std::string_view text) { return Trim(Lower(text)); }

char GroupOf(int position) { return position == 4 ? 'G' : position == 3 ? 'D' : 'F'; }

// --- PNG / DXT5 ----------------------------------------------------------------

std::optional<Bytes> Unpack(const uint8_t* in, size_t n) {
  if (n < 2 || in[1] != 0xFB || ((in[0] & 0xFE) != 0x10 && (in[0] & 0xFE) != 0x90)) {
    return Bytes(in, in + n);
  }
  const size_t size_len = (in[0] & 0x80) ? 4 : 3;
  size_t p = 2 + ((in[0] & 0x01) ? size_len : 0);
  if (p + size_len > n) return std::nullopt;
  size_t out_size = 0;
  for (size_t i = 0; i < size_len; ++i) out_size = (out_size << 8) | in[p + i];
  p += size_len;
  Bytes out;
  out.reserve(out_size);
  auto literal = [&](size_t count) {
    if (p + count > n) return false;
    out.insert(out.end(), in + p, in + p + count);
    p += count;
    return true;
  };
  while (p < n) {
    const uint8_t b0 = in[p];
    size_t length = 0, offset = 0;
    if (b0 < 0x80) {
      if (p + 2 > n) return std::nullopt;
      const uint8_t b1 = in[p + 1];
      p += 2;
      if (!literal(b0 & 3)) return std::nullopt;
      length = ((b0 >> 2) & 7) + 3;
      offset = ((b0 & 0x60) << 3) + b1 + 1;
    } else if (b0 < 0xC0) {
      if (p + 3 > n) return std::nullopt;
      const uint8_t b1 = in[p + 1], b2 = in[p + 2];
      p += 3;
      if (!literal(b1 >> 6)) return std::nullopt;
      length = (b0 & 0x3F) + 4;
      offset = ((b1 & 0x3F) << 8) + b2 + 1;
    } else if (b0 < 0xE0) {
      if (p + 4 > n) return std::nullopt;
      const uint8_t b1 = in[p + 1], b2 = in[p + 2], b3 = in[p + 3];
      p += 4;
      if (!literal(b0 & 3)) return std::nullopt;
      length = ((b0 & 0x0C) << 6) + b3 + 5;
      offset = ((b0 & 0x10) << 12) + (uint32_t(b1) << 8) + b2 + 1;
    } else if (b0 < 0xFC) {
      p += 1;
      if (!literal(((b0 & 0x1F) << 2) + 4)) return std::nullopt;
      continue;
    } else {
      p += 1;
      if (!literal(b0 & 3)) return std::nullopt;
      break;
    }
    if (offset > out.size()) return std::nullopt;
    const size_t start = out.size() - offset;
    for (size_t i = 0; i < length; ++i) out.push_back(out[start + i]);
  }
  if (out.size() != out_size) return std::nullopt;
  return out;
}

int TiledBlock(int x, int y, int width_blocks) {
  constexpr int log_bpp = 4;
  const int aligned = (width_blocks + 31) & ~31;
  const int macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7);
  const int micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpp;
  const int off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return (((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F)) >>
         log_bpp;
}

void AlphaPalette(int a0, int a1, int pal[8]) {
  pal[0] = a0;
  pal[1] = a1;
  if (a0 > a1) {
    for (int k = 0; k < 6; ++k) pal[2 + k] = ((6 - k) * a0 + (k + 1) * a1) / 7;
  } else {
    for (int k = 0; k < 4; ++k) pal[2 + k] = ((4 - k) * a0 + (k + 1) * a1) / 5;
    pal[6] = 0;
    pal[7] = 255;
  }
}

void Rgb565(int c, int rgb[3]) {
  rgb[0] = (c >> 11) * 255 / 31;
  rgb[1] = ((c >> 5) & 63) * 255 / 63;
  rgb[2] = (c & 31) * 255 / 31;
}

int To565(int r, int g, int b) {
  return ((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255);
}

Bytes EncodeDxt5(const Bytes& rgba, int width, int height) {
  const int bw = width / 4, bh = height / 4;
  Bytes out(size_t(bw * bh * 16));
  for (int by = 0; by < bh; ++by) {
    for (int bx = 0; bx < bw; ++bx) {
      uint8_t px[16][4];
      for (int k = 0; k < 16; ++k) {
        const int o = ((by * 4 + k / 4) * width + bx * 4 + k % 4) * 4;
        std::memcpy(px[k], rgba.data() + o, 4);
      }
      int a0 = 0, a1 = 255;
      for (auto& p : px) {
        a0 = std::max(a0, int(p[3]));
        a1 = std::min(a1, int(p[3]));
      }
      uint64_t abits = 0;
      if (a0 != a1) {
        int pal[8];
        AlphaPalette(a0, a1, pal);
        for (int k = 0; k < 16; ++k) {
          int best = 0, best_d = 256;
          for (int j = 0; j < 8; ++j) {
            const int d = std::abs(pal[j] - px[k][3]);
            if (d < best_d) {
              best_d = d;
              best = j;
            }
          }
          abits |= uint64_t(best) << (3 * k);
        }
      }
      int count = 0;
      int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
      double mean[3] = {};
      for (auto& p : px) {
        if (p[3] == 0) continue;
        ++count;
        for (int c = 0; c < 3; ++c) {
          lo[c] = std::min(lo[c], int(p[c]));
          hi[c] = std::max(hi[c], int(p[c]));
          mean[c] += p[c];
        }
      }
      if (count == 0) {
        count = 16;
        lo[0] = lo[1] = lo[2] = 255;
        hi[0] = hi[1] = hi[2] = 0;
        mean[0] = mean[1] = mean[2] = 0;
        for (auto& p : px) {
          for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], int(p[c]));
            hi[c] = std::max(hi[c], int(p[c]));
            mean[c] += p[c];
          }
        }
      }
      for (int c = 0; c < 3; ++c) mean[c] /= count;
      for (int c : {0, 2}) {
        double cov = 0;
        for (auto& p : px) {
          if (p[3] == 0 && count != 16) continue;
          cov += (p[c] - mean[c]) * (p[1] - mean[1]);
        }
        if (cov < 0) std::swap(lo[c], hi[c]);
      }
      int c0 = To565(hi[0], hi[1], hi[2]), c1 = To565(lo[0], lo[1], lo[2]);
      if (c0 < c1) std::swap(c0, c1);
      uint32_t cbits = 0;
      if (c0 != c1) {
        int pal[4][3];
        Rgb565(c0, pal[0]);
        Rgb565(c1, pal[1]);
        for (int c = 0; c < 3; ++c) {
          pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
          pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
        }
        for (int k = 0; k < 16; ++k) {
          int best = 0, best_d = 1 << 30;
          for (int j = 0; j < 4; ++j) {
            int d = 0;
            for (int c = 0; c < 3; ++c) d += (pal[j][c] - px[k][c]) * (pal[j][c] - px[k][c]);
            if (d < best_d) {
              best_d = d;
              best = j;
            }
          }
          cbits |= uint32_t(best) << (2 * k);
        }
      }
      uint8_t block[16];
      block[0] = uint8_t(a0);
      block[1] = uint8_t(a1);
      for (int i = 0; i < 6; ++i) block[2 + i] = uint8_t((abits >> (8 * i)) & 0xFF);
      block[8] = uint8_t(c0);
      block[9] = uint8_t(c0 >> 8);
      block[10] = uint8_t(c1);
      block[11] = uint8_t(c1 >> 8);
      for (int i = 0; i < 4; ++i) block[12 + i] = uint8_t((cbits >> (8 * i)) & 0xFF);
      std::swap(block[0], block[1]);
      for (int i = 2; i < 16; i += 2) std::swap(block[i], block[i + 1]);
      const int at = TiledBlock(bx, by, bw) * 16;
      std::memcpy(out.data() + at, block, 16);
    }
  }
  return out;
}

Bytes Resize(const Bytes& rgba, int width, int height, int new_width, int new_height) {
  const double sx = double(width) / new_width, sy = double(height) / new_height;
  Bytes out(size_t(new_width * new_height * 4));
  for (int y = 0; y < new_height; ++y) {
    const double y0 = y * sy, y1 = (y + 1) * sy;
    for (int x = 0; x < new_width; ++x) {
      const double x0 = x * sx, x1 = (x + 1) * sx;
      double r = 0, g = 0, b = 0, a = 0, area = 0;
      for (int j = int(y0); j < height && j < y1 + 0.999999; ++j) {
        const double wy = std::min(y1, j + 1.0) - std::max(y0, double(j));
        if (wy <= 0) continue;
        for (int i = int(x0); i < width && i < x1 + 0.999999; ++i) {
          const double wx = std::min(x1, i + 1.0) - std::max(x0, double(i));
          if (wx <= 0) continue;
          const double w = wx * wy;
          const int o = (j * width + i) * 4;
          const double pa = rgba[size_t(o + 3)] * w;
          r += rgba[size_t(o)] * pa;
          g += rgba[size_t(o + 1)] * pa;
          b += rgba[size_t(o + 2)] * pa;
          a += pa;
          area += w;
        }
      }
      const size_t o = size_t(y * new_width + x) * 4;
      if (a > 0) {
        out[o] = uint8_t(std::lround(r / a));
        out[o + 1] = uint8_t(std::lround(g / a));
        out[o + 2] = uint8_t(std::lround(b / a));
        out[o + 3] = uint8_t(std::lround(a / area));
      }
    }
  }
  return out;
}

Bytes Place(const Bytes& rgba, int width, int height, int canvas_w, int canvas_h, int x, int y) {
  Bytes out(size_t(canvas_w * canvas_h * 4));
  for (int row = 0; row < height; ++row) {
    const int cy = y + row;
    if (cy < 0 || cy >= canvas_h) continue;
    const int left = std::max(0, -x), right = std::min(width, canvas_w - x);
    if (left >= right) continue;
    std::memcpy(out.data() + (cy * canvas_w + x + left) * 4, rgba.data() + (row * width + left) * 4,
                size_t(right - left) * 4);
  }
  return out;
}

Image ReadPng(const fs::path& path) {
  const auto file = ReadFile(path);
  if (!file) Fail("could not read " + path.string());
  int w = 0, h = 0, n = 0;
  uint8_t* pixels = stbi_load_from_memory(file->data(), int(file->size()), &w, &h, &n, 4);
  if (!pixels) Fail(path.string() + " is not a readable PNG");
  Image image;
  image.width = w;
  image.height = h;
  image.rgba.assign(pixels, pixels + size_t(w) * size_t(h) * 4);
  stbi_image_free(pixels);
  return image;
}

// --- Archive -------------------------------------------------------------------

struct BigEntry {
  std::string name;
  uint32_t offset = 0, size = 0, unpacked = 0;
  std::optional<Bytes> replaced;
};

struct NamedBig {
  std::string name;
  uint32_t offset = 0, size = 0;
};

std::vector<NamedBig> BigEntries(const Bytes& data) {
  if (data.size() < 16 || (std::memcmp(data.data(), "BIGF", 4) && std::memcmp(data.data(), "BIG4", 4))) {
    Fail("not a BIG archive");
  }
  const uint32_t count = LoadBE32(data.data() + 8);
  std::vector<NamedBig> out;
  size_t p = 16;
  for (uint32_t i = 0; i < count; ++i) {
    if (p + 8 > data.size()) Fail("truncated BIG directory");
    NamedBig entry;
    entry.offset = LoadBE32(data.data() + p);
    entry.size = LoadBE32(data.data() + p + 4);
    p += 8;
    const auto end = std::find(data.begin() + ptrdiff_t(p), data.end(), 0);
    if (end == data.end()) Fail("truncated BIG name");
    entry.name.assign(reinterpret_cast<const char*>(data.data() + p), end - (data.begin() + ptrdiff_t(p)));
    p = size_t(end - data.begin()) + 1;
    out.push_back(std::move(entry));
  }
  return out;
}

uint32_t Djb2(std::string_view name) {
  uint32_t h = 5381;
  for (unsigned char c : Lower(name)) h = h * 33 + c;
  return h;
}

class Archive {
 public:
  explicit Archive(Bytes big, const Bytes& bh) : big_(std::move(big)) {
    if (big_.size() < 16) Fail("data0.big is too small");
    const uint32_t count = LoadBE32(big_.data() + 8);
    size_t p = 16;
    uint32_t first = UINT32_MAX, end = 0;
    for (uint32_t k = 0; k < count; ++k) {
      BigEntry entry;
      entry.offset = LoadBE32(big_.data() + p);
      entry.size = LoadBE32(big_.data() + p + 4);
      p += 8;
      const auto nul = std::find(big_.begin() + ptrdiff_t(p), big_.end(), 0);
      if (nul == big_.end()) Fail("truncated data0.big directory");
      entry.name.assign(reinterpret_cast<const char*>(big_.data() + p), nul - (big_.begin() + ptrdiff_t(p)));
      p = size_t(nul - big_.begin()) + 1;
      if (bh.size() < 16 + 16 * (k + 1)) Fail("data0.bh is too small");
      const uint32_t bh_off = LoadBE32(bh.data() + 16 + 16 * k);
      entry.unpacked = LoadBE32(bh.data() + 16 + 16 * k + 8);
      if (bh_off != entry.offset) Fail("data0.bh does not match data0.big");
      first = std::min(first, entry.offset);
      end = std::max(end, entry.offset + entry.size);
      index_[entry.name] = entries_.size();
      entries_.push_back(std::move(entry));
    }
    trailer_.assign(big_.begin() + ptrdiff_t(p), big_.begin() + ptrdiff_t(std::min(p + 8, big_.size())));
    first_data_ = first;
    data_end_ = end;
  }

  bool Contains(const std::string& name) const { return index_.contains(name); }

  Bytes Read(const std::string& name) {
    auto& entry = entries_.at(index_.at(name));
    if (entry.replaced) return *entry.replaced;
    auto unpacked = Unpack(big_.data() + entry.offset, entry.size);
    if (!unpacked) Fail("could not unpack " + name);
    return *unpacked;
  }

  void Put(const std::string& name, Bytes data) {
    if (auto it = index_.find(name); it != index_.end()) {
      entries_[it->second].unpacked = 0;
      entries_[it->second].replaced = std::move(data);
      return;
    }
    BigEntry entry;
    entry.name = name;
    entry.replaced = std::move(data);
    index_[name] = entries_.size();
    entries_.push_back(std::move(entry));
  }

  std::pair<Bytes, Bytes> Save() {
    size_t directory = 0;
    for (const auto& entry : entries_) directory += 8 + entry.name.size() + 1;
    size_t shift = 0;
    if (16 + directory + trailer_.size() > first_data_) {
      shift = 16 + directory + trailer_.size() - first_data_;
    }
    shift = (shift + 4095) & ~size_t(4095);
    Bytes big(first_data_ + shift);
    big.insert(big.end(), big_.begin() + ptrdiff_t(first_data_), big_.begin() + ptrdiff_t(data_end_));
    for (auto& entry : entries_) {
      if (!entry.replaced) {
        entry.offset += uint32_t(shift);
      } else {
        big.insert(big.end(), (64 - big.size() % 64) % 64, 0);
        entry.offset = uint32_t(big.size());
        entry.size = uint32_t(entry.replaced->size());
        entry.unpacked = 0;
        big.insert(big.end(), entry.replaced->begin(), entry.replaced->end());
      }
    }
    size_t p = 16;
    for (const auto& entry : entries_) {
      StoreBE32(big.data() + p, entry.offset);
      StoreBE32(big.data() + p + 4, entry.size);
      std::memcpy(big.data() + p + 8, entry.name.data(), entry.name.size());
      big[p + 8 + entry.name.size()] = 0;
      p += 8 + entry.name.size() + 1;
    }
    std::memcpy(big.data() + p, trailer_.data(), trailer_.size());
    std::memcpy(big.data(), "BIG4", 4);
    const uint32_t total = uint32_t(big.size());
    for (int i = 0; i < 4; ++i) big[4 + i] = uint8_t(total >> (8 * i));
    StoreBE32(big.data() + 8, uint32_t(entries_.size()));
    StoreBE32(big.data() + 12, uint32_t(p + 8));

    Bytes bh(16 + 16 * entries_.size() + 16);
    std::memcpy(bh.data(), "Viv4", 4);
    const uint32_t bh_size = uint32_t(bh.size());
    for (int i = 0; i < 4; ++i) bh[4 + i] = uint8_t(bh_size >> (8 * i));
    StoreBE32(bh.data() + 8, uint32_t(entries_.size()));
    StoreBE32(bh.data() + 12, bh_size);
    for (size_t k = 0; k < entries_.size(); ++k) {
      const auto& entry = entries_[k];
      uint8_t* at = bh.data() + 16 + 16 * k;
      StoreBE32(at, entry.offset);
      StoreBE32(at + 4, entry.size);
      StoreBE32(at + 8, entry.unpacked);
      StoreBE32(at + 12, Djb2(entry.name));
    }
    StoreBE32(bh.data() + bh.size() - 16, uint32_t(big.size()));
    return {std::move(big), std::move(bh)};
  }

 private:
  Bytes big_;
  Bytes trailer_;
  std::vector<BigEntry> entries_;
  std::map<std::string, size_t> index_;
  size_t first_data_ = 0, data_end_ = 0;
};

Bytes Portrait(Archive& archive, const std::string& folder, int tw, int th, int dw, int dh,
               const Image& image) {
  Bytes nested = archive.Read(folder + "/p" + std::to_string(kTemplateId) + ".big");
  size_t tex = 0;
  bool found = false;
  for (const auto& entry : BigEntries(nested)) {
    if (entry.size == 4096u + uint32_t(tw * th) && entry.offset + 4 <= nested.size() &&
        std::memcmp(nested.data() + entry.offset, "\x89RW4", 4) == 0) {
      tex = entry.offset;
      found = true;
      break;
    }
  }
  if (!found) Fail(folder + " template has no texture");
  const int scaled_w = std::max(dw, int(std::lround(double(image.width) * dh / image.height)));
  Bytes scaled = Resize(image.rgba, image.width, image.height, scaled_w, dh);
  Bytes cropped = Place(scaled, scaled_w, dh, dw, dh, -((scaled_w - dw) / 2), 0);
  Bytes canvas = Place(cropped, dw, dh, tw, th, 0, 0);
  Bytes encoded = EncodeDxt5(canvas, tw, th);
  std::memcpy(nested.data() + tex + 4096, encoded.data(), encoded.size());
  return nested;
}

// --- Database ------------------------------------------------------------------

struct Field {
  int type = 0;
  uint32_t bit_offset = 0, bits = 0;
};

struct Table {
  std::string name;
  size_t offset = 0, record_size = 0, capacity = 0, count = 0, data_offset = 0;
  std::map<std::string, Field> fields;
  size_t end() const { return data_offset + record_size * capacity; }
};

struct Value {
  bool text = false;
  int64_t number = 0;
  std::string str;
};

class Database {
 public:
  explicit Database(Bytes data) : data_(std::move(data)) {
    if (data_.size() < 0x18 || data_[0] != 'D' || data_[1] != 'B') Fail("nhlng.db is not a TDB database");
    const uint32_t count = LoadBE32(data_.data() + 0x10);
    index_end_ = 0x18 + 8 * count;
    for (uint32_t i = 0; i < count; ++i) {
      Table table;
      table.name.assign(reinterpret_cast<const char*>(data_.data() + 0x18 + 8 * i), 4);
      const uint32_t rel = LoadBE32(data_.data() + 0x1C + 8 * i);
      table.offset = index_end_ + rel;
      table.record_size = LoadBE32(data_.data() + table.offset + 8);
      table.capacity = (uint32_t(data_[table.offset + 20]) << 8) | data_[table.offset + 21];
      table.count = (uint32_t(data_[table.offset + 22]) << 8) | data_[table.offset + 23];
      const int nfields = data_[table.offset + 28];
      size_t p = table.offset + 40;
      for (int f = 0; f < nfields; ++f) {
        Field field;
        field.type = int(LoadBE32(data_.data() + p));
        field.bit_offset = LoadBE32(data_.data() + p + 4);
        std::string name(reinterpret_cast<const char*>(data_.data() + p + 8), 4);
        field.bits = LoadBE32(data_.data() + p + 12);
        table.fields.emplace(std::move(name), field);
        p += 16;
      }
      table.data_offset = p;
      order_.push_back(table.name);
      tables_.emplace(table.name, std::move(table));
    }
  }

  Table& operator[](const std::string& name) { return tables_.at(name); }

  int64_t GetInt(const Table& table, size_t row, const std::string& field) const {
    const Field& f = table.fields.at(field);
    const uint8_t* rec = data_.data() + table.data_offset + row * table.record_size;
    if (f.type == 0) Fail(table.name + "." + field + " is a string");
    uint64_t value = 0;
    for (uint32_t i = 0; i < f.bits; ++i) {
      const uint32_t bit = f.bit_offset + i;
      const int bit_in = 7 - int(bit % 8);
      value = (value << 1) | ((rec[bit / 8] >> bit_in) & 1);
    }
    if (f.type == 2 && f.bits > 0 && (value >> (f.bits - 1))) value -= uint64_t(1) << f.bits;
    return int64_t(value);
  }

  std::string GetStr(const Table& table, size_t row, const std::string& field) const {
    const Field& f = table.fields.at(field);
    const uint8_t* rec = data_.data() + table.data_offset + row * table.record_size;
    const uint8_t* start = rec + f.bit_offset / 8;
    const uint8_t* stop = rec + (f.bit_offset + f.bits) / 8;
    const uint8_t* nul = std::find(start, stop, 0);
    return std::string(reinterpret_cast<const char*>(start), reinterpret_cast<const char*>(nul));
  }

  void SetInt(Table& table, size_t row, const std::string& field, int64_t value) {
    const Field& f = table.fields.at(field);
    if (f.type == 0) Fail(table.name + "." + field + " is a string");
    uint64_t bits = uint64_t(value);
    if (value < 0) {
      if (f.type != 2 || value < -(int64_t(1) << (f.bits - 1))) {
        Fail(table.name + "." + field + " out of range");
      }
      bits = uint64_t(value + (int64_t(1) << f.bits));
    } else if (bits >= (uint64_t(1) << f.bits)) {
      Fail(table.name + "." + field + " out of range");
    }
    uint8_t* rec = data_.data() + table.data_offset + row * table.record_size;
    for (uint32_t i = 0; i < f.bits; ++i) {
      const uint32_t bit = f.bit_offset + f.bits - 1 - i;
      const int bit_in = 7 - int(bit % 8);
      const uint8_t bit_value = uint8_t((bits >> i) & 1);
      rec[bit / 8] = uint8_t((rec[bit / 8] & ~(1 << bit_in)) | (bit_value << bit_in));
    }
  }

  void SetStr(Table& table, size_t row, const std::string& field, const std::string& value) {
    const Field& f = table.fields.at(field);
    const size_t size = f.bits / 8;
    if (value.size() >= size) Fail(table.name + "." + field + " is too long");
    uint8_t* at = data_.data() + table.data_offset + row * table.record_size + f.bit_offset / 8;
    std::memset(at, 0, size);
    std::memcpy(at, value.data(), value.size());
  }

  void Checksums() {
    auto store = [&](size_t at, uint32_t crc) { StoreBE32(data_.data() + at, crc); };
    store(0x14, Crc32Mpeg2(data_.data(), 0x14));
    const uint8_t* previous = data_.data() + 0x18;
    size_t previous_size = index_end_ - 0x18;
    for (const auto& name : order_) {
      Table& table = tables_.at(name);
      store(table.offset, Crc32Mpeg2(previous, previous_size));
      store(table.offset + 36, Crc32Mpeg2(data_.data() + table.offset + 4, 32));
      previous = data_.data() + table.offset + 40;
      previous_size = table.end() - (table.offset + 40);
    }
    store(data_.size() - 4, Crc32Mpeg2(previous, previous_size));
  }

  const Bytes& bytes() const { return data_; }

 private:
  Bytes data_;
  size_t index_end_ = 0;
  std::map<std::string, Table> tables_;
  std::vector<std::string> order_;
};

struct Row {
  std::map<std::string, Value> values;
};

class Players {
 public:
  explicit Players(Database& db) : db_(db), table_(db["cPbu"]) {
    rows_.resize(table_.count);
    for (size_t r = 0; r < table_.count; ++r) {
      for (const auto& [name, field] : table_.fields) {
        Value value;
        if (field.type == 0) {
          value.text = true;
          value.str = db_.GetStr(table_, r, name);
        } else {
          value.number = db_.GetInt(table_, r, name);
        }
        rows_[r].values.emplace(name, std::move(value));
      }
    }
    for (const char* name : {"yvSd", "ajmx", "yuHm"}) {
      Table& table = db_[name];
      auto& index = ratings_[name];
      for (size_t r = 0; r < table.count; ++r) index[db_.GetInt(table, r, kKey)] = r;
    }
  }

  int64_t Num(size_t row, const char* field) const { return rows_[row].values.at(field).number; }
  const std::string& Str(size_t row, const char* field) const { return rows_[row].values.at(field).str; }

  void SetNum(size_t row, const char* field, int64_t value) {
    db_.SetInt(table_, row, field, value);
    rows_[row].values[field].number = value;
    rows_[row].values[field].text = false;
  }
  void SetStr(size_t row, const char* field, const std::string& value) {
    db_.SetStr(table_, row, field, value);
    rows_[row].values[field].str = value;
    rows_[row].values[field].text = true;
  }

  std::vector<const char*> RatingTables(size_t row) const {
    return Num(row, kPos) == 4 ? std::vector<const char*>{"yuHm"}
                               : std::vector<const char*>{"yvSd", "ajmx"};
  }

  bool HasRatings(size_t row) const {
    const int64_t key = Num(row, kKey);
    for (const char* name : RatingTables(row)) {
      if (!ratings_.at(name).contains(key)) return false;
    }
    return true;
  }

  double Overall(size_t row) {
    const char* name = RatingTables(row)[0];
    Table& table = db_[name];
    const size_t r = ratings_.at(name).at(Num(row, kKey));
    double sum = 0;
    int n = 0;
    for (const auto& [field, desc] : table.fields) {
      if (desc.bits == 6 && desc.type != 0) {
        sum += db_.GetInt(table, r, field);
        ++n;
      }
    }
    return n ? sum / n : 0;
  }

  void CopyRatings(size_t source, size_t target) {
    for (const char* name : RatingTables(source)) {
      Table& table = db_[name];
      const size_t src = ratings_.at(name).at(Num(source, kKey));
      const size_t dst = ratings_.at(name).at(Num(target, kKey));
      for (const auto& [field, desc] : table.fields) {
        if (field == kKey || desc.type == 0) continue;
        db_.SetInt(table, dst, field, db_.GetInt(table, src, field));
      }
    }
  }

  void CopyBody(size_t source, size_t target) {
    for (const char* field : kBody) SetNum(target, field, Num(source, field));
  }

  std::optional<size_t> Find(std::string_view first, std::string_view last) const {
    const auto want_first = Norm(first), want_last = Norm(last);
    std::vector<size_t> hits;
    for (size_t r = 0; r < rows_.size(); ++r) {
      if (Norm(Str(r, kFirst)) == want_first && Norm(Str(r, kLast)) == want_last) hits.push_back(r);
    }
    std::stable_sort(hits.begin(), hits.end(), [&](size_t a, size_t b) {
      const bool a_free = Num(a, kNhl) == 0, b_free = Num(b, kNhl) == 0;
      if (a_free != b_free) return !a_free;
      return HasRatings(a) && !HasRatings(b);
    });
    if (hits.empty()) return std::nullopt;
    return hits[0];
  }

  std::string Name(size_t row) const { return Str(row, kFirst) + " " + Str(row, kLast); }

  Database& db_;
  Table& table_;
  std::vector<Row> rows_;
  std::map<std::string, std::map<int64_t, size_t>> ratings_;
};

// --- Roster JSON ---------------------------------------------------------------

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(char(cp));
  } else if (cp < 0x800) {
    out.push_back(char(0xC0 | (cp >> 6)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(char(0xE0 | (cp >> 12)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  }
}

struct Json {
  const std::string& text;
  size_t i = 0;
  void SkipWs() {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  }
  char Peek() {
    SkipWs();
    return i < text.size() ? text[i] : 0;
  }
  std::string String() {
    SkipWs();
    if (text[i] != '"') Fail("roster.json: expected a string");
    ++i;
    std::string out;
    while (i < text.size() && text[i] != '"') {
      if (text[i] == '\\') {
        ++i;
        if (i >= text.size()) Fail("roster.json: bad escape");
        if (text[i] == 'u') {
          if (i + 4 >= text.size()) Fail("roster.json: bad unicode escape");
          uint32_t cp = 0;
          for (int k = 1; k <= 4; ++k) {
            const char c = text[i + k];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp += uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') cp += uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp += uint32_t(c - 'A' + 10);
            else Fail("roster.json: bad unicode escape");
          }
          AppendUtf8(out, cp);
          i += 5;
          continue;
        }
        const char e = text[i];
        out.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
        ++i;
      } else {
        out.push_back(text[i++]);
      }
    }
    if (i >= text.size()) Fail("roster.json: unterminated string");
    ++i;
    return out;
  }
  void Skip() {
    SkipWs();
    if (text[i] == '"') {
      String();
      return;
    }
    if (text[i] == '{' || text[i] == '[') {
      const char open = text[i], close = open == '{' ? '}' : ']';
      int depth = 0;
      bool in_str = false;
      for (; i < text.size(); ++i) {
        if (in_str) {
          if (text[i] == '\\') ++i;
          else if (text[i] == '"') in_str = false;
          continue;
        }
        if (text[i] == '"') in_str = true;
        else if (text[i] == open) ++depth;
        else if (text[i] == close && --depth == 0) {
          ++i;
          return;
        }
      }
      Fail("roster.json: truncated value");
    }
    while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) &&
           text[i] != ',' && text[i] != '}' && text[i] != ']') {
      ++i;
    }
  }
};

std::vector<Player> ParseRoster(const std::string& text) {
  Json json{text, 0};
  const auto at = text.find("\"players\"");
  if (at == std::string::npos) Fail("roster.json has no players");
  json.i = at + 9;
  if (json.Peek() != ':') Fail("roster.json: expected players");
  ++json.i;
  if (json.Peek() != '[') Fail("roster.json: players is not a list");
  ++json.i;
  std::vector<Player> players;
  while (json.Peek() && json.Peek() != ']') {
    if (json.Peek() != '{') Fail("roster.json: expected a player");
    ++json.i;
    Player player;
    while (json.Peek() && json.Peek() != '}') {
      const std::string key = json.String();
      if (json.Peek() != ':') Fail("roster.json: expected ':'");
      ++json.i;
      auto take_str = [&](std::string& dest) {
        if (json.Peek() == 'n') json.Skip();
        else dest = json.String();
      };
      if (key == "first_name") take_str(player.first);
      else if (key == "last_name") take_str(player.last);
      else if (key == "team") take_str(player.team);
      else if (key == "position") take_str(player.position);
      else if (key == "type") take_str(player.type);
      else if (key == "headshot") take_str(player.headshot);
      else if (key == "replaces") take_str(player.replaces);
      else if (key == "shoots") take_str(player.shoots);
      else if (key == "birth_date") take_str(player.birth);
      else if (key == "birthplace") take_str(player.place);
      else if (key == "jersey_number") {
        json.SkipWs();
        player.number = std::stoi(text.substr(json.i));
        json.Skip();
      } else {
        json.Skip();
      }
      json.SkipWs();
      if (json.text[json.i] == ',') ++json.i;
    }
    if (json.Peek() == '}') ++json.i;
    players.push_back(std::move(player));
    json.SkipWs();
    if (json.i < json.text.size() && json.text[json.i] == ',') ++json.i;
  }
  return players;
}

int IndexOf(const char* const* list, size_t n, const std::string& value) {
  for (size_t i = 0; i < n; ++i) {
    if (value == list[i]) return int(i);
  }
  return -1;
}

std::string TemplateFor(const Player& player, char group) {
  const std::string type = player.position == "G" ? "all_around" : player.type;
  if (group == 'G') return "Henrik Lundqvist";
  if (group == 'D' && type == "fast") return "Mike Green";
  if (group == 'D' && type == "strong") return "Zdeno Chara";
  if (group == 'D') return "Nicklas Lidstrom";
  if (type == "fast") return "Patrick Kane";
  if (type == "strong") return "Jarome Iginla";
  return "Sidney Crosby";
}

fs::path FindRosterJson() {
  std::error_code ec;
  for (auto dir = rex::filesystem::GetExecutableFolder(); !dir.empty(); dir = dir.parent_path()) {
    const auto candidate = dir / "roster" / "roster.json";
    if (fs::is_regular_file(candidate, ec)) return candidate;
    if (dir == dir.parent_path()) break;
  }
  return {};
}

std::string ReadVersion(const fs::path& roster_dir) {
  const auto bytes = ReadFile(roster_dir / "version.txt");
  return bytes ? Trim(std::string(bytes->begin(), bytes->end())) : "0";
}

// Content-only fingerprint so the same zip matches on every machine. The UI
// marker is included for the on-disk apply state (portraits sit on top of it).
std::string Fingerprint(const fs::path& roster_path, const std::vector<Player>& players,
                        const std::string& marker) {
  std::string out = marker;
  out.push_back('\n');
  out += "version=";
  out += ReadVersion(roster_path.parent_path());
  out.push_back('\n');
  std::error_code ec;
  for (const auto& player : players) {
    if (player.headshot.empty()) continue;
    const fs::path path = roster_path.parent_path() / player.headshot;
    if (!fs::is_regular_file(path, ec)) continue;
    const auto file = ReadFile(path);
    out += player.headshot;
    out.push_back(' ');
    out += std::to_string(file ? Crc32Mpeg2(file->data(), file->size()) : 0);
    out.push_back('\n');
  }
  const auto json = ReadFile(roster_path);
  out += std::to_string(json ? Crc32Mpeg2(json->data(), json->size()) : 0);
  return out;
}

// Online id ignores the UI marker so the same roster pack matches across port
// builds. "stock" when nothing custom is present.
std::string OnlineIdFrom(const fs::path& roster_path, const std::vector<Player>& players,
                         bool applied) {
  if (!applied) return "stock";
  std::string out = "version=";
  out += ReadVersion(roster_path.parent_path());
  out.push_back('\n');
  std::error_code ec;
  for (const auto& player : players) {
    if (player.headshot.empty()) continue;
    const fs::path path = roster_path.parent_path() / player.headshot;
    if (!fs::is_regular_file(path, ec)) continue;
    const auto file = ReadFile(path);
    out += player.headshot;
    out.push_back(' ');
    out += std::to_string(file ? Crc32Mpeg2(file->data(), file->size()) : 0);
    out.push_back('\n');
  }
  const auto json = ReadFile(roster_path);
  out += std::to_string(json ? Crc32Mpeg2(json->data(), json->size()) : 0);
  char hex[9];
  std::snprintf(hex, sizeof(hex), "%08x", Crc32Mpeg2(reinterpret_cast<const uint8_t*>(out.data()),
                                                     out.size()));
  return hex;
}

void Apply(const fs::path& game_dir, const fs::path& roster_path) {
  const auto json_bytes = ReadFile(roster_path);
  if (!json_bytes) Fail("could not read " + roster_path.string());
  const std::string json(json_bytes->begin(), json_bytes->end());
  auto roster = ParseRoster(json);
  for (size_t i = 0; i < roster.size(); ++i) {
    const auto& player = roster[i];
    const std::string who = "player " + std::to_string(i + 1) + " (" + player.first + " " + player.last + ")";
    if (player.first.empty() || player.last.empty() || player.team.empty() || player.position.empty()) {
      Fail(who + " is missing a name, team or position");
    }
    if (IndexOf(kTeams, std::size(kTeams), player.team) < 0) Fail(who + ": unknown team");
    if (IndexOf(kPositions, std::size(kPositions), player.position) < 0) Fail(who + ": unknown position");
    if (player.position != "G" && player.type != "all_around" && player.type != "fast" &&
        player.type != "strong") {
      Fail(who + ": type must be all_around, fast or strong");
    }
    if (player.number < 0 || player.number > 99) Fail(who + ": jersey_number must be 0-99");
  }

  const auto marker_bytes = ReadFile(game_dir / kMarkerName);
  const std::string marker = marker_bytes ? Trim(std::string(marker_bytes->begin(), marker_bytes->end())) : "";
  if (marker.empty()) Fail("front-end mod marker is missing");
  const std::string fingerprint = Fingerprint(roster_path, roster, marker);
  // Already applied: still publish the online id (content hash, no UI marker).
  auto set_online = [&](bool applied) {
    g_online_id = OnlineIdFrom(roster_path, roster, applied);
  };
  if (const auto applied = ReadFile(game_dir / kStateName);
      applied && std::string(applied->begin(), applied->end()) == fingerprint) {
    set_online(true);
    return;
  }

  const fs::path big_path = game_dir / "data0.big", bh_path = game_dir / "data0.bh";
  const fs::path db_path = game_dir / "db" / "nhlng.db";
  const fs::path big_base = fs::path(big_path.string() + ".preroster");
  const fs::path bh_base = fs::path(bh_path.string() + ".preroster");
  const fs::path db_base = fs::path(db_path.string() + ".orig");
  std::error_code ec;
  if (!fs::is_regular_file(db_base, ec)) fs::copy_file(db_path, db_base, ec);
  const auto state_file = ReadFile(game_dir / kStateName);
  const std::string state = state_file ? std::string(state_file->begin(), state_file->end()) : "";
  const auto newline = state.find('\n');
  const std::string state_marker = newline == std::string::npos ? Trim(state) : state.substr(0, newline);
  if (state_marker != marker || !fs::is_regular_file(big_base, ec)) {
    fs::copy_file(big_path, big_base, fs::copy_options::overwrite_existing, ec);
    fs::copy_file(bh_path, bh_base, fs::copy_options::overwrite_existing, ec);
    if (ec) Fail("could not back up data0.big");
  }
  auto big = ReadFile(big_base);
  auto bh = ReadFile(bh_base);
  auto db_bytes = ReadFile(db_base);
  if (!big || !bh || !db_bytes) Fail("could not read the roster backups");

  Archive archive(std::move(*big), *bh);
  const std::string head_dir = "fe/ion/artassets/playerheads/p0_4000";
  auto has_portrait = [&](int64_t id) { return archive.Contains(head_dir + "/p" + std::to_string(id) + ".big"); };
  std::set<int64_t> stars;
  for (int id = 0; id < 4000; ++id) {
    if (has_portrait(id)) stars.insert(id);
  }

  Database db(std::move(*db_bytes));
  Players players(db);
  std::vector<Player> ready;
  std::vector<std::string> skipped;
  for (const auto& player : roster) {
    const fs::path shot = roster_path.parent_path() / player.headshot;
    const auto existing = players.Find(player.first, player.last);
    if (fs::is_regular_file(shot, ec) ||
        (existing && stars.contains(players.Num(*existing, kId)))) {
      ready.push_back(player);
    } else if (!player.first.empty()) {
      skipped.push_back(player.first + " " + player.last);
    }
  }
  if (ready.empty()) {
    g_online_id = "stock";
    REXLOG_INFO("[roster] {} has no headshots in {} yet", roster_path.string(),
                (roster_path.parent_path() / "headshots").string());
    return;
  }
  set_online(true);

  std::set<std::string> keep;
  for (const char* name : kKeep) keep.insert(Norm(name));
  for (const auto& player : ready) {
    keep.insert(Norm(player.first + " " + player.last));
    if (!player.replaces.empty()) keep.insert(Norm(player.replaces));
  }
  auto find_star = [&](const std::string& name) -> std::optional<size_t> {
    const auto space = name.find(' ');
    const auto first = space == std::string::npos ? name : name.substr(0, space);
    const auto last = space == std::string::npos ? std::string() : name.substr(space + 1);
    if (auto row = players.Find(first, last); row && stars.contains(players.Num(*row, kId))) return row;
    for (size_t r = 0; r < players.rows_.size(); ++r) {
      if (stars.contains(players.Num(r, kId)) && Norm(players.Name(r)) == Norm(name)) return r;
    }
    return std::nullopt;
  };
  std::vector<size_t> claimable;
  std::set<int64_t> seen;
  auto consider = [&](size_t row) {
    if (!stars.contains(players.Num(row, kId)) || seen.contains(players.Num(row, kKey))) return;
    if (keep.contains(Norm(players.Name(row)))) return;
    claimable.push_back(row);
    seen.insert(players.Num(row, kKey));
  };
  for (const char* name : kClaim) {
    if (auto row = find_star(name)) consider(*row);
  }
  for (size_t r = 0; r < players.rows_.size(); ++r) consider(r);

  std::set<int64_t> protected_keys;
  struct Applied {
    Player player;
    size_t row;
  };
  std::vector<Applied> applied;
  for (const auto& player : ready) {
    const int team = IndexOf(kTeams, std::size(kTeams), player.team);
    const int position = IndexOf(kPositions, std::size(kPositions), player.position);
    const std::string name = player.first + " " + player.last;
    const auto existing = players.Find(player.first, player.last);
    size_t row = 0;
    if (!player.replaces.empty()) {
      auto found = find_star(player.replaces);
      if (!found) Fail(name + " replaces " + player.replaces + ", who is not an arcade star");
      row = *found;
      REXLOG_INFO("[roster] {} replaces {} (id {})", name, player.replaces, players.Num(row, kId));
    } else if (existing && stars.contains(players.Num(*existing, kId))) {
      row = *existing;
      REXLOG_INFO("[roster] {} keeps arcade id {}", name, players.Num(row, kId));
    } else {
      const char group = GroupOf(position);
      auto it = std::find_if(claimable.begin(), claimable.end(), [&](size_t candidate) {
        return GroupOf(int(players.Num(candidate, kPos))) == group;
      });
      if (it == claimable.end()) {
        REXLOG_WARN("[roster] {} skipped: no arcade {} slot left (the game has 32 forward, 6 "
                    "defense and 5 goalie star slots)",
                    name, player.position);
        continue;
      }
      row = *it;
      claimable.erase(it);
      REXLOG_INFO("[roster] {} takes arcade slot of {} (id {})", name, players.Name(row),
                  players.Num(row, kId));
      if (existing && *existing != row) {
        const std::string disp_first = players.Str(row, kFirst), disp_last = players.Str(row, kLast);
        players.SetStr(*existing, kFirst, disp_first);
        players.SetStr(*existing, kLast, disp_last);
        players.SetNum(*existing, kTeam, 0);
        players.SetNum(*existing, kNhl, 0);
        protected_keys.insert(players.Num(*existing, kKey));
      }
    }
    protected_keys.insert(players.Num(row, kKey));
    // Always take the JSON type's ratings and build, including players who
    // were already arcade stars (Forsberg, Selanne, …).
    players.SetNum(row, kPos, position);
    const auto template_name = TemplateFor(player, GroupOf(position));
    const auto space = template_name.find(' ');
    if (auto source = players.Find(template_name.substr(0, space), template_name.substr(space + 1))) {
      players.CopyRatings(*source, row);
      players.CopyBody(*source, row);
    }
    if (players.Num(row, kNhl) != team + 1) {
      std::optional<size_t> worst;
      for (size_t r = 0; r < players.rows_.size(); ++r) {
        if (players.Num(r, kNhl) != team + 1 || players.Num(r, kTeam) != team) continue;
        if (GroupOf(int(players.Num(r, kPos))) != GroupOf(position)) continue;
        if (protected_keys.contains(players.Num(r, kKey)) || !players.HasRatings(r)) continue;
        if (!worst || players.Overall(r) < players.Overall(*worst)) worst = r;
      }
      if (worst) {
        players.SetNum(*worst, kTeam, 0);
        players.SetNum(*worst, kNhl, 0);
        protected_keys.insert(players.Num(*worst, kKey));
        REXLOG_INFO("[roster]   {} leaves {} for free agency", players.Name(*worst), player.team);
      }
    }
    players.SetStr(row, kFirst, player.first);
    players.SetStr(row, kLast, player.last);
    players.SetNum(row, kTeam, team);
    players.SetNum(row, kNhl, team + 1);
    players.SetNum(row, kPos, position);
    players.SetNum(row, kNumber, player.number);
    if (!player.shoots.empty()) {
      const auto side = Lower(player.shoots);
      if (side != "left" && side != "right" && side != "l" && side != "r") {
        Fail(name + ": shoots must be left or right");
      }
      players.SetNum(row, kShoots, side == "left" || side == "l" ? 0 : 1);
    }
    players.SetNum(row, kFace, players.Num(row, kId));
    players.SetNum(row, kAsset, players.Num(row, kId));
    if (player.birth.size() >= 10) {
      players.SetNum(row, kYear, std::stoi(player.birth.substr(0, 4)) - 1900);
      players.SetNum(row, kMonth, std::stoi(player.birth.substr(5, 2)));
      players.SetNum(row, kDay, std::stoi(player.birth.substr(8, 2)));
    }
    if (!player.place.empty()) players.SetStr(row, kBirthplace, player.place);
    applied.push_back({player, row});
  }

  for (const auto& item : applied) {
    const fs::path shot = roster_path.parent_path() / item.player.headshot;
    if (!fs::is_regular_file(shot, ec)) continue;
    const Image image = ReadPng(shot);
    const std::string id = std::to_string(players.Num(item.row, kId));
    struct Spec {
      const char* folder;
      int tw, th, dw, dh;
    };
    const Spec specs[] = {{"fe/ion/artassets/playerheads/p0_4000", 512, 256, 467, 253},
                          {"fe/ion/artassets/playerheadssmall/p0_4000", 256, 128, 202, 128}};
    for (const auto& spec : specs) {
      archive.Put(std::string(spec.folder) + "/p" + id + ".big",
                  Portrait(archive, spec.folder, spec.tw, spec.th, spec.dw, spec.dh, image));
    }
    REXLOG_INFO("[roster]   portrait p{} from {}", id, item.player.headshot);
  }
  if (!skipped.empty()) {
    std::string list;
    for (size_t i = 0; i < skipped.size(); ++i) {
      if (i) list += ", ";
      list += skipped[i];
    }
    REXLOG_INFO("[roster] skipped (no headshot yet): {}", list);
  }

  db.Checksums();
  auto [out_big, out_bh] = archive.Save();
  const fs::path big_tmp = fs::path(big_path.string() + ".tmp");
  const fs::path bh_tmp = fs::path(bh_path.string() + ".tmp");
  const fs::path db_tmp = fs::path(db_path.string() + ".tmp");
  if (!WriteFile(big_tmp, out_big.data(), out_big.size()) || !WriteFile(bh_tmp, out_bh.data(), out_bh.size()) ||
      !WriteFile(db_tmp, db.bytes().data(), db.bytes().size())) {
    Fail("could not write the roster files");
  }
  fs::rename(db_tmp, db_path, ec);
  if (!ec) fs::rename(big_tmp, big_path, ec);
  if (!ec) fs::rename(bh_tmp, bh_path, ec);
  if (ec) Fail("could not replace the roster files");
  WriteFile(game_dir / kStateName, reinterpret_cast<const uint8_t*>(fingerprint.data()), fingerprint.size());
  REXLOG_INFO("[roster] applied {} players from {}", applied.size(), roster_path.string());
}

}  // namespace

void ApplyRoster(const fs::path& game_dir) {
  g_online_id = "stock";
  std::error_code ec;
  if (game_dir.empty() || !fs::is_regular_file(game_dir / "data0.big", ec)) return;
  const fs::path roster = FindRosterJson();
  if (roster.empty()) return;
  try {
    Apply(game_dir, roster);
  } catch (const std::exception& error) {
    g_online_id = "stock";
    REXLOG_ERROR("[roster] not applied: {}", error.what());
  }
  if (g_online_id != "stock") {
    REXLOG_INFO("[roster] online id {}", g_online_id);
  }
}

std::string RosterOnlineId() { return g_online_id; }

}  // namespace nhl3on3
