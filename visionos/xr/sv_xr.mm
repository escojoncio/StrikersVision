// StrikersVision: include/port/xr_quest.h on Apple Vision Pro. The Quest layer
// (src/platform/xr_quest.cpp, OpenXR + Vulkan) ported to Compositor Services + ARKit + Metal; the
// game-facing behaviour is the same (60 Hz game step on a faster display, the four view modes, the
// Touch layout on the PlayStation VR2 Sense controllers, the Tabletop field you grab, the per-eye
// re-sends of glPlat.cpp, the pool guard, the online lockstep, the stats lines).
//
// Frame (main.cpp's headset loop, all on the game thread):
//   PortXrBeginFrame   live settings, view-mode requests, the network room, controllers -> PAD, the
//                      head predicted for the next display frame (the eye passes use it), and whether
//                      a 60 Hz game frame is due. No renderer (space closed), paused or invalidated:
//                      it sleeps 10 ms and says no, so the game simply waits.
//   ...game frame: glPlat.cpp re-sends the 3D views once per eye (PortXrStereo*) into IOSurfaces
//                      imported into Dawn (SharedTextureMemory) ...
//   PortXrEndFrame     after aurora::gfx::synchronize(): the flat game picture copied on the GPU into
//                      an IOSurface (or read back to the CPU when Dawn cannot share), the eye images
//                      handed back from Dawn with their MTLSharedEvent fence; then ONE Compositor
//                      Services frame (sv_xrhost_frame_begin waits for its optimal input time, which
//                      paces the loop at the display rate) composited with Metal: eye images, the
//                      screen, the celebrations (svfx), presented with the device anchor the pixels
//                      were drawn for, so the system reprojects them.
//
// GPU ordering: the composite's command buffer waits on Dawn's fence (encodeWaitForEvent) before it
// samples an image Dawn wrote, and signals an event of its own when done; Dawn's next BeginAccess on
// that image waits for that signal (an imported SharedFence). Nothing waits on the CPU.
//
// Built with ARC (-fobjc-arc).

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>

#include "../include/sv_bridge.h"
#include "sv_shaders.h"
#include "sv_xr.h"

#include "../../smstrikers-port/include/port/xr_net.h"
#include "../../smstrikers-port/include/port/xr_quest.h"
#include "../../smstrikers-port/src/platform/xr_net.hpp"

#include <aurora/aurora.h>
#include <aurora/gfx.hpp>
#include "../../smstrikers-port/extern/aurora/lib/webgpu/gpu.hpp"

#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

// QuartzCore: the clock ARKit and Compositor Services timestamps are in.
extern "C" double CACurrentMediaTime(void);
// src/platform/audio_out.cpp (include/port/audio.h), for the underrun count in the stats line.
extern "C" void PortAudioStats(unsigned long* outBuffers, unsigned long* outUnderruns, int* outEverNonSilent);
// Aurora (extern/aurora): pool accounting, thread waits, decode-once vertices, coverage, anisotropy.
extern "C" void aurora_gfx_pool_stats(uint32_t* peakBytes, uint32_t* reservedBytes, size_t count);
extern "C" void aurora_gfx_pool_usage(uint32_t* usedBytes, size_t count);
extern "C" void aurora_xr_take_waits(uint64_t* outNs, int count);
extern "C" void aurora_xr_set_vertex_decode(int mode);
extern "C" void aurora_xr_take_decode_stats(uint32_t out[6]);
extern "C" unsigned long long aurora_xr_decoded_bytes(void);
extern "C" unsigned long long aurora_xr_decoded_bytes_last_frame(void);
extern "C" void aurora_gx_set_xr_coverage(bool enabled);
extern "C" void aurora_xr_set_texture_anisotropy(unsigned int n);
// src/platform/xr_layers.cpp (C++ linkage, include/port/xr_layers.h).
unsigned long PortXrLayersTakeCulled();
void PortXrSetStadiumEconomy(float minPx, float eyeHalfWidthPx, int skipTallField, int bothLayers);

namespace {

constexpr const char* kBuildTag = "SV_XR build=sv1";

// ---------------------------------------------------------------------------------------------
// Small maths: column-major 4x4 (Metal / simd), and the game's row-vector M4 (xr_quest.cpp).
// ---------------------------------------------------------------------------------------------

struct V3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Mat {
  float m[16];  // column-major: m[c * 4 + r]
};

Mat mat_identity() {
  Mat r{};
  r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
  return r;
}

Mat mat_mul(const Mat& a, const Mat& b) {
  Mat r{};
  for (int c = 0; c < 4; ++c)
    for (int rr = 0; rr < 4; ++rr) {
      float v = 0.0f;
      for (int k = 0; k < 4; ++k) v += a.m[k * 4 + rr] * b.m[c * 4 + k];
      r.m[c * 4 + rr] = v;
    }
  return r;
}

// General inverse (cofactors); the projections are not rigid.
Mat mat_inverse(const Mat& in) {
  const float* m = in.m;
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
           m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
           m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
           m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
            m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
           m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
           m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
           m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
            m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
           m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
           m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
            m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
            m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
           m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
           m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
            m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
            m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  Mat r = mat_identity();
  if (std::fabs(det) < 1e-20f) return r;
  det = 1.0f / det;
  for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * det;
  return r;
}

// T(position) * Ry(yaw): a pose on the room's floor plane, column-major.
Mat mat_pose_yaw(const V3& p, float yaw) {
  Mat r = mat_identity();
  const float c = std::cos(yaw), s = std::sin(yaw);
  r.m[0] = c;
  r.m[2] = -s;
  r.m[8] = s;
  r.m[10] = c;
  r.m[12] = p.x;
  r.m[13] = p.y;
  r.m[14] = p.z;
  return r;
}

V3 mat_translation(const Mat& a) { return V3{a.m[12], a.m[13], a.m[14]}; }

// Row-vector 4x4 in the game's nlMatrix4 convention (xr_quest.cpp's M4): v' = v * M. Its memory is
// the column-major array of the column-vector matrix it equals (M = C^T), so a Mat converts by copy.
struct M4 {
  float m[4][4];
};

M4 m4_identity() {
  M4 r{};
  for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
  return r;
}

M4 m4_mul(const M4& a, const M4& b) {
  M4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
  return r;
}

M4 m4_from_mat(const Mat& c) {
  M4 r;
  std::memcpy(r.m, c.m, sizeof(r.m));
  return r;
}

// Game world -> room (LOCAL). The game is Z-up with the pitch in the XY plane; the room is Y-up
// looking down -Z: (x, y, z) -> (x, z, -y), then scale, turn by `yaw` about +Y, move to `origin`.
// The same mapping as xr_quest.cpp and sv_fx.mm's game_to_room.
M4 m4_game_to_room(float scale, float yaw, const V3& origin) {
  const float gr[3][3] = {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
  const float c = std::cos(yaw), s = std::sin(yaw);
  const float yr[3][3] = {{c, 0, -s}, {0, 1, 0}, {s, 0, c}};
  M4 r = m4_identity();
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      float v = 0;
      for (int k = 0; k < 3; ++k) v += gr[i][k] * yr[k][j];
      r.m[i][j] = scale * v;
    }
  r.m[3][0] = origin.x;
  r.m[3][1] = origin.y;
  r.m[3][2] = origin.z;
  return r;
}

// The eye's frustum as tangents (left, right, down, up; left and down negative) from the drawable's
// projection (right-handed, looking down -Z, w = -z): on the left edge at z = -1, x = tanL gives
// NDC -1, so -1 = P00 * tanL - P20.
struct Tangents {
  float l = -1.0f, r = 1.0f, d = -1.0f, u = 1.0f;
};

Tangents tangents_from_projection(const Mat& p) {
  Tangents t;
  const float p00 = p.m[0], p11 = p.m[5], p20 = p.m[8], p21 = p.m[9];
  if (std::fabs(p00) < 1e-6f || std::fabs(p11) < 1e-6f) return t;
  t.l = (p20 - 1.0f) / p00;
  t.r = (p20 + 1.0f) / p00;
  t.d = (p21 - 1.0f) / p11;
  t.u = (p21 + 1.0f) / p11;
  return t;
}

// GXSetProjection's perspective layout (C_MTXPerspective rows, clip z in [-w, 0]), as xr_quest.cpp's
// gx_projection, from tangents. Near/far are room metres.
void gx_projection(const Tangents& t, float n, float f, float out[16]) {
  std::memset(out, 0, sizeof(float) * 16);
  out[0 * 4 + 0] = 2.0f / (t.r - t.l);
  out[0 * 4 + 2] = (t.r + t.l) / (t.r - t.l);
  out[1 * 4 + 1] = 2.0f / (t.u - t.d);
  out[1 * 4 + 2] = (t.u + t.d) / (t.u - t.d);
  out[2 * 4 + 2] = -n / (f - n);
  out[2 * 4 + 3] = -(f * n) / (f - n);
  out[3 * 4 + 2] = -1.0f;
}

uint64_t now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

// The three Jumbotron sizes the right stick click cycles: width and distance in metres.
struct ScreenPreset {
  float width;
  float distance;
};
constexpr ScreenPreset kScreenPresets[] = {{2.4f, 2.5f}, {3.6f, 3.0f}, {6.0f, 4.0f}};
constexpr int kScreenPresetCount = 3;

constexpr int64_t kGameTickNs = 16666667;  // one game frame every 1/60 s
constexpr uint64_t kDoubleTapNs = 350000000ull;

// Which 3D layers the current mode draws. The flags match glPlat.cpp's XR_LAYER_*.
enum LayerKind { kLayerStadium = 0, kLayerTable = 1 };
constexpr int kFlagSkipActors = 1;
constexpr int kFlagSkipSky = 2;
constexpr int kFlagTable = 4;
constexpr int kPools = 4;
constexpr double kPoolMargin = 1.25;
// Where you stand in the life-size stadium (game y), as xr_quest.cpp.
constexpr float kStandY = -10.5f;

// One image Dawn writes and the composite reads: an IOSurface, the same memory as a Dawn texture
// (SharedTextureMemory) and as a Metal texture, with the fences of the hand-off.
struct SharedImage {
  IOSurfaceRef surface = nullptr;
  uint32_t width = 0, height = 0;
  wgpu::SharedTextureMemory memory;
  wgpu::Texture texture;
  wgpu::TextureView view;
  bool accessOpen = false;
  // Dawn's end-of-access fence: the composite waits for `doneValue` on `doneEvent` before reading.
  id<MTLSharedEvent> doneEvent = nil;
  uint64_t doneValue = 0;
  bool valid = false;  // holds a finished picture
  bool refused = false;  // Dawn refused an access (a validation error): the image is not usable
  // The last composite that read it signals compositeEvent with this value (Dawn's next write waits).
  uint64_t readValue = 0;
  // Metal side (the composite's device), made on first use.
  id<MTLTexture> mtl = nil;     // RGBA8Unorm over the IOSurface
  id<MTLTexture> mtlSrgb = nil; // sRGB view: the game's display-encoded bytes arrive linear
  bool isDepth = false;         // the eye pass's depth as the float's bytes (Aurora's packed snapshot), read raw
};

// The pose an eye image was drawn for (the drawable reprojects from it).
struct RenderPose {
  bool valid = false;
  Mat originFromDevice = mat_identity();
  int32_t anchorId = -1;
  Mat deviceFromView[2] = {mat_identity(), mat_identity()};
  Mat projection[2] = {mat_identity(), mat_identity()};
};

// Eye pairs: one on show (completed), up to two handed back and waiting for the GPU, one being written
// by the game thread (display_encode only ever shows a completed pair; pick_eye_slot never writes the
// one on show).
constexpr int kEyeSlots = 4;
// The eye passes' near and far planes (gx_projection), in room metres; the composite inverts them.
constexpr float kEyeNear = 0.05f, kEyeFar = 1500.0f;
constexpr int kScreenSlots = 4;  // same roles as the eye slots
// STRIKERS_XR_DEPTH=1: the system gets the real depth only nearer than this many metres from the eye.
constexpr float kNearDepthCutoff = 1.2f;

constexpr float kTableScaleBase = 0.032f;  // room metres per game unit: the 41 m pitch is ~1.3 m
constexpr float kTableScaleMin = 0.008f, kTableScaleMax = 0.25f;

struct Table {
  V3 position{0.0f, 1.05f, -0.95f};  // about waist height in front of where you stood (set at start)
  float yaw = 0.0f;
  float scale = kTableScaleBase;
  int grabHands = 0;     // bit 0 left, bit 1 right
  bool rightGrab = false;
  V3 grabOffset;
  float twoDist = 1.0f, twoScale = kTableScaleBase, twoYaw = 0.0f, prevAngle = 0.0f, turn = 0.0f;
  V3 twoPos, twoMid;
};

struct Rumble {
  float amplitude = 0.5f;
  bool startPending = false;
  float seconds = 0.0f;
  bool stopPending = false;
};

struct Settings {
  int screenPreset = 1;
  float screenCurve = 0.0f;
  float eyeScale = 1.0f;
  int msaaTable = 4, msaaStadium = 4;
  bool hudOnly = true;
  float charScale = 1.5f;
  float stadiumScale = 1.0f;
  float tableScale = 1.0f;  // times kTableScaleBase, applied to g.table.scale when it changes
  float lodMinPx = 0.0f;
  bool skipTallField = false;
  int haptics = 100;
  float deadzone = 0.12f;
  bool psButtonStart = false;
  int depth = 0;  // STRIKERS_XR_DEPTH: 0 off, 1 near only, 2 full real depth (for the system)
  int statsSeconds = 5;
};

struct Metal {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  MTLPixelFormat colorFormat = MTLPixelFormatInvalid;
  MTLPixelFormat depthFormat = MTLPixelFormatInvalid;
  id<MTLRenderPipelineState> eyePipeline = nil;
  id<MTLRenderPipelineState> screenPipeline = nil;
  id<MTLRenderPipelineState> depthFixupPipeline = nil;  // depth-only: near-only depth for the system
  id<MTLDepthStencilState> depthAlways = nil;  // eye images (their estimated depth)
  id<MTLDepthStencilState> depthTest = nil;    // the screen
  id<MTLSamplerState> sampler = nil;
  id<MTLTexture> scratchDepth = nil;  // when a drawable hands out no depth texture
  bool failed = false;
  // CPU fallback of the flat picture.
  id<MTLTexture> cpuScreen[kScreenSlots] = {};
  id<MTLTexture> cpuScreenSrgb[kScreenSlots] = {};
  uint64_t cpuScreenRead[kScreenSlots] = {};
  uint32_t cpuWidth = 0, cpuHeight = 0;
  int cpuShown = -1;
  int cpuNext = 0;
};

struct State {
  bool active = false;
  bool quit = false;

  // The renderer (cp_layer_renderer_t), retained here while used.
  void* renderer = nullptr;
  uint32_t rendererGeneration = 0xFFFFFFFFu;
  bool rendererRunning = false;
  int lastRendererState = -1;
  bool waitingLogged = false;
  uint64_t pausedSinceNs = 0, pausedWarnedNs = 0;

  // Modes.
  bool tabletop = false;
  bool mrMode = true;
  int reportedViewMode = -1;
  int reportedLimbs = -1;
  Settings set;

  // Head and eyes. The room's origin is on the floor (ARKit); `eyeHeight` = the head's height at the
  // first pose, which stands in for Quest's LOCAL origin (the head at the start).
  bool haveHead = false;
  float eyeHeight = 1.6f;
  float floorY = 0.0f;
  Mat lastHead = mat_identity();
  int32_t lastHeadAnchor = -1;
  Mat drawableDeviceFromView[2] = {mat_identity(), mat_identity()};
  Mat drawableProjection[2] = {mat_identity(), mat_identity()};
  float viewportW = 0.0f, viewportH = 0.0f;
  bool drawableKnown = false;
  double lastPresentation = 0.0;
  double displayPeriod = 1.0 / 90.0;
  RenderPose predicted;  // this game frame's eye poses (PortXrBeginFrame)
  bool viewsValid = false;

  // Input.
  SVPad localPad{};
  bool port1Active = false;
  bool prevStickClickL = false, prevStickClickR = false;
  uint64_t pendingTapL = 0, pendingTapR = 0;
  bool prevGripR = false;
  uint32_t prevItemButtons = 0;
  bool prevPs = false;
  Rumble rumble;
  uint64_t rumblePulses = 0;
  bool inputLogged = false;
  V3 hand[2];
  bool handValid[2] = {false, false};

  Table table;
  bool tableSet = false;
  bool bringTable = false;
  uint64_t lastTablePoseSentNs = 0;
  bool tablePoseDirty = false;
  bool remoteNonSpatialLogged = false;

  // 3D modes.
  bool stereoFailed = false;
  bool stereoReady = false;
  uint32_t eyeWidth = 0, eyePxHeight = 0;  // the eye images, pixels
  SharedImage eyes[kEyeSlots][2];
  // The eye passes' depth (one per eye image; RGBA8 holding each float's bytes, since this Dawn
  // imports no float IOSurface), handed to the composite so the system gets the real depth of every
  // pixel (nearby objects then occlude correctly). Off when Dawn refuses it: the composite falls
  // back to the table-plane estimate.
  SharedImage eyeDepths[kEyeSlots][2];
  bool depthExport = false;
  int eyeSlot = 0;  // the slot this game frame renders into (pick_eye_slot)
  int shownSlot = -1;  // the newest completed pair the composite has switched to
  RenderPose shownPose;
  // Per slot: handed back by the game thread (a finished or still-running pair), in which order, and
  // the pose it was drawn for. A slot is written again only after pick_eye_slot cleared its flag.
  bool eyeHanded[kEyeSlots] = {};
  uint64_t eyeSeq[kEyeSlots] = {};
  RenderPose eyePose[kEyeSlots];
  uint64_t eyeSeqCounter = 0, shownSeq = 0;
  bool stereoThisFrame = false;
  bool eyePassOpen[2] = {false, false};
  int stereoLayers = 0;
  int frameLayerKind[2] = {0, 0};
  bool hasProjection = false;
  uint64_t eyeSendStartNs = 0;
  // Pool guard (xr_quest.cpp).
  uint32_t poolReserved[kPools] = {};
  float perSend[2][kPools] = {};
  unsigned long long decodedAtMeasure = 0;
  float decodedPerFrame = 0.0f;
  bool measuring = false;
  int measuringKind = 0;
  uint32_t measureStart[kPools] = {};
  uint64_t layersDroppedSinceStats = 0;

  // The flat picture.
  bool gpuScreen = false;  // GPU copy into an IOSurface; false = CPU readback (aurora frame callback)
  SharedImage screens[kScreenSlots];
  int screenNext = 0;   // the slot the next copy goes into (pick_screen_slot)
  int screenShown = -1; // the newest completed picture the composite has switched to
  bool screenHanded[kScreenSlots] = {};
  uint64_t screenSeq[kScreenSlots] = {};
  uint64_t screenSeqCounter = 0, screenShownSeq = 0;
  std::atomic<bool> gpuScreenError{false};

