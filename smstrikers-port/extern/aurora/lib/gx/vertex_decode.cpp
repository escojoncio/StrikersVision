#include "vertex_decode.hpp"

#include <dolphin/gx/GXEnum.h>

#include <cstring>

namespace aurora::gx::vertex_decode {
namespace {

bool is_color(int attr) noexcept { return attr == GX_VA_CLR0 || attr == GX_VA_CLR1; }
bool is_tex_mtx_idx(int attr) noexcept { return attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX; }
bool is_tex(int attr) noexcept { return attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7; }

// Bytes one colour element occupies (what fetch_color_attr reads), 0 = unknown type.
u32 color_size(u8 compType) noexcept {
  switch (compType) {
  case GX_RGB565:
  case GX_RGBA4:
    return 2;
  case GX_RGB8:
  case GX_RGBA6:
    return 3;
  case GX_RGBX8:
  case GX_RGBA8:
    return 4;
  default:
    return 0;
  }
}

u32 comp_size(u8 compType) noexcept {
  switch (compType) {
  case GX_U8:
  case GX_S8:
    return 1;
  case GX_U16:
  case GX_S16:
    return 2;
  case GX_F32:
    return 4;
  default:
    return 0;
  }
}

// load_u16 / load_u32 in the pulling shader: bytes assembled little-endian, byte-swapped unless le.
inline u32 read16(const u8* p, bool le) noexcept {
  return le ? (static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8)
            : (static_cast<u32>(p[0]) << 8 | static_cast<u32>(p[1]));
}

inline u32 read32(const u8* p, bool le) noexcept {
  return le ? (static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 |
               static_cast<u32>(p[3]) << 24)
            : (static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 | static_cast<u32>(p[2]) << 8 |
               static_cast<u32>(p[3]));
}

inline u32 float_bits(float f) noexcept {
  u32 w;
  std::memcpy(&w, &f, sizeof(w));
  return w;
}

struct Active {
  u8 attr;
  u8 attrType;  // GX_DIRECT / GX_INDEX8 / GX_INDEX16
  u8 offset;    // within the raw vertex
  u8 word;      // first output word
  u8 kind;      // 0 matrix index, 1 components, 2 colour
  u8 cnt;       // components read
  u8 compType;
  u8 frac;
  u8 stride;    // array stride
  u8 elemSize;  // bytes one element needs
  bool le;      // array data endianness (direct data is always big-endian)
  bool divide3; // PNMTXIDX: raw / 3
};

} // namespace

Layout layout_for(const ShaderConfig& config) noexcept {
  Layout l;
  if (config.lineMode != 0) {
    return {};
  }
  u32 next = 4;  // words 0..2 position, 3 PNMTXIDX
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto& m = config.attrs[i];
    if (m.attrType == GX_NONE) {
      continue;
    }
    if (m.attrType != GX_DIRECT && m.attrType != GX_INDEX8 && m.attrType != GX_INDEX16) {
      return {};
    }
    if (i == GX_VA_PNMTXIDX) {
      if (m.attrType != GX_DIRECT) {
        return {};
      }
      l.attrs[i] = {3, 1};
    } else if (is_tex_mtx_idx(i)) {
      if (m.attrType != GX_DIRECT) {
        return {};
      }
      l.attrs[i] = {static_cast<u8>(next), 1};
      next += 1;
    } else if (i == GX_VA_POS) {
      if (comp_size(m.compType) == 0 || (m.cnt != 2 && m.cnt != 3)) {
        return {};
      }
      l.attrs[i] = {0, 3};
    } else if (i == GX_VA_NRM) {
      // NBT normals (binormal/tangent for emboss texgens) keep the pulling path.
      if (comp_size(m.compType) == 0 || m.cnt != 3 || m.nbt3) {
        return {};
      }
      l.attrs[i] = {static_cast<u8>(next), 3};
      next += 3;
    } else if (is_color(i)) {
      if (color_size(m.compType) == 0) {
        return {};
      }
      l.attrs[i] = {static_cast<u8>(next), 1};
      next += 1;
    } else if (is_tex(i)) {
      if (comp_size(m.compType) == 0 || (m.cnt != 1 && m.cnt != 2)) {
        return {};
      }
      l.attrs[i] = {static_cast<u8>(next), 2};
      next += 2;
    } else {
      return {};
    }
  }
  if (l.attrs[GX_VA_POS].count == 0) {
    return {};
  }
  l.strideWords = (next + 3u) & ~3u;
  if (l.strideWords > 252u) {
    return {};
  }
  l.valid = true;
  return l;
}

namespace {

// The element an attribute of vertex vb reads: in the vertex itself (direct) or in its array
// (indexed; GX_INDEX16 indices are big-endian, as attr_address reads them). Null = past the array.
template <u8 AttrType>
inline const u8* element(const u8* vb, const Active& a, const ArraySource& array) noexcept {
  if constexpr (AttrType == GX_DIRECT) {
    return vb + a.offset;
  } else {
    const u32 idx = AttrType == GX_INDEX8 ? vb[a.offset] : (static_cast<u32>(vb[a.offset]) << 8 | vb[a.offset + 1]);
    const uint64_t off = static_cast<uint64_t>(idx) * a.stride;
    if (off + a.elemSize > array.size) {
      return nullptr;
    }
    return array.data + off;
  }
}

// One component, as fetch_*: f32(v) * 2^-frac equals f32(v) / 2^frac exactly (power of two).
template <u8 CompType>
inline u32 convert(const u8* p, float scale, bool le) noexcept {
  if constexpr (CompType == GX_U8) {
    return float_bits(static_cast<float>(p[0]) * scale);
  } else if constexpr (CompType == GX_S8) {
    return float_bits(static_cast<float>(static_cast<int8_t>(p[0])) * scale);
  } else if constexpr (CompType == GX_U16) {
    return float_bits(static_cast<float>(read16(p, le)) * scale);
  } else if constexpr (CompType == GX_S16) {
    return float_bits(static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(read16(p, le)))) * scale);
  } else {
    return read32(p, le);
  }
}

