#pragma once

// strikers-quest: where the threads wait on each other, summed in nanoseconds and read (and reset)
// by the headset's stats line through aurora_xr_take_waits (frame.cpp).

#include <atomic>
#include <chrono>
#include <cstdint>

namespace aurora::xr_waits {
enum Kind : int {
  FifoDrain,   // game thread waiting for the FIFO processor (fifo::drain)
  QueueFull,   // a thread waiting for room in the render worker's queue
  Sync,        // waiting for the render worker to empty its queue (gfx::synchronize)
  FrameSlot,   // begin_frame waiting for a frame packet the render worker still holds
  Staging,     // begin_frame waiting for a staging buffer the GPU still reads
  Pipeline,    // waiting for a pipeline to compile
  FifoBusy,    // FIFO processor working
  RenderBusy,  // render worker working
  Count
};

inline std::atomic<uint64_t> g_ns[Count];

inline uint64_t now_ns() noexcept {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct Scope {
  Kind kind;
  uint64_t start;
  explicit Scope(Kind k) noexcept : kind(k), start(now_ns()) {}
  ~Scope() { g_ns[kind].fetch_add(now_ns() - start, std::memory_order_relaxed); }
};
} // namespace aurora::xr_waits