  // GPU hand-off.
  id<MTLSharedEvent> compositeEvent = nil;
  uint64_t compositeValue = 0;
  wgpu::SharedFence compositeFence;  // compositeEvent, imported into Dawn
  std::atomic<uint64_t> cmdCommitted{0};
  std::atomic<uint64_t> cmdCompleted{0};
  uint64_t fenceSkips = 0;

  Metal mtl;

  // Effects.
  bool fxReady = false;
  bool celebBuzz = false;
  int buzzWave = 0;
  bool dropLogged = false;
  // The flat picture placed by the player (left grip on it, or the match window's buttons): kept
  // where they put it in every view until "back to its place". lastScreen* is the last placement drawn.
  bool barePinchPrev[2] = {false, false};  // bare hands: pinching last frame
  bool barePinchOk[2] = {false, false};    // this pinch started on the table or the screen
  bool barePinchTable[2] = {false, false}; // ... on the table
  bool screenUser = false;
  V3 screenUserPos;
  float screenUserYaw = 0.0f;
  bool screenGrab = false;
  int screenGrabHand = 0;
  V3 screenGrabOffset;
  bool lastScreenShown = false;
  bool lastScreenMovable = false;
  V3 lastScreenPos;
  float lastScreenYaw = 0.0f, lastScreenWidth = 0.0f;
  std::string screenCmdSeen;
  bool gameplayLive = false;
  uint64_t lastFxNs = 0;
  struct {
    bool shown = false, logged = false;
    float game[3] = {0, 0, 0};
    float halfU = 0.1f, halfV = 0.2f;
  } sts;

  // Pacing and stats.
  bool frameBegun = false;
  int64_t tickAccumNs = 0;
  uint64_t lastTickNs = 0;
  double loopIntervalEma = 0.0;
  bool onlineHalfRate = false;
  uint32_t onlineParity = 0;
  uint64_t lastStatsNs = 0;
  uint64_t gameFramesSinceStats = 0, displayFramesSinceStats = 0, noFrameSinceStats = 0;
  uint64_t loopNs[5] = {};
  uint64_t beginWaitNs = 0, endSyncNs = 0, compositeNs = 0, eyeSendNs = 0, fxNs = 0;
  unsigned long lastUnderruns = 0;
  uint64_t drawablesSinceStats = 0;
  // Presentation diagnostics (stats line "present:").
  uint64_t eyeChecksSinceStats = 0, eyeNotReadySinceStats = 0;
  uint64_t framesPerImage[3] = {};  // display frames one game image stayed up: 1, 2, 3+
  uint64_t framesOnShown = 0;
  uint64_t lockWaitNs = 0, lockWaitMaxNs = 0;  // display thread: time to take g_lock after frame_begin
};
State g;

// Two threads touch `g`: the game thread (every Port* entry point, on the game's own loop) and the
// display thread (display_thread_main: one Compositor Services frame at 90 Hz, composited from the
// newest eye images). Everything that reads or writes `g` runs under this lock; the long waits
// (the compositor's input time, Aurora's synchronize) never do.
std::recursive_mutex g_lock;
std::thread g_displayThread;
std::atomic<bool> g_displayQuit{false};

// GPU timing for the log (Metal completion handlers and shared-event listeners, any thread): the eye
// images from Dawn's hand-off to its fence firing, and the composite's own GPU time, in microseconds.
std::atomic<uint64_t> g_eyeGpuUs{0}, g_eyeGpuMaxUs{0}, g_eyeGpuCount{0};
std::atomic<uint64_t> g_compGpuUs{0}, g_compGpuMaxUs{0}, g_compGpuCount{0};
MTLSharedEventListener* g_eventListener = nil;

void note_max(std::atomic<uint64_t>& m, uint64_t v) {
  uint64_t cur = m.load();
  while (v > cur && !m.compare_exchange_weak(cur, v)) {
  }
}

// Late presentations (composite command buffer finished after the drawable's presentation time) and
// the longest g_lock hold of the game thread, for the stats line.
std::atomic<uint64_t> g_presentCount{0}, g_lateCount{0}, g_lateMaxUs{0};
std::atomic<uint64_t> g_gameLockHoldMaxUs{0};
thread_local bool t_isDisplayThread = false;
thread_local int t_lockDepth = 0;

// g_lock with the game thread's outermost hold time measured (the display thread is not).
class TimedLock {
 public:
  explicit TimedLock(std::recursive_mutex& m) : m_(m) { lock(); }
  ~TimedLock() {
    if (owns_) unlock();
  }
  TimedLock(const TimedLock&) = delete;
  TimedLock& operator=(const TimedLock&) = delete;
  void lock() {
    m_.lock();
    owns_ = true;
    outer_ = !t_isDisplayThread && t_lockDepth++ == 0;
    if (outer_) t0_ = now_ns();
  }
  void unlock() {
    owns_ = false;
    if (!t_isDisplayThread) {
      --t_lockDepth;
      if (outer_) note_max(g_gameLockHoldMaxUs, (now_ns() - t0_) / 1000);
    }
    m_.unlock();
  }