template <u8 AttrType, u8 CompType>
bool decode_components(const Active& a, const ArraySource& array, const u8* vtxData, u32 stride, u32 vtxCount,
                       u32 words, u32* out) noexcept {
  constexpr u32 size = CompType == GX_F32 ? 4 : (CompType == GX_U16 || CompType == GX_S16 ? 2 : 1);
  const float scale = 1.0f / static_cast<float>(1u << (a.frac & 31u));
  const bool le = a.le;
  const u32 cnt = a.cnt;
  for (u32 v = 0; v < vtxCount; ++v) {
    const u8* src = element<AttrType>(vtxData + static_cast<size_t>(v) * stride, a, array);
    if (src == nullptr) {
      return false;
    }
    u32* o = out + static_cast<size_t>(v) * words + a.word;
    for (u32 c = 0; c < cnt; ++c) {
      o[c] = convert<CompType>(src + c * size, scale, le);
    }
  }
  return true;
}

template <u8 AttrType>
bool decode_raw(const Active& a, const ArraySource& array, const u8* vtxData, u32 stride, u32 vtxCount, u32 words,
                u32* out) noexcept {
  for (u32 v = 0; v < vtxCount; ++v) {
    const u8* src = element<AttrType>(vtxData + static_cast<size_t>(v) * stride, a, array);
    if (src == nullptr) {
      return false;
    }
    u32* o = out + static_cast<size_t>(v) * words + a.word;
    if (a.kind == 0) {
      *o = a.divide3 ? src[0] / 3u : src[0];
    } else {
      // Colour bytes in memory order, as load_u32_raw assembles them; the shader unpacks.
      u32 w = 0;
      for (u32 b = 0; b < a.elemSize; ++b) {
        w |= static_cast<u32>(src[b]) << (8u * b);
      }
      *o = w;
    }
  }
  return true;
}

template <u8 AttrType>
bool decode_attr(const Active& a, const ArraySource& array, const u8* vtxData, u32 stride, u32 vtxCount, u32 words,
                 u32* out) noexcept {
  if (a.kind != 1) {
    return decode_raw<AttrType>(a, array, vtxData, stride, vtxCount, words, out);
  }
  switch (a.compType) {
  case GX_U8:
    return decode_components<AttrType, GX_U8>(a, array, vtxData, stride, vtxCount, words, out);
  case GX_S8:
    return decode_components<AttrType, GX_S8>(a, array, vtxData, stride, vtxCount, words, out);
  case GX_U16:
    return decode_components<AttrType, GX_U16>(a, array, vtxData, stride, vtxCount, words, out);
  case GX_S16:
    return decode_components<AttrType, GX_S16>(a, array, vtxData, stride, vtxCount, words, out);
  default:
    return decode_components<AttrType, GX_F32>(a, array, vtxData, stride, vtxCount, words, out);
  }
}

} // namespace

bool decode(const ShaderConfig& config, const Layout& layout, const u8* vtxData, u32 vtxCount,
            const std::array<ArraySource, MaxVtxAttr>& arrays, u32* out) noexcept {
  std::array<Active, MaxVtxAttr> active{};
  u32 activeCount = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto& slot = layout.attrs[i];
    if (slot.count == 0) {
      continue;
    }
    const auto& m = config.attrs[i];
    Active a{};
    a.attr = static_cast<u8>(i);
    a.attrType = m.attrType;
    a.offset = m.offset;
    a.word = slot.word;
    a.compType = m.compType;
    a.frac = m.frac;
    a.stride = m.stride;
    a.le = m.attrType == GX_DIRECT ? false : m.le;
    if (i == GX_VA_PNMTXIDX || is_tex_mtx_idx(i)) {
      a.kind = 0;
      a.cnt = 1;
      a.elemSize = 1;
      a.divide3 = i == GX_VA_PNMTXIDX;
    } else if (is_color(i)) {
      a.kind = 2;
      a.cnt = 1;
      a.elemSize = static_cast<u8>(color_size(m.compType));
    } else {
      a.kind = 1;
      a.cnt = m.cnt;
      a.elemSize = static_cast<u8>(comp_size(m.compType) * m.cnt);
    }
    if (a.attrType != GX_DIRECT && arrays[i].data == nullptr) {
      return false;
    }
    active[activeCount++] = a;
  }

  // One pass per attribute (the conversion picked once, not per vertex), into a zeroed block: the
  // padding, an absent PNMTXIDX and the missing z / t of 2-component positions / 1-component UVs
  // stay 0 (vec3f(xy, 0.0) / vec2f(s, 0.0) in the pulling shader).
  const u32 stride = config.vtxStride;
  const u32 words = layout.strideWords;
  std::memset(out, 0, static_cast<size_t>(vtxCount) * words * sizeof(u32));
  for (u32 k = 0; k < activeCount; ++k) {
    const Active& a = active[k];
    const auto& array = arrays[a.attr];
    bool ok;
    switch (a.attrType) {
    case GX_DIRECT:
      ok = decode_attr<GX_DIRECT>(a, array, vtxData, stride, vtxCount, words, out);
      break;
    case GX_INDEX8:
      ok = decode_attr<GX_INDEX8>(a, array, vtxData, stride, vtxCount, words, out);
      break;
    default:
      ok = decode_attr<GX_INDEX16>(a, array, vtxData, stride, vtxCount, words, out);
      break;
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

} // namespace aurora::gx::vertex_decode
