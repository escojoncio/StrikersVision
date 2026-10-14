#pragma once

#include "types.hpp"

namespace aurora::gfx {
inline constexpr bool UseTextureBuffer = true;
// smstrikers-port: sized from this game's measured peaks rather than upstream's
// defaults (uniform 24, storage 8 MiB). Measured worst case is 1.92, 1.95, 0.52,
// 8.10 and 19.45 MiB in the order below; overflow of the first four is a bare
// abort(), so each carries 2-4x margin. Storage is grown, not shrunk.
// strikers-quest: the headset's 3D modes send the frame up to five times (game
// view + two eyes x two layers), and vertices, indices and uniforms are pushed
// again on every send, and so are the storage arrays (GXSetArray clears their
// per-draw cache). Tabletop · Stadium overflowed the 5 MiB vertex pool by 84
// bytes at kick-off (r4), then the 16 MiB storage pool (r6). xr_quest.cpp also
// drops eye layers that would not fit rather than let a pool abort.
// strikers-quest r38: decoded vertices (gx/vertex_decode.hpp) live in the vertex pool, and decoded
// draws push no arrays: vertex 16 -> 24 MiB, storage 40 -> 32 MiB (r38 peaks: vertex 82% of 16 MiB
// in Tabletop · Stadium with everything decoded, storage 26% of 40 MiB with decoding off).
inline constexpr uint64_t UniformBufferSize = 25165824;  // 24 MiB
inline constexpr uint64_t VertexBufferSize = 25165824;   // 24 MiB
inline constexpr uint64_t IndexBufferSize = 6291456;     // 6 MiB
inline constexpr uint64_t StorageBufferSize = 33554432;  // 32 MiB
inline constexpr uint64_t TextureUploadSize = 25165824;  // 24 MiB

namespace detail {
struct Resources {
  wgpu::Buffer vertexBuffer;
  wgpu::Buffer uniformBuffer;
  wgpu::Buffer indexBuffer;
  wgpu::Buffer storageBuffer;
  wgpu::BindGroupLayout staticBindGroupLayout;
  wgpu::BindGroup staticBindGroup;
  wgpu::BindGroupLayout uniformBindGroupLayout;
  wgpu::BindGroup uniformBindGroup;
  wgpu::Limits limits;
  AuroraStats stats{};
};

Resources& resources() noexcept;
} // namespace detail
} // namespace aurora::gfx