 private:
  std::recursive_mutex& m_;
  bool owns_ = false, outer_ = false;
  uint64_t t0_ = 0;
};
using Lock = TimedLock;

// The newest flat frame read back by Aurora on its render worker (CPU fallback only).
std::mutex g_frameMutex;
std::vector<uint8_t> g_frame;  // tightly packed RGBA8
uint32_t g_frameWidth = 0, g_frameHeight = 0;
uint64_t g_frameSerial = 0, g_frameUploaded = 0;

void on_aurora_frame(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t bytesPerRow, bool bgra,
                     void*) {
  std::lock_guard<std::mutex> lock(g_frameMutex);
  const size_t tight = static_cast<size_t>(width) * 4;
  g_frame.resize(tight * height);
  uint8_t* dst = g_frame.data();
  for (uint32_t row = 0; row < height; ++row) {
    const uint8_t* s = pixels + static_cast<size_t>(row) * bytesPerRow;
    uint8_t* d = dst + static_cast<size_t>(row) * tight;
    if (!bgra) {
      std::memcpy(d, s, tight);
    } else {
      for (uint32_t col = 0; col < width; ++col) {
        d[col * 4 + 0] = s[col * 4 + 2];
        d[col * 4 + 1] = s[col * 4 + 1];
        d[col * 4 + 2] = s[col * 4 + 0];
        d[col * 4 + 3] = s[col * 4 + 3];
      }
    }
  }
  g_frameWidth = width;
  g_frameHeight = height;
  ++g_frameSerial;
}

// ---------------------------------------------------------------------------------------------
// Modes and settings
// ---------------------------------------------------------------------------------------------

const char* mode_name() {
  if (g.tabletop) return g.mrMode ? "Tabletop · Living Room" : "Tabletop · Stadium";
  return g.mrMode ? "Jumbotron · Living Room" : "Jumbotron · Stadium";
}

int view_mode() {
  if (g.tabletop) return g.mrMode ? SV_VIEW_TABLETOP_ROOM : SV_VIEW_TABLETOP_STADIUM;
  return g.mrMode ? SV_VIEW_JUMBOTRON_ROOM : SV_VIEW_JUMBOTRON_STADIUM;
}

// Tells the app (HUD, hands) whenever the mode differs from what it last heard.
void report_mode() {
  const int vm = view_mode();
  if (vm != g.reportedViewMode) {
    g.reportedViewMode = vm;
    sv_game_view_mode_changed(vm);
  }
  // Standing in the stadium the system's hands would float over the pitch; in the room they stay.
  const int limbs = g.mrMode ? 1 : 0;
  if (limbs != g.reportedLimbs) {
    g.reportedLimbs = limbs;
    sv_game_set_upper_limbs_visible(limbs);
  }
}

void set_mode(bool tabletop, bool mr) {
  const bool changed = tabletop != g.tabletop || mr != g.mrMode;
  g.tabletop = tabletop;
  g.mrMode = mr;
  if (changed) SVLOG("mode: %s", mode_name());
  report_mode();
}

void apply_view_mode(int vm) {
  switch (vm) {
  case SV_VIEW_JUMBOTRON_ROOM: set_mode(false, true); break;
  case SV_VIEW_JUMBOTRON_STADIUM: set_mode(false, false); break;
  case SV_VIEW_TABLETOP_ROOM: set_mode(true, true); break;
  case SV_VIEW_TABLETOP_STADIUM: set_mode(true, false); break;
  default: break;
  }
}

int msaa_value(const char* name, int fallback) {
  const int v = sv_env_int(name, fallback, 1, 4);
  return v > 1 ? 4 : 1;
}

// Every setting from the environment (SETTINGS.md); called at init and after any live change.
void read_settings() {
  Settings& s = g.set;
  s.screenPreset = sv_env_int("STRIKERS_XR_SCREEN_SIZE", 1, 0, kScreenPresetCount - 1);
  s.screenCurve = sv_env_float("STRIKERS_XR_SCREEN_CURVE", 0.0f, 0.0f, 1.0f);
  s.eyeScale = sv_env_float("STRIKERS_XR_EYE_SCALE", 1.0f, 0.3f, 1.5f);
  const int msaa = msaa_value("STRIKERS_XR_MSAA", 4);
  s.msaaTable = msaa_value("STRIKERS_XR_MSAA_TABLE", msaa);
  s.msaaStadium = msaa_value("STRIKERS_XR_MSAA_STADIUM", msaa);
  s.hudOnly = sv_env_int("STRIKERS_XR_HUD_ONLY", 1, 0, 1) != 0;
  s.charScale = sv_env_float("STRIKERS_XR_CHARSCALE", 1.5f, 1.0f, 3.0f);
  s.stadiumScale = sv_env_float("STRIKERS_XR_STADIUM_SCALE", 1.0f, 0.1f, 2.0f);
  s.tableScale = sv_env_float("STRIKERS_XR_TABLE_SCALE", 1.0f, 0.25f, 4.0f);
  s.lodMinPx = sv_env_float("STRIKERS_XR_LOD", 0.0f, 0.0f, 32.0f);
  s.skipTallField = sv_env_int("STRIKERS_XR_SKIP_TALL_FIELD", 0, 0, 1) != 0;
  s.haptics = sv_env_int("STRIKERS_XR_HAPTICS", 100, 0, 100);
  s.deadzone = sv_env_float("STRIKERS_XR_DEADZONE", 0.12f, 0.0f, 0.4f);
  s.psButtonStart = sv_env_int("STRIKERS_XR_PS_BUTTON", 0, 0, 1) != 0;
  s.depth = sv_env_int("STRIKERS_XR_DEPTH", 0, 0, 2);
  s.statsSeconds = sv_env_int("STRIKERS_XR_STATS_SECONDS", 5, 1, 60);
}

// The Tabletop field's size from its setting (the hands and the left stick also change it in play).
void apply_table_scale_setting() {
  g.table.scale = std::max(kTableScaleMin, std::min(kTableScaleMax, kTableScaleBase * g.set.tableScale));
  if (g.tableSet) g.tablePoseDirty = true;  // placed already: tell the other participants
}

void apply_aurora_settings() {
  aurora_xr_set_texture_anisotropy(static_cast<unsigned>(sv_env_int("STRIKERS_XR_ANISO", 8, 1, 16)));
  aurora_xr_set_vertex_decode(sv_env_int("STRIKERS_XR_DECODE", 2, 0, 2));
}

// Placement from the match window: works without tracked controllers. Commands: screen-here,
// screen-reset, table-here, table-up/down (5 cm), table-near/far (10 cm), table-left/right (15 deg).
void place_command(const char* value) {
  const char* c = std::strchr(value, ':');
  c = c != nullptr ? c + 1 : value;
  Mat head;
  if (sv_xrhost_head(CACurrentMediaTime(), head.m) < 0) head = g.lastHead;
  float fx = -head.m[8], fz = -head.m[10];
  const float len = std::max(1e-3f, std::sqrt(fx * fx + fz * fz));
  fx /= len;
  fz /= len;
  const V3 h = mat_translation(head);
  if (std::strcmp(c, "screen-reset") == 0) {
    g.screenUser = false;
    g.screenGrab = false;
    SVLOG("screen: back to its place");
  } else if (std::strcmp(c, "screen-here") == 0) {
    const float d = (g.tabletop && !g.stereoFailed) ? 1.1f : kScreenPresets[g.set.screenPreset].distance;
    g.screenUserPos = V3{h.x + fx * d, h.y - 0.05f, h.z + fz * d};
    g.screenUserYaw = std::atan2(-fx, -fz);
    g.screenUser = true;
    g.screenGrab = false;
    SVLOG("screen: put in front of you at (%.2f, %.2f, %.2f)", g.screenUserPos.x, g.screenUserPos.y, g.screenUserPos.z);
  } else if (std::strcmp(c, "table-here") == 0) {
    if (g.tabletop) g.bringTable = true;
  } else if (std::strncmp(c, "table-", 6) == 0) {
    const char* m = c + 6;
    // Near/far along the line from you to the table.
    float dx = g.table.position.x - h.x, dz = g.table.position.z - h.z;
    const float dl = std::max(1e-3f, std::sqrt(dx * dx + dz * dz));
    dx /= dl;
    dz /= dl;
    if (std::strcmp(m, "up") == 0) g.table.position.y += 0.05f;
    else if (std::strcmp(m, "down") == 0) g.table.position.y = std::max(g.floorY + 0.1f, g.table.position.y - 0.05f);
    else if (std::strcmp(m, "far") == 0) g.table.position = V3{g.table.position.x + dx * 0.1f, g.table.position.y, g.table.position.z + dz * 0.1f};
    else if (std::strcmp(m, "near") == 0 && dl > 0.35f) g.table.position = V3{g.table.position.x - dx * 0.1f, g.table.position.y, g.table.position.z - dz * 0.1f};
    else if (std::strcmp(m, "left") == 0) g.table.yaw += 0.2618f;
    else if (std::strcmp(m, "right") == 0) g.table.yaw -= 0.2618f;
    else return;
    g.tableSet = true;
    g.tablePoseDirty = true;
    SVLOG("table %s: at (%.2f, %.2f, %.2f) yaw %.2f", m, g.table.position.x, g.table.position.y, g.table.position.z,
          g.table.yaw);
  }
}

// Live settings from the app (sv_setting_changed), on the game thread between frames.
void take_settings() {
  char key[96], value[256];
  bool any = false, aurora = false, stadiumScale = false;
  while (sv_game_take_setting(key, sizeof(key), value, sizeof(value))) {
    // Kept in sv_game.mm's table, not the environment: setenv while other threads may getenv is
    // not safe. sv_env_* read the table first.
    sv_setting_override(key, value);
    any = true;
    if (std::strcmp(key, "STRIKERS_XR_ANISO") == 0 || std::strcmp(key, "STRIKERS_XR_DECODE") == 0) aurora = true;
    if (std::strcmp(key, "STRIKERS_XR_STADIUM_SCALE") == 0) stadiumScale = true;
    if (std::strcmp(key, "STRIKERS_XR_PLACE") == 0 && g.screenCmdSeen != value) {
      // The match window's placement buttons (any controller, the DualSense too): "<n>:<command>".
      g.screenCmdSeen = value;
      place_command(value);
    }
    SVLOG("setting %s=%s", key, value[0] != '\0' ? value : "(default)");
  }
  if (!any) return;
  const int oldPreset = g.set.screenPreset;
  const float oldTableScale = g.set.tableScale;
  read_settings();
  if (g.set.tableScale != oldTableScale) {
    apply_table_scale_setting();
    SVLOG("table scale %.2fx (%.4f m per game unit)", g.set.tableScale, g.table.scale);
  }
  if (stadiumScale && g.mrMode) {
    SVLOG("stadium scale %.2fx: no stadium in %s (it applies to the Stadium views)", g.set.stadiumScale, mode_name());
  }
  if (g.set.screenPreset != oldPreset) {
    SVLOG("screen preset %d (%.1f m wide at %.1f m)", g.set.screenPreset, kScreenPresets[g.set.screenPreset].width,
          kScreenPresets[g.set.screenPreset].distance);
  }
  if (aurora) apply_aurora_settings();
}

bool stereo_mode() { return g.tabletop || !g.mrMode; }

int layer_count() {
  if (!stereo_mode()) return 0;
  if (g.tabletop && !g.mrMode) return 2;  // Tabletop · Stadium: the stadium around you, then the field
  return 1;
}

LayerKind layer_kind(int layer) {
  if (g.tabletop && g.mrMode) return kLayerTable;
  if (g.tabletop && !g.mrMode) return layer == 0 ? kLayerStadium : kLayerTable;
  return kLayerStadium;
}

// Quest's LOCAL space had its origin at the head at the start; here the origin is on the floor, so
// Quest's head-relative placements are lifted by the eye height.
V3 local(float x, float y, float z) { return V3{x, y + g.eyeHeight, z}; }

// The stadium: you stand on the near touchline at centre field, feet on the floor, facing -Z.
V3 stadium_origin() {
  const float s = g.set.stadiumScale;
  return V3{0.0f, g.floorY, kStandY * s};
}
M4 stadium_model() {
  const float s = g.set.stadiumScale;
  return m4_game_to_room(s, 0.0f, V3{0.0f, g.floorY, kStandY * s});
}
M4 table_model() { return m4_game_to_room(g.table.scale, g.table.yaw, g.table.position); }

V3 table_point(float gx, float gy, float gz) {
  const M4 t = table_model();
  return V3{gx * t.m[0][0] + gy * t.m[1][0] + gz * t.m[2][0] + t.m[3][0],
            gx * t.m[0][1] + gy * t.m[1][1] + gz * t.m[2][1] + t.m[3][1],
            gx * t.m[0][2] + gy * t.m[1][2] + gz * t.m[2][2] + t.m[3][2]};
}

float character_scale() { return (g.active && g.tabletop && !g.stereoFailed) ? g.set.charScale : 1.0f; }

// ---------------------------------------------------------------------------------------------
// SharePlay: the table's pose
// ---------------------------------------------------------------------------------------------

void send_table_pose(bool force) {
  int participants = 0, spatial = 0;
  sv_game_shareplay(&participants, &spatial);
  if (participants <= 0) {
    g.tablePoseDirty = false;
    return;
  }
  const uint64_t t = now_ns();
  if (!force && t - g.lastTablePoseSentNs < 100000000ull) return;  // 10 a second while moving
  g.lastTablePoseSentNs = t;
  g.tablePoseDirty = false;
  const Mat m = mat_pose_yaw(g.table.position, g.table.yaw);
  sv_game_send_table_pose(m.m, g.table.scale);
}

void take_remote_table_pose() {
  float m[16], scale = 0.0f;
  if (!sv_game_take_remote_table_pose(m, &scale)) return;
  int participants = 0, spatial = 0;
  sv_game_shareplay(&participants, &spatial);
  if (!spatial) {
    // Without a shared (spatial) space each headset's origin is its own: a pose from elsewhere would
    // put the field somewhere arbitrary.
    if (!g.remoteNonSpatialLogged) {
      g.remoteNonSpatialLogged = true;
      SVLOG("shareplay: table pose from a participant ignored (not in a spatial session)");
    }
    return;
  }
  if (g.table.grabHands != 0) return;  // the local hands win while holding it
  g.table.position = V3{m[12], m[13], m[14]};
  g.table.yaw = std::atan2(m[8], m[10]);
  if (scale > 0.0f) g.table.scale = std::max(kTableScaleMin, std::min(kTableScaleMax, scale));
  g.tableSet = true;
}

// ---------------------------------------------------------------------------------------------
// Input: Sense controllers (and any gamepad) -> GameCube pad, as xr_quest.cpp sync_input
// ---------------------------------------------------------------------------------------------

int8_t stick_axis(float v) {
  const float c = std::max(-1.0f, std::min(1.0f, v));
  return static_cast<int8_t>(std::lround(c * 127.0f));
}

void apply_stick(float x, float y, int8_t* outX, int8_t* outY) {
  const float dz = g.set.deadzone;
  const float mag = std::sqrt(x * x + y * y);
  if (mag < dz || mag < 1e-6f) {
    *outX = 0;
    *outY = 0;
    return;
  }
  // Rescaled past the dead zone: a small push still moves, a full push still reaches the rim.
  const float scaled = std::min(1.0f, (mag - dz) / std::max(1e-3f, 1.0f - dz)) / mag;
  *outX = stick_axis(x * scaled);
  *outY = stick_axis(y * scaled);  // GameController +y is up, as is the GameCube's
}

uint8_t trigger_analog(float v) {
  if (v < 0.05f) return 0;
  return static_cast<uint8_t>(std::lround(std::min(1.0f, v) * 255.0f));
}

void apply_rumble() {
  Rumble& r = g.rumble;
  if (!r.startPending && !r.stopPending) return;
  const float strength = static_cast<float>(g.set.haptics) / 100.0f;
  if (r.startPending && strength > 0.0f) {
    sv_xrhost_rumble(std::min(1.0f, r.amplitude * strength), std::max(0.02f, r.seconds));
    ++g.rumblePulses;
  } else if (r.stopPending) {
    sv_xrhost_rumble(0.0f, 0.0f);
  }
  r.startPending = false;
  r.stopPending = false;
}

// The hand on or right next to the Tabletop field (within 8 cm of the pitch and its fence).
bool near_table(const V3& p) {
  const float dx = p.x - g.table.position.x, dz = p.z - g.table.position.z;
  const float cs = std::cos(g.table.yaw), sn = std::sin(g.table.yaw), s = g.table.scale;
  const float gx = (dx * cs - dz * sn) / s, gy = -(dx * sn + dz * cs) / s;
  const float margin = 0.08f / s;
  const float dy = p.y - g.table.position.y;
  return std::fabs(gx) < 22.5f + margin && std::fabs(gy) < 13.5f + margin && dy > -0.12f && dy < 2.5f * s + 0.15f;
}

void sync_input(double time) {
  int32_t handConnected[2] = {0, 0};
  uint32_t handButtons[2] = {0, 0};
  float handAnalog[8] = {};
  int32_t poseValid[2] = {0, 0};
  float grip[32] = {};
  int32_t padConnected = 0;
  uint32_t padButtons = 0;
  float padAnalog[6] = {};
  sv_xrhost_input(time, handConnected, handButtons, handAnalog, poseValid, grip, &padConnected, &padButtons,
                  padAnalog);
  if (!g.inputLogged && (handConnected[0] || handConnected[1] || padConnected)) {
    g.inputLogged = true;
    SVLOG("input: left Sense %d, right Sense %d, gamepad %d", handConnected[0], handConnected[1], padConnected);
  }

  SVPad status{};
  const uint32_t L = handButtons[0], R = handButtons[1];
  // Touch layout on the Sense: right cross/circle = A/B, left square/triangle = X/Y.
  if (R & SV_XRB_LOWER) status.button |= SV_PAD_A;
  if (R & SV_XRB_UPPER) status.button |= SV_PAD_B;
  if (L & SV_XRB_LOWER) status.button |= SV_PAD_X;
  if (L & SV_XRB_UPPER) status.button |= SV_PAD_Y;
  if ((L | R) & SV_XRB_MENU) status.button |= SV_PAD_START;
  // The PS button never leaves the game; optionally it pauses.
  const bool ps = ((L | R) & SV_XRB_PS) != 0 || (padButtons & SV_GPB_HOME) != 0;
  if (ps && !g.prevPs) SVLOG("input: PS button%s", g.set.psButtonStart ? " -> Start" : " (ignored)");
  g.prevPs = ps;
  if (ps && g.set.psButtonStart) status.button |= SV_PAD_START;

  float tl = handAnalog[0 * 4 + 0], tr = handAnalog[1 * 4 + 0];
  float stickLX = handAnalog[0 * 4 + 2], stickLY = handAnalog[0 * 4 + 3];
  float stickRX = handAnalog[1 * 4 + 2], stickRY = handAnalog[1 * 4 + 3];
  bool gripRNow = handAnalog[1 * 4 + 1] > 0.5f;
  float gripL = handAnalog[0 * 4 + 1];
  bool clickL = (L & SV_XRB_STICK_CLICK) != 0;
  bool clickR = (R & SV_XRB_STICK_CLICK) != 0;

  // A gamepad: Aurora's standard layout (south A, east B, west X, north Y, right shoulder Z), its
  // sticks and triggers merged with the Sense controllers' (the bigger deflection wins).
  if (padConnected) {
    if (padButtons & SV_GPB_SOUTH) status.button |= SV_PAD_A;
    if (padButtons & SV_GPB_EAST) status.button |= SV_PAD_B;
    if (padButtons & SV_GPB_WEST) status.button |= SV_PAD_X;
    if (padButtons & SV_GPB_NORTH) status.button |= SV_PAD_Y;
    if (padButtons & SV_GPB_START) status.button |= SV_PAD_START;
    if (padButtons & SV_GPB_RSHOULDER) status.button |= SV_PAD_Z;
    if (padButtons & SV_GPB_UP) status.button |= SV_PAD_UP;
    if (padButtons & SV_GPB_DOWN) status.button |= SV_PAD_DOWN;
    if (padButtons & SV_GPB_LEFT) status.button |= SV_PAD_LEFT;
    if (padButtons & SV_GPB_RIGHT) status.button |= SV_PAD_RIGHT;
    auto bigger = [](float& ax, float& ay, float bx, float by) {
      if (bx * bx + by * by > ax * ax + ay * ay) {
        ax = bx;
        ay = by;
      }
    };
    bigger(stickLX, stickLY, padAnalog[0], padAnalog[1]);
    bigger(stickRX, stickRY, padAnalog[2], padAnalog[3]);
    tl = std::max(tl, padAnalog[4]);
    tr = std::max(tr, padAnalog[5]);
    clickL = clickL || (padButtons & SV_GPB_LSTICK) != 0;
    clickR = clickR || (padButtons & SV_GPB_RSTICK) != 0;
  }

  status.triggerLeft = trigger_analog(tl);
  status.triggerRight = trigger_analog(tr);
  if (tl > 0.9f) status.button |= SV_PAD_L;
  if (tr > 0.9f) status.button |= SV_PAD_R;

  for (int h = 0; h < 2; ++h) {
    g.handValid[h] = poseValid[h] != 0;
    if (g.handValid[h]) g.hand[h] = V3{grip[h * 16 + 12], grip[h * 16 + 13], grip[h * 16 + 14]};
  }
  // Bare hands (no Sense in that hand: a DualSense, or none): a pinch stands in for the grip, but only
  // on the table or the screen, so pinching the windows never moves anything nor presses Z.
  int32_t bareValid[2] = {0, 0}, barePinch[2] = {0, 0};
  float barePos[6] = {};
  sv_xrhost_hands(time, bareValid, barePinch, barePos);
  bool bare[2] = {false, false};
  for (int h = 0; h < 2; ++h) {
    if (g.handValid[h] || handConnected[h] || !bareValid[h]) {
      // A Sense in that hand (even with its tracking lost for a moment) keeps its own buttons.
      g.barePinchOk[h] = g.barePinchTable[h] = false;
      if (!bareValid[h]) g.barePinchPrev[h] = false;
      continue;
    }
    bare[h] = true;
    g.handValid[h] = true;
    g.hand[h] = V3{barePos[h * 3 + 0], barePos[h * 3 + 1], barePos[h * 3 + 2]};
    const bool down = barePinch[h] != 0;
    if (down && !g.barePinchPrev[h]) {
      // Decided when the pinch starts: on (or right by) the table or within reach of the screen.
      const V3& p = g.hand[h];
      const float dx = p.x - g.lastScreenPos.x, dy = p.y - g.lastScreenPos.y, dz = p.z - g.lastScreenPos.z;
      const float reach = std::max(0.3f, 0.5f * g.lastScreenWidth);
      const bool onScreen = g.lastScreenShown && g.lastScreenMovable && dx * dx + dy * dy + dz * dz < reach * reach;
      const bool onTable = g.tabletop && !g.stereoFailed && near_table(p);
      // The table wins where both are in reach, for either hand.
      g.barePinchTable[h] = onTable;
      g.barePinchOk[h] = onScreen || onTable;
      if (g.barePinchOk[h]) SVLOG("input: %s hand pinch on the %s", h == 0 ? "left" : "right", onScreen ? "screen" : "table");
    }
    if (!down) g.barePinchOk[h] = g.barePinchTable[h] = false;
    g.barePinchPrev[h] = down;
  }
  if (bare[0]) gripL = g.barePinchOk[0] ? 1.0f : 0.0f;
  if (bare[1]) gripRNow = g.barePinchOk[1];

  // Tabletop: grabbing the field. The left grip grabs it from anywhere; the right grip grabs it when
  // the hand is on or right next to it, and anywhere else stays the GameCube Z (the item swap). One
  // hand carries it; with both, pulling them apart or together resizes it and turning them around
  // each other turns it. The left hand alone also resizes (stick up/down) and turns (left/right) it.
  const V3 handL = g.hand[0], handR = g.hand[1];
  const bool haveL = g.handValid[0], haveR = g.handValid[1];
  if (gripRNow && !g.prevGripR) g.table.rightGrab = g.tabletop && !g.stereoFailed && haveR && near_table(handR);
  if (!gripRNow) g.table.rightGrab = false;
  if (gripRNow && !g.table.rightGrab && !bare[1]) status.button |= SV_PAD_Z;
  g.prevGripR = gripRNow;
  // The flat picture: the left grip on it (anywhere in the big-screen views, within reach of it at the
  // table) carries it; a bare-hand pinch on it does too, with either hand. It then stays where it was
  // let go, facing you.
  const bool rightBareOnScreen = bare[1] && g.barePinchOk[1] && !g.barePinchTable[1] && !g.table.rightGrab;
  const bool leftHolds = gripL > 0.6f && haveL && !(bare[0] && g.barePinchTable[0]);
  if (g.screenGrab && !(g.screenGrabHand == 0 ? leftHolds : rightBareOnScreen)) {
    SVLOG("screen placed at (%.2f, %.2f, %.2f) yaw %.2f", g.screenUserPos.x, g.screenUserPos.y, g.screenUserPos.z,
          g.screenUserYaw);
    g.screenGrab = false;
  } else if (!g.screenGrab && g.table.grabHands == 0 && g.lastScreenShown && g.lastScreenMovable &&
             (leftHolds || rightBareOnScreen)) {
    const int h = leftHolds ? 0 : 1;
    const V3& hp = g.hand[h];
    const float dx = hp.x - g.lastScreenPos.x, dy = hp.y - g.lastScreenPos.y, dz = hp.z - g.lastScreenPos.z;
    const float reach = std::max(0.3f, 0.5f * g.lastScreenWidth);
    if ((!g.tabletop && !bare[h]) || dx * dx + dy * dy + dz * dz < reach * reach) {
      if (!g.screenUser) {
        g.screenUser = true;
        g.screenUserPos = g.lastScreenPos;
        g.screenUserYaw = g.lastScreenYaw;
      }
      g.screenGrab = true;
      g.screenGrabHand = h;
      g.screenGrabOffset = V3{g.screenUserPos.x - hp.x, g.screenUserPos.y - hp.y, g.screenUserPos.z - hp.z};
    }
  }
  if (g.screenGrab) {
    const V3& hp = g.hand[g.screenGrabHand];
    g.screenUserPos = V3{hp.x + g.screenGrabOffset.x, hp.y + g.screenGrabOffset.y, hp.z + g.screenGrabOffset.z};
    // lastHead is only refreshed while no eye images are shown: the predicted head is current.
    const V3 head = mat_translation(g.viewsValid ? g.predicted.originFromDevice : g.lastHead);
    g.screenUserYaw = std::atan2(head.x - g.screenUserPos.x, head.z - g.screenUserPos.z);
  }
  const bool holdL = g.tabletop && gripL > 0.6f && !(g.screenGrab && g.screenGrabHand == 0) &&
                     (!bare[0] || g.barePinchTable[0]);
  const int hands = (holdL && haveL ? 1 : 0) | (g.table.rightGrab && haveR ? 2 : 0);
  if (hands != g.table.grabHands) {
    if (hands == 3) {
      const float dx = handR.x - handL.x, dy = handR.y - handL.y, dz = handR.z - handL.z;
      g.table.twoDist = std::max(0.05f, std::sqrt(dx * dx + dy * dy + dz * dz));
      g.table.prevAngle = std::atan2(dz, dx);
      g.table.turn = 0.0f;
      g.table.twoScale = g.table.scale;
      g.table.twoYaw = g.table.yaw;
      g.table.twoPos = g.table.position;
      g.table.twoMid = V3{0.5f * (handL.x + handR.x), 0.5f * (handL.y + handR.y), 0.5f * (handL.z + handR.z)};
    } else if (hands != 0) {
      const V3& p = hands == 1 ? handL : handR;
      g.table.grabOffset = V3{g.table.position.x - p.x, g.table.position.y - p.y, g.table.position.z - p.z};
    } else {
      SVLOG("table at (%.2f, %.2f, %.2f) yaw %.2f scale %.4f", g.table.position.x, g.table.position.y,
            g.table.position.z, g.table.yaw, g.table.scale);
      send_table_pose(true);
    }
    g.table.grabHands = hands;
  }
  if (hands == 3) {
    const float dx = handR.x - handL.x, dy = handR.y - handL.y, dz = handR.z - handL.z;
    const float dist = std::max(0.05f, std::sqrt(dx * dx + dy * dy + dz * dz));
    float step = std::atan2(dz, dx) - g.table.prevAngle;
    if (step > 3.14159265f) step -= 6.2831853f;
    if (step < -3.14159265f) step += 6.2831853f;
    g.table.prevAngle += step;
    g.table.turn += step;
    g.table.scale = std::max(kTableScaleMin, std::min(kTableScaleMax, g.table.twoScale * dist / g.table.twoDist));
    const float k = g.table.scale / g.table.twoScale;
    g.table.yaw = g.table.twoYaw - g.table.turn;
    const V3 mid{0.5f * (handL.x + handR.x), 0.5f * (handL.y + handR.y), 0.5f * (handL.z + handR.z)};
    const float vx = g.table.twoPos.x - g.table.twoMid.x, vz = g.table.twoPos.z - g.table.twoMid.z;
    const float cs = std::cos(g.table.turn), sn = std::sin(g.table.turn);
    g.table.position = V3{mid.x + (vx * cs - vz * sn) * k, mid.y + (g.table.twoPos.y - g.table.twoMid.y) * k,
                          mid.z + (vx * sn + vz * cs) * k};
    g.tablePoseDirty = true;
  } else if (hands != 0) {
    const V3& p = hands == 1 ? handL : handR;
    g.table.position = V3{p.x + g.table.grabOffset.x, p.y + g.table.grabOffset.y, p.z + g.table.grabOffset.z};
    g.tablePoseDirty = true;
  }
  if (holdL) {
    if (hands == 1 && std::fabs(stickLY) > 0.2f) {
      g.table.scale = std::max(kTableScaleMin, std::min(kTableScaleMax, g.table.scale * std::exp(stickLY * 0.025f)));
      g.tablePoseDirty = true;
    }
    if (hands == 1 && std::fabs(stickLX) > 0.2f) {
      g.table.yaw -= stickLX * 0.035f;
      g.tablePoseDirty = true;
    }
    stickLX = stickLY = 0.0f;
  }
  if (g.tablePoseDirty) send_table_pose(false);

  const uint32_t itemButtons = status.button & (SV_PAD_X | SV_PAD_Y | SV_PAD_Z);
  if ((itemButtons & ~g.prevItemButtons) != 0) {
    SVLOG("input: %s%s%s", (itemButtons & ~g.prevItemButtons & SV_PAD_X) ? "X (use item) " : "",
          (itemButtons & ~g.prevItemButtons & SV_PAD_Z) ? "Z (swap item) " : "",
          (itemButtons & ~g.prevItemButtons & SV_PAD_Y) ? "Y " : "");
  }
  g.prevItemButtons = itemButtons;
  apply_stick(stickLX, stickLY, &status.stickX, &status.stickY);
  apply_stick(stickRX, stickRY, &status.substickX, &status.substickY);

  // Stick clicks, the two buttons a GameCube pad never had. Double taps switch modes, so a stray
  // click mid-play does nothing:
  //   left  double tap: Living Room <-> Stadium      left  single tap: the table comes to you
  //   right double tap: Jumbotron <-> Tabletop       right single tap: next screen size
  const uint64_t t = now_ns();
  if (clickL && !g.prevStickClickL) {
    if (g.pendingTapL != 0 && t - g.pendingTapL < kDoubleTapNs) {
      g.pendingTapL = 0;
      set_mode(g.tabletop, !g.mrMode);
    } else {
      g.pendingTapL = t;
    }
  }
  if (g.pendingTapL != 0 && t - g.pendingTapL >= kDoubleTapNs) {
    g.pendingTapL = 0;
    if (g.tabletop) g.bringTable = true;
  }
  if (clickR && !g.prevStickClickR) {
    if (g.pendingTapR != 0 && t - g.pendingTapR < kDoubleTapNs) {
      g.pendingTapR = 0;
      set_mode(!g.tabletop, g.mrMode);
    } else {
      g.pendingTapR = t;
    }
  }
  if (g.pendingTapR != 0 && t - g.pendingTapR >= kDoubleTapNs) {
    g.pendingTapR = 0;
    g.set.screenPreset = (g.set.screenPreset + 1) % kScreenPresetCount;
    // Also into the settings table, so a later re-read agrees.
    char v[8];
    std::snprintf(v, sizeof(v), "%d", g.set.screenPreset);
    sv_setting_override("STRIKERS_XR_SCREEN_SIZE", v);
    SVLOG("screen preset %d (%.1f m wide at %.1f m)", g.set.screenPreset, kScreenPresets[g.set.screenPreset].width,
          kScreenPresets[g.set.screenPreset].distance);
  }
  g.prevStickClickL = clickL;
  g.prevStickClickR = clickR;

  apply_rumble();
  g.localPad = status;
}

// ---------------------------------------------------------------------------------------------
// Renderer hand-over
// ---------------------------------------------------------------------------------------------

void drop_renderer(const char* why) {
  if (g.renderer != nullptr) {
    SVLOG("cp: renderer released (%s)", why);
    CFRelease(g.renderer);
    g.renderer = nullptr;
  }
  g.rendererRunning = false;
  g.lastRendererState = -1;
  // The next renderer starts from a clean picture; the eye images stay (same size, same device).
  g.hasProjection = false;
  g.shownSlot = -1;
  for (bool& h : g.eyeHanded) h = false;  // their poses belong to the old renderer
}

bool build_pipelines(id<MTLDevice> device, MTLPixelFormat color, MTLPixelFormat depth);

// Everything a frame needs from the new renderer's device, before its first frame: the shader
// library, the composite's pipelines and the effects, so the first frames are not spent compiling.
void prepare_renderer() {
  id<MTLDevice> device = (__bridge id<MTLDevice>)sv_xrhost_renderer_device(g.renderer);
  uint32_t color = 0, depth = 0;
  sv_xrhost_renderer_formats(g.renderer, &color, &depth);
  if (device == nil || color == 0) {
    SVWARN("cp: renderer without device or colour format (%u); pipelines wait for the first frame", color);
    return;
  }
  if (depth == 0) depth = static_cast<uint32_t>(MTLPixelFormatDepth32Float);
  const uint64_t t0 = now_ns();
  if (build_pipelines(device, static_cast<MTLPixelFormat>(color), static_cast<MTLPixelFormat>(depth))) {
    g.fxReady = svfx::init((__bridge void*)device, color);
  }
  SVLOG("cp: renderer prepared in %.1f ms (colour %u, depth %u, effects %d)",
        static_cast<double>(now_ns() - t0) / 1e6, color, depth, g.fxReady ? 1 : 0);
}

// True when there is a running renderer to draw into. Otherwise the caller waits.
bool ensure_renderer() {
  uint32_t generation = 0;
  void* current = sv_game_copy_renderer(&generation);  // +1 retained, or NULL
  if (generation != g.rendererGeneration) {
    g.rendererGeneration = generation;
    drop_renderer("replaced");
    if (current != nullptr) {
      g.renderer = current;
      current = nullptr;
      SVLOG("cp: renderer %u taken", generation);
      g.waitingLogged = false;
      g.pausedSinceNs = 0;
      sv_xrhost_session_retry();
      prepare_renderer();
    }
  }
  if (current != nullptr) CFRelease(current);
  if (g.renderer == nullptr) {
    if (!g.waitingLogged) {
      g.waitingLogged = true;
      SVLOG("cp: waiting for the immersive space (no renderer); the game is paused");
    }
    return false;
  }
  const int state = sv_xrhost_renderer_state(g.renderer);
  if (state != g.lastRendererState) {
    static const char* const kStates[] = {"paused", "running", "invalidated"};
    SVLOG("cp: renderer state %s", (state >= 0 && state <= 2) ? kStates[state] : "?");
    g.lastRendererState = state;
  }
  if (state == 2) {
    drop_renderer("invalidated");
    g.lastRendererState = 2;
    return false;
  }
  g.rendererRunning = state == 1;
  if (state == 0) {
    const uint64_t t = now_ns();
    if (g.pausedSinceNs == 0) {
      g.pausedSinceNs = t;
      g.pausedWarnedNs = 0;
    } else if (t - g.pausedSinceNs > 5000000000ull && t - g.pausedWarnedNs > 30000000000ull) {
      g.pausedWarnedNs = t;
      SVWARN("cp: the renderer has been paused for %.0f s (space hidden or not yet shown); the game waits",
             static_cast<double>(t - g.pausedSinceNs) / 1e9);
    }
  } else {
    g.pausedSinceNs = 0;
  }
  return g.rendererRunning;
}

// ---------------------------------------------------------------------------------------------
// Shared images (IOSurface <-> Dawn <-> Metal)
// ---------------------------------------------------------------------------------------------

// Runs `fn` inside a Dawn validation error scope and waits for the verdict: Aurora treats any
// uncaught Dawn error as fatal, and an import refused by Dawn must become a log line, not a crash.
template <typename Fn>
bool dawn_scoped(const char* what, Fn&& fn) {
  auto& device = aurora::webgpu::g_device;
  device.PushErrorScope(wgpu::ErrorFilter::Validation);
  fn();
  // Owned jointly with the callback: should the wait time out, a late callback still writes into
  // live memory, not into this returned frame.
  struct Verdict {
    std::atomic<bool> done{false};
    bool failed = false;
    std::string message;
  };
  auto verdict = std::make_shared<Verdict>();
  const auto future = device.PopErrorScope(
      wgpu::CallbackMode::WaitAnyOnly,
      [verdict](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView msg) {
        if (status != wgpu::PopErrorScopeStatus::Success || type != wgpu::ErrorType::NoError) {
          verdict->failed = true;
          verdict->message = std::string{std::string_view{msg}};
        }
        verdict->done.store(true);
      });
  aurora::webgpu::g_instance.WaitAny(future, 5000000000ull);
  if (!verdict->done.load() || verdict->failed) {
    SVERR("dawn: %s failed: %s", what, verdict->done.load() ? verdict->message.c_str() : "error scope timed out");
    return false;
  }
  return true;
}

void release_shared(SharedImage& img) {
  img.view = nullptr;
  img.texture = nullptr;
  img.memory = nullptr;
  img.mtl = nil;
  img.mtlSrgb = nil;
  img.doneEvent = nil;
  if (img.surface != nullptr) {
    CFRelease(img.surface);
    img.surface = nullptr;
  }
  img = SharedImage{};
}

bool create_shared(SharedImage& img, uint32_t width, uint32_t height, const char* what, bool depth = false) {
  release_shared(img);
  img.isDepth = depth;
  const wgpu::TextureFormat format = wgpu::TextureFormat::RGBA8Unorm;
  NSDictionary* props = @{
    (__bridge NSString*)kIOSurfaceWidth : @(width),
    (__bridge NSString*)kIOSurfaceHeight : @(height),
    (__bridge NSString*)kIOSurfaceBytesPerElement : @4,
    (__bridge NSString*)kIOSurfacePixelFormat : @(kCVPixelFormatType_32RGBA),
    (__bridge NSString*)kIOSurfaceBytesPerRow : @(IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, width * 4)),
  };
  img.surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
  if (img.surface == nullptr) {
    SVERR("dawn: IOSurfaceCreate %ux%u failed (%s)", width, height, what);
    return false;
  }
  img.width = width;
  img.height = height;
  const bool ok = dawn_scoped(what, [&] {
    wgpu::SharedTextureMemoryIOSurfaceDescriptor io{};
    io.ioSurface = img.surface;
    io.allowStorageBinding = false;
    wgpu::SharedTextureMemoryDescriptor desc{};
    desc.nextInChain = &io;
    img.memory = aurora::webgpu::g_device.ImportSharedTextureMemory(&desc);
    if (!img.memory) return;
    wgpu::TextureDescriptor tex{};
    tex.format = format;
    tex.size = {width, height, 1};
    tex.usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::RenderAttachment |
                wgpu::TextureUsage::TextureBinding;
    img.texture = img.memory.CreateTexture(&tex);
    if (!img.texture) return;
    wgpu::TextureViewDescriptor view{};
    view.format = format;
    view.dimension = wgpu::TextureViewDimension::e2D;
    view.mipLevelCount = 1;
    view.arrayLayerCount = 1;
    img.view = img.texture.CreateView(&view);
  });
  if (!ok || !img.memory || !img.texture || !img.view) {
    SVERR("dawn: IOSurface %ux%u not usable by Dawn (%s)", width, height, what);
    release_shared(img);
    return false;
  }
  return true;
}

