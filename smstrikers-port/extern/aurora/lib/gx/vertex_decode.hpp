#pragma once

// strikers-quest: decode a draw's GX vertices once on the CPU into plain 32-bit words, so the
// vertex shader reads one vec4 per slot instead of pulling big-endian bytes (index fetch, byte
// assembly, bswap, sign extension, bounds checks) for every vertex, in every eye, binning pass and
// tile. Measured on Quest 3 (r37): the player draws ran 241 instructions per vertex in the binning
// pass alone at 82% fetch stall and 7.6% wave occupancy.
//
// The decoded values are bit-identical to what the pulling shader computes: fixed-point inputs are
// f32(v) / 2^frac (exact on CPU and GPU), F32 inputs are copied bits, and colours stay raw bytes
// that the shader unpacks with the same WGSL expressions as before (shader.cpp clr_w_*).
//
// Layout per vertex, in u32 words (stride rounded up to whole vec4s):
//   0..2  POS (x, y, z; z = 0 for 2-component positions)   3  PNMTXIDX (already / 3), 0 if absent
//   then, in GX attribute order, for each present attribute:
//   NRM 3 words, CLR0/CLR1 1 raw word each, TEXn 2 words (t = 0 for 1-component), TEXnMTXIDX 1 word.

#include "gx.hpp"

#include <array>
#include <cstdint>

namespace aurora::gx::vertex_decode {

struct AttrSlot {
  u8 word = 0;   // first word within the vertex
  u8 count = 0;  // words used, 0 = attribute absent
};

struct Layout {
  std::array<AttrSlot, MaxVtxAttr> attrs{};
  u32 strideWords = 0;  // multiple of 4
  bool valid = false;   // false: this config cannot be decoded (lines/points, NBT normals, odd formats)
};

// Same answer on the FIFO thread (decode) and the pipeline threads (shader generation): pure.
Layout layout_for(const ShaderConfig& config) noexcept;

struct ArraySource {
  const u8* data = nullptr;
  u32 size = 0;
};

// Decodes vtxCount vertices of raw GX vertex data (vtxStride bytes each, as described by config)
// into out (vtxCount * layout.strideWords words). Indexed attributes read arrays[attr]. Returns
// false (out partly written) when an index points past its array: such draws keep the pulling path,
// which reads whatever follows in the storage pool, and a CPU decode cannot reproduce that.
bool decode(const ShaderConfig& config, const Layout& layout, const u8* vtxData, u32 vtxCount,
            const std::array<ArraySource, MaxVtxAttr>& arrays, u32* out) noexcept;

} // namespace aurora::gx::vertex_decode