bool end_access(SharedImage& img);
void pick_screen_slot();

// Opens Dawn's access. Dawn's GPU work waits for the last composite that read the image.
bool begin_access(SharedImage& img) {
  if (img.accessOpen) return true;
  wgpu::SharedTextureMemoryBeginAccessDescriptor desc{};
  desc.concurrentRead = false;
  desc.initialized = true;
  uint64_t value = img.readValue;
  if (value != 0 && g.compositeFence) {
    desc.fenceCount = 1;
    desc.fences = &g.compositeFence;
    desc.signaledValueCount = 1;
    desc.signaledValues = &value;
  } else {
    desc.fenceCount = 0;
    desc.fences = nullptr;
    desc.signaledValueCount = 0;
    desc.signaledValues = nullptr;
  }
  // Scoped: a validation error here would otherwise reach Aurora's uncaptured-error handler (fatal).
  bool opened = false;
  const bool clean = dawn_scoped("BeginAccess", [&] {
    opened = static_cast<bool>(img.memory.BeginAccess(img.texture, &desc));
  });
  img.accessOpen = opened;
  if (!clean || !opened) {
    SVERR("dawn: BeginAccess refused (%ux%u)", img.width, img.height);
    if (opened) end_access(img);
    img.accessOpen = false;
    img.refused = true;
    return false;
  }
  return true;
}

// Ends Dawn's access and keeps its fence for the composite. False: no picture (never written, or no
// fence, which would leave the composite unordered against Dawn).
bool end_access(SharedImage& img) {
  if (!img.accessOpen) return false;
  img.accessOpen = false;
  wgpu::SharedTextureMemoryEndAccessState state{};
  bool ok = false;
  const bool clean = dawn_scoped("EndAccess", [&] { ok = static_cast<bool>(img.memory.EndAccess(img.texture, &state)); });
  if (!ok || !clean) {
    SVERR("dawn: EndAccess failed (%ux%u)", img.width, img.height);
    img.valid = false;
    img.refused = true;
    return false;
  }
  id<MTLSharedEvent> event = nil;
  uint64_t value = 0;
  for (size_t i = 0; i < state.fenceCount; ++i) {
    wgpu::SharedFenceMTLSharedEventExportInfo mtlInfo{};
    wgpu::SharedFenceExportInfo info{};
    info.nextInChain = &mtlInfo;
    state.fences[i].ExportInfo(&info);
    if (info.type != wgpu::SharedFenceType::MTLSharedEvent || mtlInfo.sharedEvent == nullptr) continue;
    // A pending fence handed back unused (the composite's own event) says nothing about Dawn's writes.
    if (g.compositeEvent != nil && mtlInfo.sharedEvent == (__bridge void*)g.compositeEvent) continue;
    const uint64_t v = i < state.signaledValueCount ? state.signaledValues[i] : 0;
    if (event == nil || v > value) {
      event = (__bridge id<MTLSharedEvent>)mtlInfo.sharedEvent;
      value = v;
    }
  }
  if (event == nil) {
    // Dawn hands back no fence when the texture was not used since BeginAccess.
    img.valid = false;
    return false;
  }
  img.doneEvent = event;
  img.doneValue = value;
  img.valid = true;
  return true;
}

// The composite's own event, shared with Dawn as a fence its BeginAccess waits on.
bool ensure_composite_event() {
  if (g.compositeEvent != nil && g.compositeFence) return true;
  if (g.compositeEvent == nil) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    g.compositeEvent = [device newSharedEvent];
    if (g.compositeEvent == nil) {
      SVERR("metal: newSharedEvent failed; 3D modes and the GPU screen path are off");
      return false;
    }
  }
  return dawn_scoped("ImportSharedFence(composite event)", [&] {
    wgpu::SharedFenceMTLSharedEventDescriptor ev{};
    ev.sharedEvent = (__bridge void*)g.compositeEvent;
    wgpu::SharedFenceDescriptor desc{};
    desc.nextInChain = &ev;
    g.compositeFence = aurora::webgpu::g_device.ImportSharedFence(&desc);
  }) && static_cast<bool>(g.compositeFence);
}

// ---------------------------------------------------------------------------------------------
// The flat picture
// ---------------------------------------------------------------------------------------------

void use_cpu_screen(const char* why) {
  if (!g.gpuScreen) return;
  g.gpuScreen = false;
  for (SharedImage& s : g.screens) release_shared(s);
  g.screenShown = -1;
  for (bool& h : g.screenHanded) h = false;
  aurora_xr_set_frame_callback(&on_aurora_frame, nullptr);
  SVWARN("screen: GPU copy unavailable (%s); reading frames back to the CPU instead", why);
}

// After synchronize(): the newest game frame into the next screen IOSurface, copied by Dawn.
void copy_screen_gpu() {
  if (g.gpuScreenError.load()) {
    use_cpu_screen("a Dawn validation error in the copy");
    return;
  }
  const auto& src = aurora::webgpu::present_source();
  if (!src.texture || src.size.width == 0 || src.size.height == 0) return;
  if (src.format != wgpu::TextureFormat::RGBA8Unorm) {
    use_cpu_screen("Aurora's frame is not RGBA8Unorm");
    return;
  }
  pick_screen_slot();
  SharedImage& img = g.screens[g.screenNext];
  if (img.width != src.size.width || img.height != src.size.height || !img.memory) {
    if (!create_shared(img, src.size.width, src.size.height, "screen image")) {
      use_cpu_screen("IOSurface import");
      return;
    }
    SVLOG("screen: GPU copy into a %ux%u IOSurface", img.width, img.height);
  }
  if (!begin_access(img)) {
    use_cpu_screen("BeginAccess");
    return;
  }
  auto& device = aurora::webgpu::g_device;
  device.PushErrorScope(wgpu::ErrorFilter::Validation);
  wgpu::CommandEncoderDescriptor encDesc{};
  encDesc.label = "SV screen copy";
  wgpu::CommandEncoder encoder = device.CreateCommandEncoder(&encDesc);
  wgpu::TexelCopyTextureInfo from{};
  from.texture = src.texture;
  wgpu::TexelCopyTextureInfo to{};
  to.texture = img.texture;
  const wgpu::Extent3D extent{img.width, img.height, 1};
  encoder.CopyTextureToTexture(&from, &to, &extent);
  wgpu::CommandBuffer commands = encoder.Finish();
  aurora::webgpu::g_queue.Submit(1, &commands);
  // Not waited for: a refusal is reported later (any thread) and switches to the CPU path then.
  device.PopErrorScope(wgpu::CallbackMode::AllowSpontaneous,
                       [](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView msg) {
                         if (status == wgpu::PopErrorScopeStatus::Success && type != wgpu::ErrorType::NoError) {
                           SVERR("dawn: screen copy: %.*s", static_cast<int>(msg.length), msg.data);
                           g.gpuScreenError.store(true);
                         }
                       });
  if (end_access(img)) {
    g.screenHanded[g.screenNext] = true;  // shown once the GPU has finished it (select_shown_screen)
    g.screenSeq[g.screenNext] = ++g.screenSeqCounter;
  } else if (img.refused) {
    use_cpu_screen("EndAccess");
  }
}

// ---------------------------------------------------------------------------------------------
// 3D modes: eye images
// ---------------------------------------------------------------------------------------------

bool create_stereo_resources() {
  if (g.stereoFailed) return false;
  if (!g.drawableKnown || g.viewportW <= 0.0f) return false;
  const uint32_t w = (static_cast<uint32_t>(g.viewportW * g.set.eyeScale) / 8) * 8;
  const uint32_t h = (static_cast<uint32_t>(g.viewportH * g.set.eyeScale) / 8) * 8;
  if (g.stereoReady && w == g.eyeWidth && h == g.eyePxHeight) return true;
  auto fail = [](const char* why) {
    SVERR("3D modes unavailable: %s (the Jumbotron still works)", why);
    g.stereoFailed = true;
    report_mode();
    return false;
  };
  if (!aurora::webgpu::g_metalSharedImageSupported) return fail("Dawn has no IOSurface / MTLSharedEvent sharing");
  if (aurora::gfx::color_format() != wgpu::TextureFormat::RGBA8Unorm) return fail("scene format is not RGBA8Unorm");
  if (w < 64 || h < 64) return fail("no eye size from the drawable");
  if (!ensure_composite_event()) return fail("the composite's fence could not be shared with Dawn");
  g.stereoReady = false;
  g.hasProjection = false;
  g.shownSlot = -1;
  for (bool& h : g.eyeHanded) h = false;
  for (int s = 0; s < kEyeSlots; ++s)
    for (int e = 0; e < 2; ++e) {
      if (!create_shared(g.eyes[s][e], w, h, "eye image")) return fail("eye image import");
    }
  // The depth images are optional: without them the composite estimates the depth (table plane).
  g.depthExport = true;
  for (int s = 0; s < kEyeSlots && g.depthExport; ++s)
    for (int e = 0; e < 2; ++e) {
      if (!create_shared(g.eyeDepths[s][e], w, h, "eye depth", true)) {
        g.depthExport = false;
        break;
      }
    }
  if (!g.depthExport) {
    for (auto& slot : g.eyeDepths)
      for (SharedImage& img : slot) release_shared(img);
    SVWARN("3D modes: eye depth images not importable; the system gets an estimated depth (table plane)");
  }
  g.eyeWidth = w;
  g.eyePxHeight = h;
  g.eyeSlot = 0;
  g.stereoReady = true;
  SVLOG("3D modes ready: %ux%u per eye (drawable viewport %.0fx%.0f x %.2f), GPU hand-off, depth %s", w, h,
        g.viewportW, g.viewportH, g.set.eyeScale, g.depthExport ? "exported" : "estimated");
  return true;
}

bool layers_fit(const uint32_t used[kPools], const int* kinds, int count) {
  for (int p = 0; p < kPools; ++p) {
    if (g.poolReserved[p] == 0) continue;
    double need = used[p];
    for (int l = 0; l < count; ++l) need += 2.0 * g.perSend[kinds[l]][p] * kPoolMargin;
    if (p == 0) need += g.decodedPerFrame * kPoolMargin;
    if (need > g.poolReserved[p] * 0.98) return false;
  }
  return true;
}

void close_pool_measurement() {
  if (!g.measuring) return;
  g.measuring = false;
  uint32_t used[kPools];
  aurora_gfx_pool_usage(used, kPools);
  const unsigned long long decoded = aurora_xr_decoded_bytes() - g.decodedAtMeasure;
  for (int p = 0; p < kPools; ++p) {
    float delta = used[p] > g.measureStart[p] ? static_cast<float>(used[p] - g.measureStart[p]) : 0.0f;
    if (p == 0) delta = std::max(0.0f, delta - static_cast<float>(decoded));
    float& est = g.perSend[g.measuringKind][p];
    est = delta > est ? delta : est * 0.99f + delta * 0.01f;
  }
}

// After synchronize(): this game frame's eye images back from Dawn, shown from now on.
void stop_depth_export(const char* why) {
  if (!g.depthExport) return;
  g.depthExport = false;
  SVWARN("3D modes: eye depth no longer exported (%s); the system gets an estimated depth", why);
  for (auto& slot : g.eyeDepths)
    for (SharedImage& img : slot) {
      if (img.accessOpen) end_access(img);
    }
}

// Ends Dawn's access of this slot's depth images; a refusal turns the export off (the pictures stay).
void end_depth_access(int slot) {
  if (!g.depthExport) return;
  for (int e = 0; e < 2; ++e) {
    SharedImage& img = g.eyeDepths[slot][e];
    if (img.accessOpen) end_access(img);
    if (img.refused) stop_depth_export("Dawn refused the hand-back");
  }
}

// The slot the game thread writes next (g_lock held): never the pair on show, and the unhanded
// ones / the oldest handed-back ones first, so the newest pending pairs survive. A handed-back pair
// that was never shown is dropped here (the GPU was slower than the game).
void pick_eye_slot() {
  int best = -1;
  for (int s = 0; s < kEyeSlots; ++s) {
    if (s == g.shownSlot) continue;
    if (best < 0) {
      best = s;
      continue;
    }
    const uint64_t a = g.eyeHanded[s] ? g.eyeSeq[s] : 0, b = g.eyeHanded[best] ? g.eyeSeq[best] : 0;
    if (a < b) best = s;
  }
  if (best < 0) best = 0;  // unreachable with more than one slot
  g.eyeSlot = best;
  g.eyeHanded[best] = false;
}

void pick_screen_slot() {
  int best = -1;
  for (int s = 0; s < kScreenSlots; ++s) {
    if (s == g.screenShown) continue;
    if (best < 0) {
      best = s;
      continue;
    }
    const uint64_t a = g.screenHanded[s] ? g.screenSeq[s] : 0, b = g.screenHanded[best] ? g.screenSeq[best] : 0;
    if (a < b) best = s;
  }
  if (best < 0) best = 0;
  g.screenNext = best;
  g.screenHanded[best] = false;
}

// Has the GPU finished Dawn's writes of this image (so a wait on it cannot stall)?
bool image_done(const SharedImage& img) {
  return img.valid && img.doneEvent != nil && img.doneEvent.signaledValue >= img.doneValue;
}

// Display thread, g_lock held: switch to the newest handed-back pair the GPU has finished (both eyes
// and, when exported, their depth); otherwise keep showing the previous completed one with its pose.
void select_shown_eyes() {
  int newest = -1;
  uint64_t newestSeq = 0;
  for (int s = 0; s < kEyeSlots; ++s) {
    if (g.eyeHanded[s] && g.eyeSeq[s] > newestSeq) {
      newest = s;
      newestSeq = g.eyeSeq[s];
    }
  }
  if (newest < 0) return;
  const bool anyNewer = g.shownSlot < 0 || newestSeq > g.shownSeq;
  if (anyNewer) {
    ++g.eyeChecksSinceStats;
    int pick = -1;
    uint64_t pickSeq = 0;
    for (int s = 0; s < kEyeSlots; ++s) {
      if (!g.eyeHanded[s] || (g.shownSlot >= 0 && g.eyeSeq[s] <= g.shownSeq) || g.eyeSeq[s] <= pickSeq) continue;
      bool done = image_done(g.eyes[s][0]) && image_done(g.eyes[s][1]);
      if (done && g.depthExport) {
        for (int e = 0; e < 2; ++e) {
          const SharedImage& d = g.eyeDepths[s][e];
          if (d.valid && d.doneEvent != nil && d.doneEvent.signaledValue < d.doneValue) done = false;
        }
      }
      if (done) {
        pick = s;
        pickSeq = g.eyeSeq[s];
      }
    }
    if (pick >= 0) {
      if (g.shownSlot >= 0) {
        ++g.framesPerImage[std::min<uint64_t>(std::max<uint64_t>(g.framesOnShown, 1), 3) - 1];
      }
      g.framesOnShown = 0;
      g.shownSlot = pick;
      g.shownSeq = pickSeq;
      g.shownPose = g.eyePose[pick];
      if (pickSeq != newestSeq) ++g.eyeNotReadySinceStats;  // a newer pair exists but is still running
    } else {
      ++g.eyeNotReadySinceStats;
    }
  }
  ++g.framesOnShown;
}

// The same for the flat picture.
void select_shown_screen() {
  if (!g.gpuScreen) return;
  int pick = -1;
  uint64_t pickSeq = 0;
  for (int s = 0; s < kScreenSlots; ++s) {
    if (!g.screenHanded[s] || (g.screenShown >= 0 && g.screenSeq[s] <= g.screenShownSeq) || g.screenSeq[s] <= pickSeq)
      continue;
    if (!image_done(g.screens[s])) continue;
    pick = s;
    pickSeq = g.screenSeq[s];
  }
  if (pick >= 0) {
    g.screenShown = pick;
    g.screenShownSeq = pickSeq;
  }
}

void finish_stereo_frame() {
  bool ok = true;
  for (int e = 0; e < 2; ++e) ok = end_access(g.eyes[g.eyeSlot][e]) && ok;
  end_depth_access(g.eyeSlot);
  if (g.eyes[g.eyeSlot][0].refused || g.eyes[g.eyeSlot][1].refused) {
    g.stereoFailed = true;
    g.hasProjection = false;
    SVERR("3D modes off: Dawn refused to hand the eye images back");
    report_mode();
    return;
  }
  if (ok) {
    // How long the GPU takes for this pair, from the hand-off to Dawn's fence.
    SharedImage& done = g.eyes[g.eyeSlot][1];
    if (done.doneEvent != nil) {
      if (g_eventListener == nil) g_eventListener = [[MTLSharedEventListener alloc] init];
      const uint64_t t0 = now_ns();
      [done.doneEvent notifyListener:g_eventListener
                             atValue:done.doneValue
                               block:^(id<MTLSharedEvent>, uint64_t) {
                                 const uint64_t us = (now_ns() - t0) / 1000;
                                 g_eyeGpuUs.fetch_add(us);
                                 g_eyeGpuCount.fetch_add(1);
                                 note_max(g_eyeGpuMaxUs, us);
                               }];
    }
    // Handed back, not yet shown: the display thread switches to it once the GPU has finished it.
    g.eyeHanded[g.eyeSlot] = true;
    g.eyeSeq[g.eyeSlot] = ++g.eyeSeqCounter;
    g.eyePose[g.eyeSlot] = g.predicted;
    g.hasProjection = true;
  }
}

// The head predicted for the next display frame, and both eyes from it (PortXrBeginFrame).
void predict_eyes() {
  g.viewsValid = false;
  if (!g.drawableKnown || g.lastPresentation <= 0.0) return;
  const double t = g.lastPresentation + g.displayPeriod;
  Mat head;
  const int32_t id = sv_xrhost_head(t, head.m);
  if (id < 0) return;
  g.predicted.valid = true;
  g.predicted.originFromDevice = head;
  g.predicted.anchorId = id;
  for (int v = 0; v < 2; ++v) {
    g.predicted.deviceFromView[v] = g.drawableDeviceFromView[v];
    g.predicted.projection[v] = g.drawableProjection[v];
  }
  g.viewsValid = true;
}

// ---------------------------------------------------------------------------------------------
// Metal composite
// ---------------------------------------------------------------------------------------------

struct EyeUniforms {
  float viewProj[16];
  float invViewProj[16];
  float eyePos[4];
  float tableA[4];
  float tableB[4];
  float mode[4];
  float proj[4];
};

struct ScreenUniforms {
  float viewProj[16];
  float model[16];
  float size[4];
  float uvRect[4];
};

}  // namespace

// sv_xr.h: the MSL of sv_shaders.h compiled once per device, shared by the composite and the effects.
extern "C" void* sv_shader_library(void* mtlDevice) {
  static id<MTLDevice> s_device = nil;
  static id<MTLLibrary> s_library = nil;
  id<MTLDevice> device = (__bridge id<MTLDevice>)mtlDevice;
  if (device == nil) return nullptr;
  if (device == s_device) return (__bridge void*)s_library;
  s_device = device;
  s_library = nil;
  NSError* err = nil;
  const uint64_t t0 = now_ns();
  s_library = [device newLibraryWithSource:[NSString stringWithUTF8String:kSvShadersMsl]
                                   options:[MTLCompileOptions new]
                                     error:&err];
  if (s_library == nil) {
    SVERR("metal: shader library failed to compile: %s", err.localizedDescription.UTF8String);
    return nullptr;
  }
  SVLOG("metal: shader library compiled in %.1f ms", static_cast<double>(now_ns() - t0) / 1e6);
  return (__bridge void*)s_library;
}

namespace {

bool build_pipelines(id<MTLDevice> device, MTLPixelFormat color, MTLPixelFormat depth) {
  Metal& m = g.mtl;
  if (m.device == device && m.colorFormat == color && m.depthFormat == depth && m.eyePipeline != nil) return true;
  if (m.failed && m.device == device && m.colorFormat == color && m.depthFormat == depth) return false;
  m.device = device;
  m.colorFormat = color;
  m.depthFormat = depth;
  m.failed = true;
  m.eyePipeline = nil;
  m.screenPipeline = nil;
  m.depthFixupPipeline = nil;
  m.queue = [device newCommandQueue];
  if (m.queue == nil) {
    SVERR("metal: newCommandQueue failed");
    return false;
  }
  id<MTLLibrary> lib = (__bridge id<MTLLibrary>)sv_shader_library((__bridge void*)device);
  if (lib == nil) return false;
  auto make = [&](NSString* vs, NSString* fs, const char* what) -> id<MTLRenderPipelineState> {
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:vs];
    d.fragmentFunction = [lib newFunctionWithName:fs];
    if (d.vertexFunction == nil || d.fragmentFunction == nil) {
      SVERR("metal: shader function missing for %s", what);
      return nil;
    }
    d.colorAttachments[0].pixelFormat = color;
    d.depthAttachmentPixelFormat = depth;
    NSError* perr = nil;
    id<MTLRenderPipelineState> p = [device newRenderPipelineStateWithDescriptor:d error:&perr];
    if (p == nil) SVERR("metal: pipeline %s failed: %s", what, perr.localizedDescription.UTF8String);
    return p;
  };
  m.eyePipeline = make(@"eye_vertex", @"eye_fragment", "eye");
  m.screenPipeline = make(@"screen_vertex", @"screen_fragment", "screen");
  if (m.eyePipeline == nil || m.screenPipeline == nil) return false;
  {
    // Depth only (no colour attachment): the near-only depth of the system, after the composite pass.
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"eye_vertex"];
    d.fragmentFunction = [lib newFunctionWithName:@"eye_depth_fixup"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatInvalid;
    d.depthAttachmentPixelFormat = depth;
    NSError* perr = nil;
    if (d.vertexFunction != nil && d.fragmentFunction != nil) {
      m.depthFixupPipeline = [device newRenderPipelineStateWithDescriptor:d error:&perr];
    }
    // Not fatal: without it mode 1 behaves like mode 0 (depth 0 for the system).
    if (m.depthFixupPipeline == nil) {
      SVWARN("metal: depth fix-up pipeline failed (%s); near-only depth falls back to off",
             perr != nil ? perr.localizedDescription.UTF8String : "function missing");
    }
  }
  MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
  ds.depthCompareFunction = MTLCompareFunctionAlways;
  ds.depthWriteEnabled = YES;
  m.depthAlways = [device newDepthStencilStateWithDescriptor:ds];
  ds.depthCompareFunction = MTLCompareFunctionGreaterEqual;  // reverse-Z: nearer is greater
  m.depthTest = [device newDepthStencilStateWithDescriptor:ds];
  MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
  sd.minFilter = MTLSamplerMinMagFilterLinear;
  sd.magFilter = MTLSamplerMinMagFilterLinear;
  sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
  sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
  m.sampler = [device newSamplerStateWithDescriptor:sd];
  if (m.depthAlways == nil || m.depthTest == nil || m.sampler == nil) {
    SVERR("metal: depth/sampler states failed");
    return false;
  }
  m.failed = false;
  SVLOG("metal: composite ready (colour format %u, depth format %u)", static_cast<unsigned>(color),
        static_cast<unsigned>(depth));
  return true;
}

// The Metal side of a shared image, on the composite's device.
id<MTLTexture> metal_view(SharedImage& img, id<MTLDevice> device) {
  if (img.surface == nullptr) return nil;
  if (img.mtl == nil || img.mtl.device != device) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:img.width
                                                                                height:img.height
                                                                             mipmapped:NO];
    d.usage = img.isDepth ? MTLTextureUsageShaderRead : (MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView);
    d.storageMode = MTLStorageModeShared;
    img.mtl = [device newTextureWithDescriptor:d iosurface:img.surface plane:0];
    img.mtlSrgb = nil;
    if (img.mtl == nil) {
      SVERR("metal: texture over IOSurface %ux%u failed", img.width, img.height);
      return nil;
    }
    if (img.isDepth) return img.mtl;
    img.mtlSrgb = [img.mtl newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
    if (img.mtlSrgb == nil) SVWARN("metal: no sRGB view of the IOSurface texture; colours will be off");
  }
  return img.mtlSrgb != nil ? img.mtlSrgb : img.mtl;
}

// CPU fallback: the newest read-back frame into a Metal texture the composite is not reading.
void upload_cpu_screen(id<MTLDevice> device) {
  Metal& m = g.mtl;
  std::lock_guard<std::mutex> lock(g_frameMutex);
  if (g_frameSerial == g_frameUploaded || g_frameWidth == 0 || g_frameHeight == 0) return;
  if (m.cpuWidth != g_frameWidth || m.cpuHeight != g_frameHeight || m.cpuScreen[0] == nil ||
      m.cpuScreen[0].device != device) {
    for (int i = 0; i < kScreenSlots; ++i) {
      MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                   width:g_frameWidth
                                                                                  height:g_frameHeight
                                                                               mipmapped:NO];
      d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
      d.storageMode = MTLStorageModeShared;
      m.cpuScreen[i] = [device newTextureWithDescriptor:d];
      m.cpuScreenSrgb[i] = [m.cpuScreen[i] newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
      m.cpuScreenRead[i] = 0;
    }
    m.cpuWidth = g_frameWidth;
    m.cpuHeight = g_frameHeight;
    m.cpuShown = -1;
    SVLOG("screen: CPU upload path, %ux%u", g_frameWidth, g_frameHeight);
  }
  const int slot = m.cpuNext;
  // Only into a texture whose last reader has finished (else this frame is skipped, not torn).
  if (m.cpuScreenRead[slot] != 0 && g.compositeEvent != nil && g.compositeEvent.signaledValue < m.cpuScreenRead[slot])
    return;
  if (m.cpuScreen[slot] == nil) return;
  [m.cpuScreen[slot] replaceRegion:MTLRegionMake2D(0, 0, g_frameWidth, g_frameHeight)
                       mipmapLevel:0
                         withBytes:g_frame.data()
                       bytesPerRow:static_cast<NSUInteger>(g_frameWidth) * 4];
  g_frameUploaded = g_frameSerial;
  m.cpuShown = slot;
  m.cpuNext = (slot + 1) % kScreenSlots;
}

// Where the flat picture hangs this display frame (xr_quest.cpp PortXrEndFrame's quad cases).
struct ScreenPlacement {
  bool shown = false;
  V3 pos;
  float yaw = 0.0f;  // the screen's +Z (its front) turned by yaw about +Y
  float width = 0.0f, height = 0.0f;
  float radius = 0.0f;
  float uv[4] = {0, 0, 1, 1};
  bool movable = false;  // the player may carry it (not the replay drop or the Super Strike meter)
};

ScreenPlacement place_screen(uint32_t picW, uint32_t picH, const V3& head) {
  ScreenPlacement p;
  if (picW == 0 || picH == 0) return p;
  p.shown = true;
  const float aspect = static_cast<float>(picH) / static_cast<float>(picW);
  const float drop = g.fxReady ? svfx::jumbotron_drop() : 0.0f;
  const bool table = g.tabletop && !g.stereoFailed;
  auto face_head = [&](const V3& at) { return std::atan2(head.x - at.x, head.z - at.z); };
  if (table && drop > 0.0f) {
    // The replay jumbotron comes down beyond the field, facing you: from the ceiling in the room,
    // from the sky in the stadium.
    float dx = g.table.position.x - head.x, dz = g.table.position.z - head.z;
    const float len = std::max(0.1f, std::sqrt(dx * dx + dz * dz));
    dx /= len;
    dz /= len;
    p.width = g.mrMode ? 1.5f : 9.0f;
    p.height = p.width * aspect;
    V3 anchor;
    float startY, finalY;
    if (g.mrMode) {
      anchor = V3{g.table.position.x + dx * 0.6f, 0.0f, g.table.position.z + dz * 0.6f};
      const float ceiling = std::max(g.floorY + 2.3f, head.y + 0.45f);
      startY = ceiling + 0.5f * p.height;
      finalY = ceiling - 0.1f - 0.5f * p.height;
    } else {
      anchor = V3{head.x + dx * 14.0f, 0.0f, head.z + dz * 14.0f};
      startY = g.floorY + 30.0f;
      finalY = g.floorY + 3.0f + 0.5f * p.height;
    }
    anchor.y = startY + (finalY - startY) * drop;
    p.pos = anchor;
    p.yaw = face_head(anchor);
    if (drop > 0.95f && !g.dropLogged) {
      g.dropLogged = true;
      SVLOG("jumbotron down: center (%.2f, %.2f, %.2f), %.1f x %.1f m, head (%.2f, %.2f, %.2f)", anchor.x, anchor.y,
            anchor.z, p.width, p.height, head.x, head.y, head.z);
    }
    return p;
  }
  if (table) {
    g.dropLogged = false;
    p.yaw = g.table.yaw;
    if (PortXrHudOnlyNormalView() && g.sts.shown) {
      // The Super Strike meter: the square ShootToScoreMeter.cpp draws in the middle of the HUD-only
      // picture hangs over the captain, turned to you.
      const float hu = g.sts.halfU, hv = g.sts.halfV;
      p.uv[0] = 0.5f - hu;
      p.uv[1] = 0.5f - hv;
      p.uv[2] = 0.5f + hu;
      p.uv[3] = 0.5f + hv;
      p.width = std::min(0.5f, std::max(0.2f, 8.0f * g.table.scale));
      p.height = p.width * (hv * picH) / std::max(1e-3f, hu * picW);
      V3 anchor = table_point(g.sts.game[0], g.sts.game[1], g.sts.game[2] + 4.5f * character_scale());
      anchor.y += 0.5f * p.height;
      p.pos = anchor;
      p.yaw = face_head(anchor);
      if (!g.sts.logged) {
        g.sts.logged = true;
        SVLOG("super strike meter: panel (%.2f, %.2f, %.2f), %.2f x %.2f m", anchor.x, anchor.y, anchor.z, p.width,
              p.height);
      }
    } else if (PortXrHudOnlyNormalView()) {
      p.movable = true;
      // Live play draws the HUD alone: show just its score bar, a ribbon board over the field.
      p.uv[0] = 0.13f;
      p.uv[1] = 0.03f;
      p.uv[2] = 0.87f;
      p.uv[3] = 0.14f;
      p.width = 0.9f;
      p.height = p.width * (0.11f * picH) / (0.74f * picW);
      p.pos = V3{g.table.position.x, g.table.position.y + 0.4f + 0.5f * p.height, g.table.position.z};
    } else {
      p.movable = true;
      p.width = 0.75f;
      p.height = p.width * aspect;
      p.pos = V3{g.table.position.x, g.table.position.y + 0.42f + 0.5f * p.height, g.table.position.z};
    }
    if (p.movable && g.screenUser) {
      p.pos = g.screenUserPos;
      p.yaw = g.screenUserYaw;
    }
    return p;
  }
  const ScreenPreset& preset = kScreenPresets[g.set.screenPreset];
  p.pos = local(0.0f, -0.1f, -preset.distance);
  p.yaw = 0.0f;
  p.width = preset.width;
  p.height = preset.width * aspect;
  if (g.set.screenCurve > 0.01f) p.radius = preset.distance / g.set.screenCurve;
  p.movable = true;
  if (g.screenUser) {
    p.pos = g.screenUserPos;
    p.yaw = g.screenUserYaw;
  }
  return p;
}

// Nothing can be drawn (no pipelines): each drawable is still presented, every view cleared (black
// or see-through, depth far) rather than showing whatever its texture held. Without even a command
// buffer the frame is abandoned (sv_xrhost_frame_finish skips endSubmission when nothing went out).
void present_cleared(id<MTLDevice> device, int32_t count) {
  static id<MTLCommandQueue> s_fallback = nil;
  if (device != nil && (s_fallback == nil || s_fallback.device != device)) s_fallback = [device newCommandQueue];
  id<MTLCommandBuffer> cmd = [s_fallback commandBuffer];
  if (cmd == nil) {
    SVERR("cp: no command buffer to present with; the frame is abandoned");
    sv_xrhost_frame_finish();
    return;
  }
  cmd.label = @"SV clear";
  for (int32_t d = 0; d < count; ++d) {
    int32_t views = 0;
    double ptime = 0.0;
    float range[2] = {};
    sv_xrhost_drawable_info(d, &ptime, range, &views);
    for (int32_t v = 0; v < views; ++v) {
      Mat vdfv, vproj;
      void* cptr = nullptr;
      void* dptr = nullptr;
      int32_t slice = 0;
      float vp[4] = {};
      if (!sv_xrhost_frame_view(d, v, vdfv.m, vproj.m, &cptr, &dptr, &slice, vp) || cptr == nullptr) continue;
      id<MTLTexture> color = (__bridge id<MTLTexture>)cptr;
      id<MTLTexture> depth = (__bridge id<MTLTexture>)dptr;
      MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = color;
      pass.colorAttachments[0].slice = static_cast<NSUInteger>(slice);
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, g.mrMode ? 0.0 : 1.0);
      if (depth != nil) {
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.slice =
            static_cast<NSUInteger>(depth.textureType == MTLTextureType2DArray ? slice : 0);
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = 0.0;
      }
      id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
      [enc endEncoding];
    }
    sv_xrhost_frame_present(d, (__bridge void*)cmd, g.lastHeadAnchor);
  }
  [cmd commit];
  sv_xrhost_frame_finish();
}

// One display frame's pose for drawing: the eye images' pose when they are shown (the drawable then
// reprojects everything consistently), else the head predicted for this drawable.
struct DrawPose {
  Mat originFromDevice;
  int32_t anchorId = -1;
  bool fromEyes = false;
};

void fill_eye_view(svfx::EyeView& ev, const Mat& originFromView, const Mat& viewProj) {
  ev.position = svfx::V3{originFromView.m[12], originFromView.m[13], originFromView.m[14]};
  ev.right = svfx::V3{originFromView.m[0], originFromView.m[1], originFromView.m[2]};
  ev.up = svfx::V3{originFromView.m[4], originFromView.m[5], originFromView.m[6]};
  ev.back = svfx::V3{originFromView.m[8], originFromView.m[9], originFromView.m[10]};
  std::memcpy(ev.viewProj, viewProj.m, sizeof(ev.viewProj));
}

// One Compositor Services frame: everything drawn into each drawable's views, presented.
// One display frame after sv_xrhost_frame_begin returned its drawables (display thread, g_lock held).
void display_encode(int32_t count, void* devicePtr) {
  const uint64_t t1 = now_ns();
  ++g.displayFramesSinceStats;
  g.drawablesSinceStats += static_cast<uint64_t>(count);
  id<MTLDevice> device = (__bridge id<MTLDevice>)devicePtr;

  // The first drawable's views define the eye passes' frusta and sizes.
  double presentation = 0.0;
  float depthRange[2] = {0, 0};
  int32_t viewCount = 0;
  sv_xrhost_drawable_info(0, &presentation, depthRange, &viewCount);
  if (g.lastPresentation > 0.0 && presentation > g.lastPresentation) {
    const double dt = presentation - g.lastPresentation;
    if (dt < 0.1) g.displayPeriod = 0.9 * g.displayPeriod + 0.1 * dt;
  }
  g.lastPresentation = presentation;

  // Colour and depth formats from the first view's textures.
  void* colorPtr = nullptr;
  void* depthPtr = nullptr;
  int32_t slice = 0;
  float viewport[4] = {};
  Mat dfv, proj;
  bool knownNow = false;
  for (int v = 0; v < std::min(viewCount, 2); ++v) {
    if (sv_xrhost_frame_view(0, v, dfv.m, proj.m, &colorPtr, &depthPtr, &slice, viewport)) {
      g.drawableDeviceFromView[v] = dfv;
      g.drawableProjection[v] = proj;
      if (v == 0) {
        g.viewportW = viewport[2];
        g.viewportH = viewport[3];
        knownNow = true;
      }
    }
  }
  if (knownNow && viewCount >= 2 && !g.drawableKnown) {
    g.drawableKnown = true;
    SVLOG("cp: %d view(s), viewport %.0fx%.0f, depth range far %.3f near %.3f", viewCount, g.viewportW, g.viewportH,
          depthRange[0], depthRange[1]);
  }
  sv_xrhost_frame_view(0, 0, dfv.m, proj.m, &colorPtr, &depthPtr, &slice, viewport);
  id<MTLTexture> firstColor = (__bridge id<MTLTexture>)colorPtr;
  id<MTLTexture> firstDepth = (__bridge id<MTLTexture>)depthPtr;
  const MTLPixelFormat colorFormat = firstColor != nil ? firstColor.pixelFormat : MTLPixelFormatBGRA8Unorm_sRGB;
  const MTLPixelFormat depthFormat = firstDepth != nil ? firstDepth.pixelFormat : MTLPixelFormatDepth32Float;
  const bool pipelines = device != nil && build_pipelines(device, colorFormat, depthFormat);
  // Idempotent: rebuilds only when the device or the colour format changes (a reopened space).
  if (pipelines) g.fxReady = svfx::init((__bridge void*)device, static_cast<uint32_t>(colorFormat));

  id<MTLCommandBuffer> cmd = pipelines ? [g.mtl.queue commandBuffer] : nil;
  if (cmd == nil) {
    present_cleared(device, count);
    return;
  }
  cmd.label = @"SV composite";
  if (g.compositeEvent == nil) g.compositeEvent = [device newSharedEvent];  // CPU path: orders the uploads
  if (!g.gpuScreen) upload_cpu_screen(device);

  // The newest pair / picture the GPU has finished (never one it is still writing): the composite then
  // never waits on Dawn's fence.
  if (stereo_mode() && !g.stereoFailed && g.hasProjection) select_shown_eyes();
  select_shown_screen();

  // Which pose this frame is drawn with.
  const bool showEyes = stereo_mode() && !g.stereoFailed && g.hasProjection && g.shownSlot >= 0 &&
                        g.shownPose.valid && g.eyes[g.shownSlot][0].valid && g.eyes[g.shownSlot][1].valid;
  DrawPose pose;
  if (showEyes) {
    pose.originFromDevice = g.shownPose.originFromDevice;
    pose.anchorId = g.shownPose.anchorId;
    pose.fromEyes = true;
  } else {
    Mat head;
    const int32_t id = sv_xrhost_head(presentation, head.m);
    if (id >= 0) {
      g.lastHead = head;
      g.lastHeadAnchor = id;
    }
    pose.originFromDevice = g.lastHead;
    pose.anchorId = id;
  }
  if (!g.haveHead && pose.anchorId >= 0) {
    g.haveHead = true;
    g.eyeHeight = std::max(1.0f, std::min(2.1f, pose.originFromDevice.m[13] - g.floorY));
    SVLOG("head: first pose at (%.2f, %.2f, %.2f); eye height %.2f m", pose.originFromDevice.m[12],
          pose.originFromDevice.m[13], pose.originFromDevice.m[14], g.eyeHeight);
  }
  if (!g.tableSet && g.haveHead) {
    g.tableSet = true;
    g.table.position = local(0.0f, -0.55f, -0.95f);
    g.table.position.y = std::max(g.floorY + 0.6f, g.table.position.y);
  }

  // GPU order: wait for Dawn's writes of what is about to be sampled. A fence that never comes would
  // leave the command buffer (and the drawable) stuck forever, so with too many buffers in flight the
  // wait is skipped and the eye images are left out instead.
  const uint64_t outstanding = g.cmdCommitted.load() - g.cmdCompleted.load();
  const bool backlog = outstanding > 3;
  bool drawEyes = showEyes;
  if (drawEyes) {
    if (backlog) {
      drawEyes = false;
      if (g.fenceSkips++ % 90 == 0) SVWARN("cp: %llu composites in flight; eye images skipped this frame",
                                           static_cast<unsigned long long>(outstanding));
    } else {
      for (int e = 0; e < 2; ++e) {
        SharedImage& img = g.eyes[g.shownSlot][e];
        [cmd encodeWaitForEvent:img.doneEvent value:img.doneValue];
        const SharedImage& dimg = g.eyeDepths[g.shownSlot][e];
        if (g.depthExport && dimg.valid && dimg.doneEvent != nil) {
          [cmd encodeWaitForEvent:dimg.doneEvent value:dimg.doneValue];  // the same event twice is fine
        }
      }
    }
  }
  id<MTLTexture> screenTex = nil;
  uint32_t picW = 0, picH = 0;
  SharedImage* screenImg = nullptr;
  if (g.gpuScreen && g.screenShown >= 0 && g.screens[g.screenShown].valid && !backlog) {
    screenImg = &g.screens[g.screenShown];
    screenTex = metal_view(*screenImg, device);
    if (screenTex != nil) {
      [cmd encodeWaitForEvent:screenImg->doneEvent value:screenImg->doneValue];
      picW = screenImg->width;
      picH = screenImg->height;
    } else {
      screenImg = nullptr;
    }
  } else if (!g.gpuScreen && g.mtl.cpuShown >= 0) {
    const int s = g.mtl.cpuShown;
    screenTex = g.mtl.cpuScreenSrgb[s] != nil ? g.mtl.cpuScreenSrgb[s] : g.mtl.cpuScreen[s];
    picW = g.mtl.cpuWidth;
    picH = g.mtl.cpuHeight;
  }
  id<MTLTexture> eyeTex[2] = {nil, nil};
  id<MTLTexture> eyeDepthTex[2] = {nil, nil};  // the eye pass's own depth, when exported and written
  if (drawEyes) {
    for (int e = 0; e < 2; ++e) eyeTex[e] = metal_view(g.eyes[g.shownSlot][e], device);
    if (eyeTex[0] == nil || eyeTex[1] == nil) drawEyes = false;
    if (drawEyes && g.depthExport) {
      for (int e = 0; e < 2; ++e) {
        SharedImage& dimg = g.eyeDepths[g.shownSlot][e];
        if (dimg.valid) eyeDepthTex[e] = metal_view(dimg, device);
      }
    }
  }

  const V3 headPos = mat_translation(pose.originFromDevice);
  const ScreenPlacement screen = place_screen(picW, picH, headPos);
  g.lastScreenShown = screen.shown;
  g.lastScreenMovable = screen.movable;
  g.lastScreenPos = screen.pos;
  g.lastScreenYaw = screen.yaw;
  g.lastScreenWidth = screen.width;

  // The celebrations: stepped once per display frame, with this frame's eyes.
  const uint64_t fxNow = now_ns();
  svfx::Scene scene;
  scene.dt = g.lastFxNs != 0 ? std::min(0.1f, static_cast<float>(fxNow - g.lastFxNs) / 1e9f) : 0.0f;
  g.lastFxNs = fxNow;
  scene.floorY = g.floorY;
  scene.mr = g.mrMode;
  scene.tabletop = g.tabletop && !g.stereoFailed;
  scene.tablePos = svfx::V3{g.table.position.x, g.table.position.y, g.table.position.z};
  scene.tableYaw = g.table.yaw;
  scene.tableScale = g.table.scale;
  scene.charScale = character_scale();
  scene.stadiumScale = g.set.stadiumScale;
  const V3 so = stadium_origin();
  scene.stadiumOrigin = svfx::V3{so.x, so.y, so.z};
  if (screen.shown && screenTex != nil) {
    scene.screenValid = true;
    scene.screenPos = svfx::V3{screen.pos.x, screen.pos.y, screen.pos.z};
    scene.screenOrientation = svfx::Quat{0.0f, std::sin(0.5f * screen.yaw), 0.0f, std::cos(0.5f * screen.yaw)};
    scene.screenWidth = screen.width;
    scene.screenHeight = screen.height;
  }
  scene.depthFromEyes = false;
  Mat viewProjs[2] = {mat_identity(), mat_identity()};
  Mat originFromViews[2] = {mat_identity(), mat_identity()};
  for (int v = 0; v < 2; ++v) {
    const Mat& dv = pose.fromEyes ? g.shownPose.deviceFromView[v] : g.drawableDeviceFromView[v];
    const Mat& pj = pose.fromEyes ? g.shownPose.projection[v] : g.drawableProjection[v];
    originFromViews[v] = mat_mul(pose.originFromDevice, dv);
    viewProjs[v] = mat_mul(pj, mat_inverse(originFromViews[v]));
    fill_eye_view(scene.views[v], originFromViews[v], viewProjs[v]);
  }
  bool fxDraw = false;
  if (g.fxReady) {
    const uint64_t fs = now_ns();
    const bool alive = svfx::update(scene);
    fxDraw = alive && svfx::prepare(scene);
    g.fxNs += now_ns() - fs;
    if (g.celebBuzz && svfx::cannon_waves_fired() > g.buzzWave) {
      g.buzzWave = svfx::cannon_waves_fired();
      if (g.buzzWave > 1) {  // the first wave rides on the goal buzz
        g.rumble.amplitude = 1.0f;
        g.rumble.seconds = 0.22f;
        g.rumble.startPending = true;
        g.rumble.stopPending = false;
      }
    }
  }

  // Which (texture, slice) pairs were already cleared this frame: views that share one (a "shared"
  // layout) load what the first drew.
  std::vector<std::pair<void*, int32_t>> cleared;
  // STRIKERS_XR_DEPTH: what the system gets as the drawable's depth. The passes above test against
  // the real depth internally; in mode 0/1 one small pass per view/slice rewrites it afterwards (after
  // all views, since "shared" views reuse a texture slice that later views still test against).
  const int depthMode = g.set.depth;
  const bool nearFixup = depthMode == 1 && drawEyes && g.mtl.depthFixupPipeline != nil;
  struct DepthJob {
    id<MTLTexture> depth;
    int32_t slice;
    bool draw;
    float viewport[4];
    EyeUniforms u;
    int eye;
  };
  std::vector<DepthJob> depthJobs;
  std::vector<std::pair<void*, int32_t>> depthCleared;
  for (int32_t d = 0; d < count; ++d) {
    int32_t views = 0;
    double ptime = 0.0;
    float range[2] = {};
    sv_xrhost_drawable_info(d, &ptime, range, &views);
    for (int32_t v = 0; v < views; ++v) {
      Mat vdfv, vproj;
      void* cptr = nullptr;
      void* dptr = nullptr;
      int32_t vslice = 0;
      float vp[4] = {};
      if (!sv_xrhost_frame_view(d, v, vdfv.m, vproj.m, &cptr, &dptr, &vslice, vp)) continue;
      id<MTLTexture> color = (__bridge id<MTLTexture>)cptr;
      id<MTLTexture> depth = (__bridge id<MTLTexture>)dptr;
      if (color == nil) continue;
      if (depth == nil) {
        // A depth attachment of our own (the pipelines and the effects test against one).
        if (g.mtl.scratchDepth == nil || g.mtl.scratchDepth.width != color.width ||
            g.mtl.scratchDepth.height != color.height || g.mtl.scratchDepth.device != device) {
          MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depthFormat
                                                                                         width:color.width
                                                                                        height:color.height
                                                                                     mipmapped:NO];
          dd.usage = MTLTextureUsageRenderTarget;
          dd.storageMode = MTLStorageModePrivate;
          g.mtl.scratchDepth = [device newTextureWithDescriptor:dd];
        }
        depth = g.mtl.scratchDepth;
      }
      const bool ownDepth = dptr == nullptr;  // our scratch depth: the system has none to fix
      const int eye = v < 2 ? v : 1;
      const Mat ofv = mat_mul(pose.originFromDevice, pose.fromEyes && v < 2 ? g.shownPose.deviceFromView[v] : vdfv);
      const Mat vpj = pose.fromEyes && v < 2 ? g.shownPose.projection[v] : vproj;
      const Mat viewProj = mat_mul(vpj, mat_inverse(ofv));

      bool first = true;
      for (const auto& c : cleared) {
        if (c.first == cptr && c.second == vslice) first = false;
      }
      if (first) cleared.emplace_back(cptr, vslice);

      MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = color;
      pass.colorAttachments[0].slice = static_cast<NSUInteger>(vslice);
      pass.colorAttachments[0].loadAction = first ? MTLLoadActionClear : MTLLoadActionLoad;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      // Over the room only what is drawn is solid; in the stadium modes the whole view is the game's.
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, g.mrMode ? 0.0 : 1.0);
      pass.depthAttachment.texture = depth;
      pass.depthAttachment.slice = static_cast<NSUInteger>(depth.textureType == MTLTextureType2DArray ? vslice : 0);
      pass.depthAttachment.loadAction = first ? MTLLoadActionClear : MTLLoadActionLoad;
      // Stored: the system reprojects with the drawable's depth.
      pass.depthAttachment.storeAction = MTLStoreActionStore;
      pass.depthAttachment.clearDepth = 0.0;  // reverse-Z: far
      id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
      if (enc == nil) {
        SVERR("metal: render encoder for drawable %d view %d failed", d, v);
        continue;
      }
      enc.label = @"SV view";
      [enc setViewport:MTLViewport{vp[0], vp[1], vp[2], vp[3], 0.0, 1.0}];
      [enc setCullMode:MTLCullModeNone];

      EyeUniforms eyeU{};
      if (drawEyes && v < 2) {
        EyeUniforms& u = eyeU;
        std::memcpy(u.viewProj, viewProj.m, sizeof(u.viewProj));
        const Mat inv = mat_inverse(viewProj);
        std::memcpy(u.invViewProj, inv.m, sizeof(u.invViewProj));
        u.eyePos[0] = ofv.m[12];
        u.eyePos[1] = ofv.m[13];
        u.eyePos[2] = ofv.m[14];
        u.eyePos[3] = 1.0f;
        u.tableA[0] = g.table.position.x;
        u.tableA[1] = g.table.position.y;
        u.tableA[2] = g.table.position.z;
        u.tableA[3] = g.table.yaw;
        u.tableB[0] = g.table.scale;
        u.tableB[1] = g.floorY;
        u.tableB[2] = g.mrMode ? 6.0f : 60.0f;
        u.tableB[3] = g.tabletop ? 1.0f : 0.0f;
        u.mode[0] = g.mrMode ? 1.0f : 0.0f;
        u.mode[1] = static_cast<float>(depthMode);
        u.mode[3] = kNearDepthCutoff;
        u.mode[2] = eyeDepthTex[eye] != nil ? 1.0f : 0.0f;
        // The eye pass's projection (gx_projection: these near/far) and this view's clip z row, to
        // turn the exported depth into the drawable's.
        u.proj[0] = vpj.m[10];  // P22 (column 2, row 2)
        u.proj[1] = vpj.m[14];  // P23 (column 3, row 2)
        u.proj[2] = kEyeNear;
        u.proj[3] = kEyeFar;
        [enc setRenderPipelineState:g.mtl.eyePipeline];
        [enc setDepthStencilState:g.mtl.depthAlways];
        [enc setFragmentTexture:eyeTex[eye] atIndex:0];
        [enc setFragmentTexture:eyeDepthTex[eye] != nil ? eyeDepthTex[eye] : eyeTex[eye] atIndex:1];
        [enc setFragmentSamplerState:g.mtl.sampler atIndex:0];
        [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      }
      if (screen.shown && screenTex != nil) {
        ScreenUniforms u{};
        std::memcpy(u.viewProj, viewProj.m, sizeof(u.viewProj));
        const Mat model = mat_pose_yaw(screen.pos, screen.yaw);
        std::memcpy(u.model, model.m, sizeof(u.model));
        const float segs = screen.radius > 0.0f ? 64.0f : 1.0f;
        u.size[0] = screen.width;
        u.size[1] = screen.height;
        u.size[2] = screen.radius;
        u.size[3] = segs;
        std::memcpy(u.uvRect, screen.uv, sizeof(u.uvRect));
        // Always in front of the 3D picture, as Quest's quad layer was over its projection layer; its
        // depth is written, so the effects behind it are hidden.
        [enc setRenderPipelineState:g.mtl.screenPipeline];
        [enc setDepthStencilState:g.mtl.depthAlways];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:0];
        [enc setFragmentTexture:screenTex atIndex:0];
        [enc setFragmentSamplerState:g.mtl.sampler atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
                vertexStart:0
                vertexCount:static_cast<NSUInteger>((segs + 1.0f) * 2.0f)];
      }
      if (fxDraw && v < 2) svfx::encode((__bridge void*)enc, scene, eye);
      [enc endEncoding];
      if (depthMode != 2 && !ownDepth) {
        DepthJob job{};
        job.depth = depth;
        job.slice = static_cast<int32_t>(depth.textureType == MTLTextureType2DArray ? vslice : 0);
        job.eye = eye;
        std::memcpy(job.viewport, vp, sizeof(job.viewport));
        job.draw = nearFixup && drawEyes && v < 2;
        if (job.draw) job.u = eyeU;
        depthJobs.push_back(job);
      }
    }
  }

  // The system's depth (mode 0: far everywhere; mode 1: the real depth only where nearer than the
  // cutoff): a depth-only pass over each view's drawable depth, cleared to 0 (reverse-Z far) the first
  // time a (texture, slice) is met. Mode 2 leaves the real depth as the passes wrote it.
  for (const DepthJob& job : depthJobs) {
    bool first = true;
    for (const auto& c : depthCleared) {
      if (c.first == (__bridge void*)job.depth && c.second == job.slice) first = false;
    }
    if (first) depthCleared.emplace_back((__bridge void*)job.depth, job.slice);
    if (!first && !job.draw) continue;  // already far, nothing to add
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture = job.depth;
    pass.depthAttachment.slice = static_cast<NSUInteger>(job.slice);
    pass.depthAttachment.loadAction = first ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 0.0;
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
    if (enc == nil) continue;
    enc.label = @"SV depth";
    if (job.draw) {
      EyeUniforms u = job.u;
      [enc setViewport:MTLViewport{job.viewport[0], job.viewport[1], job.viewport[2], job.viewport[3], 0.0, 1.0}];
      [enc setCullMode:MTLCullModeNone];
      [enc setRenderPipelineState:g.mtl.depthFixupPipeline];
      [enc setDepthStencilState:g.mtl.depthAlways];
      [enc setFragmentTexture:eyeTex[job.eye] atIndex:0];
      [enc setFragmentTexture:eyeDepthTex[job.eye] != nil ? eyeDepthTex[job.eye] : eyeTex[job.eye] atIndex:1];
      [enc setFragmentSamplerState:g.mtl.sampler atIndex:0];
      [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
      [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [enc endEncoding];
  }

  // Dawn may write these images again once this command buffer is done with them.
  uint64_t signalValue = 0;
  if (g.compositeEvent != nil) {
    const uint64_t value = ++g.compositeValue;
    signalValue = value;
    [cmd encodeSignalEvent:g.compositeEvent value:value];
    if (drawEyes) {
      g.eyes[g.shownSlot][0].readValue = value;
      g.eyes[g.shownSlot][1].readValue = value;
      for (int e = 0; e < 2; ++e) {
        if (eyeDepthTex[e] != nil) g.eyeDepths[g.shownSlot][e].readValue = value;
      }
    }
    if (screenImg != nullptr) screenImg->readValue = value;
    if (!g.gpuScreen && g.mtl.cpuShown >= 0) g.mtl.cpuScreenRead[g.mtl.cpuShown] = value;
  }
  for (int32_t d = 0; d < count; ++d) sv_xrhost_frame_present(d, (__bridge void*)cmd, pose.anchorId);
  if (fxDraw) svfx::frame_committed((__bridge void*)cmd);
  g.cmdCommitted.fetch_add(1);
  id<MTLSharedEvent> signalEvent = g.compositeEvent;
  const double presentationTime = presentation;
  [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
    g.cmdCompleted.fetch_add(1);
    // Late: the composite's GPU work ended after the time the drawable was due on screen (both in
    // CACurrentMediaTime seconds).
    if (presentationTime > 0.0) {
      const double endTime = done.GPUEndTime > 0.0 ? done.GPUEndTime : CACurrentMediaTime();
      g_presentCount.fetch_add(1);
      if (endTime > presentationTime) {
        g_lateCount.fetch_add(1);
        note_max(g_lateMaxUs, static_cast<uint64_t>((endTime - presentationTime) * 1e6));
      }
    }
    if (done.GPUEndTime > done.GPUStartTime) {
      const uint64_t us = static_cast<uint64_t>((done.GPUEndTime - done.GPUStartTime) * 1e6);
      g_compGpuUs.fetch_add(us);
      g_compGpuCount.fetch_add(1);
      note_max(g_compGpuMaxUs, us);
    }
    // A failed command buffer never signals: Dawn's next BeginAccess (and the CPU upload check)
    // would then wait for this value forever. Signalled from the CPU instead (never lowered).
    if (signalEvent != nil && signalValue != 0 && signalEvent.signaledValue < signalValue) {
      signalEvent.signaledValue = signalValue;
    }
    if (done.status == MTLCommandBufferStatusError) {
      static std::atomic<int> s_errors{0};
      if (s_errors.fetch_add(1) < 5) {
        SVERR("metal: composite command buffer failed: %s", done.error.localizedDescription.UTF8String);
      }
    }
  }];
  [cmd commit];
  sv_xrhost_frame_finish();
  g.compositeNs += now_ns() - t1;
}

// The display thread: a Compositor Services frame at the display's rate, whatever the game thread
// is doing. The blocking part (queryNextFrame + the wait until the optimal input time, inside
// sv_xrhost_frame_begin) runs without the lock; the composite is encoded under it.
void display_thread_main() {
  pthread_setname_np("sv-display");
  t_isDisplayThread = true;
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  while (!g_displayQuit.load()) {
    void* renderer = nullptr;
    {
      Lock lock(g_lock);
      if (g.active && g.renderer != nullptr && g.rendererRunning && !sv_game_paused()) {
        renderer = g.renderer;
        CFRetain(renderer);
      }
    }
    if (renderer == nullptr) {
      usleep(5000);
      continue;
    }
    void* devicePtr = nullptr;
    const uint64_t t0 = now_ns();
    const int32_t count = sv_xrhost_frame_begin(renderer, &devicePtr);
    const uint64_t t1 = now_ns();
    {
      Lock lock(g_lock);
      const uint64_t waited = now_ns() - t1;
      g.lockWaitNs += waited;
      g.lockWaitMaxNs = std::max(g.lockWaitMaxNs, waited);
      g.beginWaitNs += t1 - t0;
      if (count <= 0) {
        ++g.noFrameSinceStats;
      } else if (g.renderer == renderer && g.active) {
        display_encode(count, devicePtr);
      } else {
        // The renderer went while this frame was being waited for: it is still presented (cleared).
        present_cleared((__bridge id<MTLDevice>)devicePtr, count);
      }
    }
    CFRelease(renderer);
    // No frame to draw this time: a short sleep instead of spinning (the next query paces again).
    if (count <= 0) usleep(2000);
  }
}

void start_display_thread() {
  if (g_displayThread.joinable()) return;
  g_displayQuit.store(false);
  g_displayThread = std::thread(display_thread_main);
}

void stop_display_thread() {
  if (!g_displayThread.joinable()) return;
  g_displayQuit.store(true);
  g_displayThread.join();
}

// ---------------------------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------------------------

void log_stats() {
  const uint64_t t = now_ns();
  if (g.lastStatsNs == 0) {
    g.lastStatsNs = t;
    return;
  }
  if (t - g.lastStatsNs < static_cast<uint64_t>(g.set.statsSeconds) * 1000000000ull) return;
  const double secs = static_cast<double>(t - g.lastStatsNs) / 1e9;
  unsigned long buffers = 0, underruns = 0;
  int everNonSilent = 0;
  PortAudioStats(&buffers, &underruns, &everNonSilent);
  uint32_t poolPeak[4] = {}, poolSize[4] = {};
  aurora_gfx_pool_stats(poolPeak, poolSize, 4);
  const auto pct = [&](int i) { return poolSize[i] ? 100.0 * poolPeak[i] / poolSize[i] : 0.0; };
  const double gf = g.gameFramesSinceStats > 0 ? static_cast<double>(g.gameFramesSinceStats) : 1.0;
  const double df = g.displayFramesSinceStats > 0 ? static_cast<double>(g.displayFramesSinceStats) : 1.0;
  SVLOG("stats: display %.1f fps, game %.1f fps, no-frame %llu, drawables %.2f/frame, audio underruns +%lu, %s, 3D "
        "%s, screen %d (%s), rumble %llu, pools v%.0f%% u%.0f%% i%.0f%% s%.0f%%, layers dropped %llu, culled "
        "%.0f/frame, in flight %llu, depth %d",
        g.displayFramesSinceStats / secs, g.gameFramesSinceStats / secs,
        static_cast<unsigned long long>(g.noFrameSinceStats), g.drawablesSinceStats / df, underruns - g.lastUnderruns,
        mode_name(), g.stereoFailed ? "failed" : (g.hasProjection ? "on" : "off"), g.set.screenPreset,
        g.gpuScreen ? "gpu" : "cpu", static_cast<unsigned long long>(g.rumblePulses), pct(0), pct(1), pct(2), pct(3),
        static_cast<unsigned long long>(g.layersDroppedSinceStats),
        static_cast<double>(PortXrLayersTakeCulled()) / gf,
        static_cast<unsigned long long>(g.cmdCommitted.load() - g.cmdCompleted.load()), g.set.depth);
  uint32_t fxParts = 0, fxIndices = 0;
  if (g.fxReady && svfx::take_stats(&fxParts, &fxIndices)) {
    SVLOG("fx: up to %u pieces, %u mesh indices per eye", fxParts, fxIndices);
  }
  if (g.gameFramesSinceStats > 0) {
    const double n = gf * 1e6;
    SVLOG("loop per game frame (ms): PortXrBeginFrame %.2f, aurora_begin_frame %.2f, tasks %.2f, aurora_end_frame "
          "%.2f, PortXrEndFrame %.2f | eye-send %.2f, sync+copy %.2f",
          g.loopNs[0] / n, g.loopNs[1] / n, g.loopNs[2] / n, g.loopNs[3] / n, g.loopNs[4] / n, g.eyeSendNs / n,
          g.endSyncNs / n);
    uint64_t w[8] = {};
    aurora_xr_take_waits(w, 8);
    SVLOG("waits per game frame (ms): fifo-drain %.2f, queue-full %.2f, sync %.2f, frame-slot %.2f, staging %.2f, "
          "pipeline %.2f | busy: fifo %.2f, render %.2f",
          w[0] / n, w[1] / n, w[2] / n, w[3] / n, w[4] / n, w[5] / n, w[6] / n, w[7] / n);
    uint32_t d[6] = {};
    aurora_xr_take_decode_stats(d);
    SVLOG("decode per game frame: %.1f draws decoded, %.1f reused, %.0f vertices, %.2f ms CPU | %.1f waiting for "
          "their pipeline, %.1f not eligible",
          d[0] / gf, d[1] / gf, d[2] / gf, d[5] / 1000.0 / gf, d[3] / gf, d[4] / gf);
  }
  const double dn = df * 1e6;
  SVLOG("display per frame (ms): wait for input time %.2f, composite %.2f, fx %.2f", g.beginWaitNs / dn,
        g.compositeNs / dn, g.fxNs / dn);
  {
    const uint64_t en = g_eyeGpuCount.exchange(0), eu = g_eyeGpuUs.exchange(0), em = g_eyeGpuMaxUs.exchange(0);
    const uint64_t cn = g_compGpuCount.exchange(0), cu = g_compGpuUs.exchange(0), cm = g_compGpuMaxUs.exchange(0);
    // Eye pair: the frame's whole Dawn queue (the flat picture included) until the pair is done. Over
    // 16.7 ms the GPU cannot hold 60 game frames.
    SVLOG("gpu (ms): eye pair %.2f avg %.2f max (%llu), composite %.2f avg %.2f max, display period %.2f",
          en ? eu / 1000.0 / en : 0.0, em / 1000.0, static_cast<unsigned long long>(en), cn ? cu / 1000.0 / cn : 0.0,
          cm / 1000.0, g.displayPeriod * 1000.0);
  }
  {
    const uint64_t pc = g_presentCount.exchange(0), lc = g_lateCount.exchange(0), lm = g_lateMaxUs.exchange(0);
    const uint64_t hold = g_gameLockHoldMaxUs.exchange(0);
    SVLOG("present: late %llu/%llu (worst %.1f ms), newest pair not ready %llu/%llu, frames per game image 1:%llu "
          "2:%llu 3+:%llu, lock wait display %.2f avg %.2f max ms, game hold max %.2f ms, depth mode %d (%s)",
          static_cast<unsigned long long>(lc), static_cast<unsigned long long>(pc), lm / 1000.0,
          static_cast<unsigned long long>(g.eyeNotReadySinceStats), static_cast<unsigned long long>(g.eyeChecksSinceStats),
          static_cast<unsigned long long>(g.framesPerImage[0]), static_cast<unsigned long long>(g.framesPerImage[1]),
          static_cast<unsigned long long>(g.framesPerImage[2]), g.lockWaitNs / dn, g.lockWaitMaxNs / 1e6, hold / 1000.0,
          g.set.depth, g.set.depth == 0 ? "off" : (g.set.depth == 1 ? "near" : "full"));
  }
  g.eyeChecksSinceStats = g.eyeNotReadySinceStats = 0;
  g.framesPerImage[0] = g.framesPerImage[1] = g.framesPerImage[2] = 0;
  g.lockWaitNs = g.lockWaitMaxNs = 0;
  g.rumblePulses = 0;
  g.layersDroppedSinceStats = 0;
  std::memset(g.loopNs, 0, sizeof(g.loopNs));
  g.beginWaitNs = g.endSyncNs = g.compositeNs = g.eyeSendNs = g.fxNs = 0;
  g.lastUnderruns = underruns;
  g.lastStatsNs = t;
  g.displayFramesSinceStats = g.gameFramesSinceStats = g.noFrameSinceStats = g.drawablesSinceStats = 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// include/port/xr_quest.h
// ---------------------------------------------------------------------------------------------

extern "C" int PortXrWanted(void) {
  const char* off = std::getenv("STRIKERS_XR");
  return (off != nullptr && off[0] == '0') ? 0 : 1;
}

extern "C" int PortXrInit(void) {
  Lock lock(g_lock);
  if (g.active) return 1;
  if (!PortXrWanted()) {
    SVLOG("STRIKERS_XR=0: the headset layer is off");
    return 0;
  }
  SVLOG("%s starting", kBuildTag);
  sv_xrhost_start(&sv_log_line);
  read_settings();
  if (g.set.tableScale != 1.0f) apply_table_scale_setting();
  apply_aurora_settings();
  apply_view_mode(sv_env_int("STRIKERS_XR_START_VIEW", SV_VIEW_JUMBOTRON_ROOM, 0, 3));
  report_mode();
  aurora_xr_set_surface_suppressed(true);
  // The flat picture: copied on the GPU into an IOSurface when Dawn can share one with Metal; else
  // Aurora reads every frame back (as on Quest) and it is uploaded.
  g.gpuScreen = aurora::webgpu::g_metalSharedImageSupported && ensure_composite_event();
  if (!g.gpuScreen) {
    aurora_xr_set_frame_callback(&on_aurora_frame, nullptr);
    SVWARN("screen: Dawn cannot share IOSurfaces with Metal here (shared image %d); CPU readback",
           aurora::webgpu::g_metalSharedImageSupported ? 1 : 0);
  }
  g.active = true;
  start_display_thread();
  SVLOG("%s ready: %s, screen path %s, waiting for the immersive space", kBuildTag, mode_name(),
        g.gpuScreen ? "GPU" : "CPU");
  return 1;
}

extern "C" int PortXrActive(void) { return g.active ? 1 : 0; }

extern "C" int PortXrShouldQuit(void) { return (g.quit || sv_game_quit_requested()) ? 1 : 0; }

// Per-draw queries from the game thread: lock-free on purpose. They read only fields the game
// thread itself writes (the view mode, the settings, stereoFailed, gameplayLive, hasProjection); the
// display thread only reads them.
extern "C" int PortXrStereoWorld(void) { return (g.active && stereo_mode() && !g.stereoFailed) ? 1 : 0; }

extern "C" int PortXrStereoFullStands(void) { return (PortXrStereoWorld() && g.tabletop && !g.mrMode) ? 1 : 0; }

extern "C" int PortXrStadiumLayerActive(void) { return (PortXrStereoWorld() && !g.mrMode) ? 1 : 0; }

extern "C" int PortXrEyeViewAllowed(int view, int tableLayer) {
  (void)tableLayer;
  return (view >= 0 && view < 32) ? 1 : 0;
}

extern "C" int PortXrStereoFrameBegin(void) {
  Lock lock(g_lock);
  if (!g.active || !g.frameBegun || !g.viewsValid || !stereo_mode() || g.stereoFailed || !g.stereoReady) return 0;
  // Online, a headset that cannot hold 60 game frames would slow the shared match: its 3D view is then
  // redrawn every other game frame (the system reprojects the last pair).
  if (PortNetOnline() && g.onlineHalfRate && (g.onlineParity++ & 1) != 0) return 0;
  if (g.poolReserved[0] == 0) aurora_gfx_pool_stats(nullptr, g.poolReserved, kPools);
  {
    const float lastFrame = static_cast<float>(aurora_xr_decoded_bytes_last_frame());
    g.decodedPerFrame = lastFrame > g.decodedPerFrame ? lastFrame : g.decodedPerFrame * 0.99f + lastFrame * 0.01f;
  }
  uint32_t used[kPools];
  aurora_gfx_pool_usage(used, kPools);
  for (int k = 0; k < 2; ++k) {
    if (g.perSend[k][0] == 0.0f) {
      for (int p = 0; p < kPools; ++p) g.perSend[k][p] = 0.5f * static_cast<float>(used[p]);
    }
  }
  int kinds[2] = {0, 0};
  int count = layer_count();
  for (int l = 0; l < count; ++l) kinds[l] = layer_kind(l);
  if (!layers_fit(used, kinds, count) && count == 2) {
    for (int p = 0; p < kPools; ++p) g.perSend[kinds[0]][p] *= 0.98f;
    kinds[0] = kinds[1];
    count = 1;
    ++g.layersDroppedSinceStats;
  }
  if (!layers_fit(used, kinds, count)) {
    g.layersDroppedSinceStats += static_cast<uint64_t>(count);
    return 0;
  }
  g.frameLayerKind[0] = kinds[0];
  g.frameLayerKind[1] = kinds[1];
  // Never into the pair on show; handed-back pairs the display never got to are overwritten oldest first.
  pick_eye_slot();
  for (int e = 0; e < 2; ++e) {
    SharedImage& img = g.eyes[g.eyeSlot][e];
    begin_access(img);
    g.eyePassOpen[e] = false;
  }
  if (g.depthExport) {
    for (int e = 0; e < 2 && g.depthExport; ++e) {
      if (!begin_access(g.eyeDepths[g.eyeSlot][e])) stop_depth_export("Dawn refused the access");
    }
  }
  if (!g.eyes[g.eyeSlot][0].accessOpen || !g.eyes[g.eyeSlot][1].accessOpen) {
    for (int e = 0; e < 2; ++e) end_access(g.eyes[g.eyeSlot][e]);
    end_depth_access(g.eyeSlot);
    // Dawn refused the shared images: the 3D modes go, the Jumbotron stays.
    g.stereoFailed = true;
    g.hasProjection = false;
    SVERR("3D modes off: Dawn refused access to the eye images");
    report_mode();
    return 0;
  }
  g.stereoLayers = count;
  g.stereoThisFrame = true;
  g.eyeSendStartNs = now_ns();
  return g.stereoLayers;
}

extern "C" int PortXrStereoEyeLayer(int eye, int layer, float view[16], float proj[16], int* flags) {
  Lock lock(g_lock);
  if (!g.stereoThisFrame || eye < 0 || eye > 1) return 0;
  SharedImage& img = g.eyes[g.eyeSlot][eye];
  if (layer == 0) {
    if (!img.accessOpen) return 0;
    aurora::gfx::ExternalPassTarget target;
    target.texture = img.texture;
    target.view = img.view;
    target.format = aurora::gfx::color_format();
    target.width = img.width;
    target.height = img.height;
    const SharedImage& dimg = g.eyeDepths[g.eyeSlot][eye];
    if (g.depthExport && dimg.accessOpen && dimg.view) {
      target.depthSnapshot = dimg.view;
      target.depthSnapshotPacked = true;
    }
    const bool stadiumLayer =
        g.frameLayerKind[0] == kLayerStadium || (g.stereoLayers > 1 && g.frameLayerKind[1] == kLayerStadium);
    target.sampleCount = static_cast<uint32_t>(stadiumLayer ? g.set.msaaStadium : g.set.msaaTable);
    aurora::gfx::set_offscreen_uses_native_logical_size(true);
    if (!aurora::gfx::create_pass_external(target)) {
      aurora::gfx::set_offscreen_uses_native_logical_size(false);
      return 0;
    }
    g.eyePassOpen[eye] = true;
    // The FIFO is drained here: safe to switch Aurora's coverage mode (solid draws write alpha 1, so
    // the field is solid over the room).
    aurora_gx_set_xr_coverage(g.mrMode);
  }
  if (!g.eyePassOpen[eye] || layer < 0 || layer > 1) return 0;
  close_pool_measurement();
  const LayerKind kind = static_cast<LayerKind>(g.frameLayerKind[layer]);
  {
    uint32_t used[kPools];
    aurora_gfx_pool_usage(used, kPools);
    for (int p = 0; p < kPools; ++p) {
      if (g.poolReserved[p] != 0 && used[p] + g.perSend[kind][p] * kPoolMargin > g.poolReserved[p] * 0.98) {
        ++g.layersDroppedSinceStats;
        return 0;
      }
    }
    g.measuring = true;
    g.measuringKind = kind;
    std::memcpy(g.measureStart, used, sizeof(used));
    g.decodedAtMeasure = aurora_xr_decoded_bytes();
  }
  const M4 model = kind == kLayerTable ? table_model() : stadium_model();
  const Mat originFromView = mat_mul(g.predicted.originFromDevice, g.predicted.deviceFromView[eye]);
  const M4 v = m4_mul(model, m4_from_mat(mat_inverse(originFromView)));
  std::memcpy(view, v.m, sizeof(v.m));
  // One projection for every layer of an eye, so their depths compare.
  gx_projection(tangents_from_projection(g.predicted.projection[eye]), kEyeNear, kEyeFar, proj);
  *flags = kind == kLayerTable ? (kFlagSkipSky | kFlagTable) : kFlagSkipActors;
  return 1;
}

extern "C" void PortXrStereoEyeEnd(int eye) {
  Lock lock(g_lock);
  close_pool_measurement();
  if (eye < 0 || eye > 1 || !g.eyePassOpen[eye]) return;
  aurora::gfx::ResolvedTargets out;
  aurora::gfx::ResolveDesc desc;
  desc.color = false;
  const SharedImage& dimg = g.eyeDepths[g.eyeSlot][eye];
  desc.depth = g.depthExport && dimg.accessOpen && dimg.view;
  aurora::gfx::resolve_pass(desc, out);
  if (desc.depth && !out.depth) stop_depth_export("Aurora has no depth snapshot on this device");
  aurora_gx_set_xr_coverage(false);
  aurora::gfx::set_offscreen_uses_native_logical_size(false);
  g.eyePassOpen[eye] = false;
}

extern "C" void PortXrStereoFrameEnd(void) {
  Lock lock(g_lock);
  if (g.eyeSendStartNs != 0) {
    g.eyeSendNs += now_ns() - g.eyeSendStartNs;
    g.eyeSendStartNs = 0;
  }
}

extern "C" void PortXrNoteLoopTimes(int ranGameFrame, unsigned long long xrBegin, unsigned long long auroraBegin,
                                    unsigned long long tasks, unsigned long long auroraEnd, unsigned long long xrEnd) {
  Lock lock(g_lock);
  if (!g.active || !ranGameFrame) return;
  g.loopNs[0] += xrBegin;
  g.loopNs[1] += auroraBegin;
  g.loopNs[2] += tasks;
  g.loopNs[3] += auroraEnd;
  g.loopNs[4] += xrEnd;
}

extern "C" void PortXrGoal(int team, int character, int superStrike, int hyper, int ownGoal, int humanPad) {
  Lock lock(g_lock);
  if (!g.active) return;
  static const char* kNames[] = {"Birdo", "Daisy", "Donkey Kong", "Hammer Bro", "Koopa", "Luigi",
                                 "Mario", "Peach", "Toad",        "Waluigi",    "Wario", "Yoshi"};
  const char* who = (character >= 0 && character < 12) ? kNames[character] : "?";
  SVLOG("celebration: GOAL team %d by %s (class %d)%s%s%s, scorer pad %d", team, who, character,
        superStrike ? " SUPER STRIKE" : "", hyper ? " perfect" : "", ownGoal ? " own goal" : "", humanPad);
  svfx::goal(team, character, superStrike != 0, hyper != 0, ownGoal != 0);
  // The scorer's hands feel it: a long, strong buzz when the goal is yours, then a thump with every
  // cannon wave (display_frame).
  g.celebBuzz = humanPad == xrnet::local_pad() && !ownGoal;
  g.buzzWave = 0;
  if (g.celebBuzz) {
    g.rumble.amplitude = 1.0f;
    g.rumble.seconds = superStrike ? 0.9f : 0.5f;
    g.rumble.startPending = true;
    g.rumble.stopPending = false;
  }
}

extern "C" void PortXrCelebrationPhase(int phase) {
  Lock lock(g_lock);
  if (!g.active) return;
  static const char* kPhases[] = {"?", "replay started", "replay played out", "skipped", "play live"};
  SVLOG("celebration: %s", (phase >= 1 && phase <= 4) ? kPhases[phase] : "?");
  svfx::phase(phase);
}

extern "C" void PortXrSetGameplayLive(int live) {
  Lock lock(g_lock);
  g.gameplayLive = live != 0;
}

extern "C" void PortXrSetPlayerMarker(int shown, float x, float y, float z, int teamCaptain) {
  Lock lock(g_lock);
  if (!g.active) return;
  svfx::set_marker(shown != 0 && g.tabletop && !g.stereoFailed, x, y, z, teamCaptain);
}

// The game's bright full-screen flashes light up your whole view instead of only the game picture;
// the dark fades stay on the picture.
extern "C" int PortXrScreenFlash(float r, float gr, float b, float a) {
  Lock lock(g_lock);
  if (!g.active || !g.fxReady || std::max(r, std::max(gr, b)) < 0.5f) return 0;
  svfx::screen_flash(r, gr, b, a);
  return 1;
}

extern "C" int PortXrTakeTestWipe(void) { return 0; }
extern "C" int PortXrTakeTestItem(void) { return 0; }
extern "C" int PortXrTakeTestSts(void) { return 0; }

extern "C" void PortXrSetStsMeter(int shown, float x, float y, float z, float halfU, float halfV) {
  Lock lock(g_lock);
  g.sts.shown = shown != 0;
  if (!g.sts.shown) {
    g.sts.logged = false;
    return;
  }
  g.sts.game[0] = x;
  g.sts.game[1] = y;
  g.sts.game[2] = z;
  g.sts.halfU = std::min(0.5f, std::max(0.01f, halfU));
  g.sts.halfV = std::min(0.5f, std::max(0.01f, halfV));
}

extern "C" void PortXrNoteItemUse(int current, int next, int blocked) {
  Lock lock(g_lock);
  SVLOG("item: use, current %d, next %d%s", current, next, blocked ? " (blocked)" : "");
}

extern "C" void PortXrLog(const char* msg) { SVLOG("%s", msg != nullptr ? msg : ""); }

extern "C" float PortXrCharacterScale(void) { return character_scale(); }

extern "C" int PortXrHudOnlyNormalView(void) {
  return (g.set.hudOnly && g.active && g.tabletop && stereo_mode() && !g.stereoFailed && g.gameplayLive &&
          g.hasProjection)
             ? 1
             : 0;
}

extern "C" void PortXrRumblePreset(int pad, int preset) {
  Lock lock(g_lock);
  if (!g.active || pad != xrnet::local_pad()) return;
  // eRumbleActionPreset: small, medium, solid contact, shot contact, Super Strike (hyper), Super Strike.
  constexpr float kAmplitude[] = {0.35f, 0.55f, 0.8f, 0.65f, 1.0f, 0.9f};
  if (preset >= 0 && preset < static_cast<int>(sizeof(kAmplitude) / sizeof(kAmplitude[0]))) {
    g.rumble.amplitude = kAmplitude[preset];
  }
}

extern "C" void PortXrRumbleStart(int pad, float seconds) {
  Lock lock(g_lock);
  if (!g.active || pad != xrnet::local_pad()) return;
  g.rumble.startPending = true;
  g.rumble.stopPending = false;
  g.rumble.seconds = seconds;
}

extern "C" void PortXrRumbleStop(int pad) {
  Lock lock(g_lock);
  if (!g.active || pad != xrnet::local_pad()) return;
  if (!g.rumble.startPending) g.rumble.stopPending = true;
}

extern "C" int PortXrBeginFrame(void) {
  Lock lock(g_lock);
  if (!g.active) return 0;
  g.frameBegun = false;
  // The game's boot (disc, memory card, first scene setup) is behind it: the app may open the space.
  sv_game_ready();
  take_settings();
  const int vm = sv_game_take_view_mode();
  if (vm >= 0) apply_view_mode(vm);
  if (sv_game_quit_requested()) {
    g.quit = true;
    return 0;
  }
  // Paused (app in the background) or no running renderer: no game frames, no display frames.
  if (sv_game_paused() || !ensure_renderer()) {
    g.lastTickNs = 0;  // no catch-up burst afterwards
    lock.unlock();
    usleep(10000);
    return 0;
  }
  g.frameBegun = true;

  xrnet::poll();
  // Controller poses for when this frame is shown (or now, before the first display frame).
  sync_input(g.lastPresentation > 0.0 ? g.lastPresentation + g.displayPeriod : CACurrentMediaTime());
  take_remote_table_pose();
  PortXrSetStadiumEconomy(g.set.lodMinPx, 0.5f * static_cast<float>(g.eyeWidth ? g.eyeWidth : 1800),
                          g.set.skipTallField ? 1 : 0, (g.tabletop && !g.mrMode && !g.stereoFailed) ? 1 : 0);

  if (stereo_mode()) {
    // Made here, between game frames (the render worker is idle after last frame's synchronize).
    create_stereo_resources();
    predict_eyes();
  } else {
    g.viewsValid = false;
    g.hasProjection = false;
  }

  // The table comes to you: 0.9 m ahead of where you look, at about waist height, its long side
  // across your view (left stick tap).
  if (g.bringTable && g.viewsValid) {
    g.bringTable = false;
    const Mat& h = g.predicted.originFromDevice;
    float fx = -h.m[8], fz = -h.m[10];
    const float len = std::max(1e-3f, std::sqrt(fx * fx + fz * fz));
    fx /= len;
    fz /= len;
    const V3 head = mat_translation(h);
    g.table.position = V3{head.x + fx * 0.9f, std::max(g.floorY + 0.75f, head.y - 0.55f), head.z + fz * 0.9f};
    g.table.yaw = std::atan2(-fx, -fz);
    g.tableSet = true;
    SVLOG("table brought to (%.2f, %.2f, %.2f) yaw %.2f", g.table.position.x, g.table.position.y, g.table.position.z,
          g.table.yaw);
    send_table_pose(true);
  }

  // Real time since the last iteration decides whether a 60 Hz game frame is due.
  const uint64_t tickNow = now_ns();
  if (g.lastTickNs != 0) {
    g.tickAccumNs += static_cast<int64_t>(tickNow - g.lastTickNs);
    g.loopIntervalEma = 0.9 * g.loopIntervalEma + 0.1 * static_cast<double>(tickNow - g.lastTickNs);
    if (!PortNetOnline()) {
      g.onlineHalfRate = false;
    } else if (!g.onlineHalfRate && stereo_mode() && g.loopIntervalEma > 15.5e6) {
      g.onlineHalfRate = true;
      SVLOG("net: this headset is too slow for 60 online; its 3D view now updates every other frame");
    }
  } else {
    g.tickAccumNs += kGameTickNs;
  }
  g.lastTickNs = tickNow;
  if (g.tickAccumNs > 4 * kGameTickNs) g.tickAccumNs = kGameTickNs;  // after a stall: one frame, no burst
  const bool due = g.tickAccumNs >= kGameTickNs;

  // Online: a game frame runs only once both players' input for it is here (xr_net.cpp).
  if (xrnet::lockstep()) {
    if (!due) return 0;
    xrnet::NetPad local;
    local.buttons = g.localPad.button;
    local.stickX = g.localPad.stickX;
    local.stickY = g.localPad.stickY;
    local.substickX = g.localPad.substickX;
    local.substickY = g.localPad.substickY;
    local.triggerLeft = g.localPad.triggerLeft;
    local.triggerRight = g.localPad.triggerRight;
    xrnet::NetPad pad0, pad1;
    if (!xrnet::step(local, &pad0, &pad1)) {
      g.tickAccumNs = std::min<int64_t>(g.tickAccumNs, kGameTickNs);
      return 0;
    }
    g.tickAccumNs -= kGameTickNs;
    auto from_net = [](const xrnet::NetPad& p) {
      SVPad s{};
      s.button = p.buttons;
      s.stickX = p.stickX;
      s.stickY = p.stickY;
      s.substickX = p.substickX;
      s.substickY = p.substickY;
      s.triggerLeft = p.triggerLeft;
      s.triggerRight = p.triggerRight;
      return s;
    };
    const SVPad s0 = from_net(pad0), s1 = from_net(pad1);
    sv_pad_set_virtual(0, &s0);
    sv_pad_set_virtual(1, &s1);
    g.port1Active = true;
    return 1;
  }
  // Offline: this headset is pad 0, every headset frame (keeps it connected without a paired pad).
  sv_pad_set_virtual(0, &g.localPad);
  if (g.port1Active) {
    sv_pad_clear_virtual(1);
    g.port1Active = false;
  }
  if (due) {
    g.tickAccumNs -= kGameTickNs;
    return 1;
  }
  return 0;
}

extern "C" void PortXrEndFrame(int ranGameFrame) {
  // g.active, g.frameBegun and g.stereoThisFrame are the game thread's own; the wait for Aurora's
  // render worker (synchronize) runs without the lock so the display thread keeps compositing.
  if (!g.active || !g.frameBegun) return;
  g.frameBegun = false;
  if (ranGameFrame) {
    const uint64_t t0 = now_ns();
    aurora::gfx::synchronize();
    Lock lock(g_lock);
    if (g.gpuScreen) copy_screen_gpu();
    if (g.stereoThisFrame) {
      g.stereoThisFrame = false;
      finish_stereo_frame();
    }
    g.endSyncNs += now_ns() - t0;
    ++g.gameFramesSinceStats;
  } else if (g.stereoThisFrame) {
    // The eye sends ran but the frame was refused: give the images back to Dawn's side unchanged.
    g.stereoThisFrame = false;
    aurora::gfx::synchronize();
    Lock lock(g_lock);
    for (int e = 0; e < 2; ++e) end_access(g.eyes[g.eyeSlot][e]);
    end_depth_access(g.eyeSlot);
  }
  Lock lock(g_lock);
  log_stats();
}

extern "C" void PortXrShutdown(void) {
  if (!g.active) return;
  SVLOG("shutting down the headset layer");
  stop_display_thread();  // joined without the lock: its last frame may be waiting for it
  Lock lock(g_lock);
  aurora_xr_set_frame_callback(nullptr, nullptr);
  sv_xrhost_rumble(0.0f, 0.0f);
  // Every composite in flight finishes before the images go.
  for (int i = 0; i < 200 && g.cmdCompleted.load() < g.cmdCommitted.load(); ++i) usleep(5000);
  svfx::shutdown();
  for (auto& slot : g.eyes)
    for (SharedImage& img : slot) {
      if (img.accessOpen) end_access(img);
      release_shared(img);
    }
  for (auto& slot : g.eyeDepths)
    for (SharedImage& img : slot) {
      if (img.accessOpen) end_access(img);
      release_shared(img);
    }
  g.depthExport = false;
  for (SharedImage& img : g.screens) {
    if (img.accessOpen) end_access(img);
    release_shared(img);
  }
  g.compositeFence = nullptr;
  drop_renderer("shutdown");
  g.active = false;
}
