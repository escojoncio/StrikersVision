// See xr_fx.h. Built with xr_quest.cpp in strikers_xr (C++20, no port prelude).

#if defined(__ANDROID__) && defined(STRIKERS_XR)

#define VK_USE_PLATFORM_ANDROID_KHR 1
#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_VULKAN 1
#include "xr_fx.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <random>
#include <vector>

#include "xr_fx_mesh.h"
#include "xr_fx_shaders.h"

namespace xrfx {
namespace {

#define FXLOG(...) __android_log_print(ANDROID_LOG_INFO, "strikers_xr", __VA_ARGS__)
#define FXERR(...) __android_log_print(ANDROID_LOG_ERROR, "strikers_xr", __VA_ARGS__)

// ---------------------------------------------------------------------------------------------
// Themes: the scorer's colours and signature piece (VISION.md, Lance 2026-10-10)
// ---------------------------------------------------------------------------------------------

enum Shape : uint8_t {
  kPaper = 0, kCoin, kHeart, kStar, kPetal, kMushroom, kEgg, kBanana, kShell, kHammer, kRose, kGarlic, kBow,
  kSpark, kMetal = 15, kFlash = 16, kMarkerRing = 17, kMarkerArrow = 18, kWash = 19
};

// The 3D mesh each piece is drawn with (xr_fx_mesh.h); sparks, flashes and the marker stay quads.
constexpr uint8_t kNoMesh = 0xFF;
uint8_t mesh_for(Shape shape) {
  switch (shape) {
  case kCoin: return mesh::kCoin;
  case kHeart: return mesh::kHeart;
  case kStar: return mesh::kStar;
  case kPetal: return mesh::kPetal;
  case kMushroom: return mesh::kMushroom;
  case kEgg: return mesh::kEgg;
  case kBanana: return mesh::kBanana;
  case kShell: return mesh::kShell;
  case kHammer: return mesh::kHammer;
  case kRose: return mesh::kRose;
  case kGarlic: return mesh::kGarlic;
  case kBow: return mesh::kBow;
  default: return kNoMesh;
  }
}

// How a piece comes to rest on the floor or the table: which of its own axes ends up pointing up
// (0 x, 1 y, 2 z) and how far its centre then sits above the surface, in piece sizes.
struct Rest {
  int up;
  float half;
};
Rest rest_for(uint8_t shape) {
  switch (shape) {
  case kCoin: return {2, 0.065f};
  case kHeart: return {2, 0.17f};
  case kStar: return {2, 0.16f};
  case kPetal: return {2, 0.06f};
  case kMushroom: return {1, 0.5f};   // stands up
  case kEgg: return {0, 0.36f};       // on its side
  case kBanana: return {2, 0.12f};
  case kShell: return {1, 0.14f};     // dome up
  case kHammer: return {2, 0.16f};
  case kRose: return {2, 0.27f};
  case kGarlic: return {1, 0.48f};
  case kBow: return {2, 0.1f};
  default: return {2, 0.02f};         // paper, foil
  }
}

// The Tabletop "that's you" marker colour, by the team captain's eCharacterClass.
uint32_t marker_colour(int captain) {
  switch (captain) {
  case 1: return 0xFF8C1A;   // Daisy
  case 2: return 0xFFC21F;   // Donkey Kong
  case 5: return 0x1FA83A;   // Luigi
  case 6: return 0xE4202A;   // Mario
  case 7: return 0xFF6FB5;   // Peach
  case 9: return 0x8A3FD1;   // Waluigi
  case 10: return 0xFFD21F;  // Wario
  case 11: return 0x3CC23C;  // Yoshi
  default: return 0x2F9BFF;
  }
}

struct Theme {
  uint32_t paper[3];  // 0xRRGGBB
  Shape piece;
  uint32_t pieceMain, pieceDetail;
};

// Indexed by eCharacterClass (Character.h): Birdo, Daisy, DK, Hammer Bro, Koopa, Luigi, Mario,
// Peach, Toad, Waluigi, Wario, Yoshi.
const Theme kThemes[12] = {
    {{0xFF6FAF, 0xE8202A, 0xFFFFFF}, kBow, 0xFF5FA0, 0xE8202A},
    {{0xFF8C1A, 0xFFD21F, 0xFFFFFF}, kPetal, 0xFFFFFF, 0xFFD21F},
    {{0x8B5A2B, 0xFFD21F, 0xD7262E}, kBanana, 0xFFD83A, 0x6B4423},
    {{0x2F6FE0, 0xC8CCD4, 0x2FB34A}, kHammer, 0x7A4A23, 0xC8CCD4},
    {{0x2FB34A, 0xFFD21F, 0xFFFFFF}, kShell, 0x22A13C, 0xF4F1E6},
    {{0x1FA83A, 0xFFFFFF, 0x1C2A6B}, kMushroom, 0x1FA83A, 0xFFFFFF},
    {{0xE4202A, 0xFFFFFF, 0x1D4FD6}, kCoin, 0xFFC21F, 0xFFE27A},
    {{0xFF8FC8, 0xFFFFFF, 0xFFC83A}, kHeart, 0xFF4F9A, 0xFFB0D6},
    {{0xE4202A, 0xFFFFFF, 0x2A56C6}, kMushroom, 0xE4202A, 0xFFFFFF},
    {{0x6B2FB3, 0x1A1A1A, 0xF0C419}, kRose, 0xD81E3A, 0x2E8B3A},
    {{0xFFD21F, 0x6E2C9C, 0xFFFFFF}, kGarlic, 0xF6F0DC, 0xD9CDAE},
    {{0x3CC23C, 0xFFFFFF, 0xE4202A}, kEgg, 0xFFFFFF, 0x3CC23C},
};
const Theme kNeutral = {{0xFFD21F, 0xFFFFFF, 0xE4202A}, kStar, 0xFFD21F, 0xFFF2A0};
const char* const kNames[12] = {"Birdo", "Daisy", "Donkey Kong", "Hammer Bro", "Koopa", "Luigi",
                                "Mario", "Peach", "Toad", "Waluigi", "Wario", "Yoshi"};

// 0xRRGGBB -> R8G8B8A8 in memory order (little-endian 0xAABBGGRR).
uint32_t pack(uint32_t rgb, uint8_t a = 255) {
  return ((rgb >> 16) & 0xFF) | (((rgb >> 8) & 0xFF) << 8) | ((rgb & 0xFF) << 16) | (uint32_t(a) << 24);
}

// ---------------------------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------------------------

enum Kind : uint8_t { kKindPaper, kKindPiece, kKindSpark, kKindRocket, kKindFlash };

// Firework burst styles.
enum Burst : uint8_t { kBurstSphere, kBurstRing, kBurstWillow, kBurstDouble };

struct Particle {
  float p[3];
  float v[3];
  float q[4];     // orientation; all zero = faces the eye
  float axis[3];  // spin axis (unit)
  float spin;     // rad/s
  float size, aspect;
  uint32_t c1, c2;
  uint8_t shape, kind;
  uint8_t mesh;   // mesh::Id, or kNoMesh for a quad
  bool landed;
  float age, life;
  float flutterPhase, flutterFreq, flutterAmp;
  float fallSpeed;
  float glow;
  float burstHeight;  // rockets: where they burst
  float drag;         // sparks: velocity kept per second (willows hang, spheres pop)
  float gravity;      // sparks: share of gravity
};

enum class Stage { Idle, Raining, Tapering };

// One instance as the vertex shader reads it (xr_fx.vert).
struct Instance {
  float posSize[4];
  float quat[4];
  uint32_t color1, color2;
  float params[4];  // shape, aspect, alpha, glow
};
static_assert(sizeof(Instance) == 56, "instance layout");

// Lance, 2026-10-10: "like we just won the Super Bowl with every goal".
constexpr uint32_t kMaxParticles = 16000;
constexpr uint32_t kSlots = 3;
constexpr uint32_t kMaxOccluderVerts = 64;

struct Slot {
  VkBuffer instances = VK_NULL_HANDLE;
  VkDeviceMemory instanceMemory = VK_NULL_HANDLE;
  void* instanceMapped = nullptr;
  VkBuffer occluders = VK_NULL_HANDLE;
  VkDeviceMemory occluderMemory = VK_NULL_HANDLE;
  void* occluderMapped = nullptr;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pending = false;
};

struct Eye {
  XrSwapchain swapchain = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageVulkanKHR> images;
  std::vector<VkImageView> views;
  std::vector<VkFramebuffer> framebuffers;
  VkImage depth = VK_NULL_HANDLE;
  VkDeviceMemory depthMemory = VK_NULL_HANDLE;
  VkImageView depthView = VK_NULL_HANDLE;
};

struct State {
  bool initTried = false;
  bool ready = false;
  DeviceInfo dev;

  Stage stage = Stage::Idle;
  float stageTime = 0.0f;
  float sinceGoal = 0.0f;
  Theme theme = kNeutral;
  float intensity = 1.0f;
  bool superStrike = false;
  int cannonWavesLeft = 0;  // waves still to fire, one every kWaveGap
  float cannonTimer = 0.0f;
  int cannonWave = 0;       // waves fired so far this goal (for the buzz pattern)
  int pendingRockets = 0;
  float rainAccum = 0.0f;
  float rocketTimer = 0.0f;
  float jetTime = 0.0f;     // pyro jets fire while > 0
  float jetAccum = 0.0f;
  bool pendingFlash = false;
  float drop = 0.0f, dropTarget = 0.0f, dropVel = 0.0f;
  bool markerShown = false;      // the player you control (Tabletop)
  float markerGame[3] = {0.0f, 0.0f, 0.0f};
  uint32_t markerColour = 0x2F9BFF;
  float markerTime = 0.0f;
  // The game's own screen flash (its white wipe flashes, the Super Strike photo flash), shown over
  // your whole view instead of on the game picture (Lance, 2026-10-10). The game reports it every
  // frame it draws one; it is gone once the reports stop.
  float washRgb[3] = {1.0f, 1.0f, 1.0f};
  float washAlpha = 0.0f;
  float washAge = 1.0f;
  float washPeak = 0.0f;
  bool washDumpArmed = false;  // self-test: dump the effects layer at the flash's height
  float nearAccum = 0.0f;      // pieces dropped close around you
  std::vector<Particle> parts;
  std::mt19937 rng{20261010u};

  VkFormat colorFormat = VK_FORMAT_UNDEFINED;
  VkRenderPass renderPass = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline fxPipeline = VK_NULL_HANDLE;
  VkPipeline occluderPipeline = VK_NULL_HANDLE;
  VkPipeline markerPipeline = VK_NULL_HANDLE;  // the effects pipeline without the depth test
  VkPipeline meshPipeline = VK_NULL_HANDLE;    // the 3D pieces: depth tested and written
  VkBuffer meshVerts = VK_NULL_HANDLE, meshIndices = VK_NULL_HANDLE;
  VkDeviceMemory meshVertMemory = VK_NULL_HANDLE, meshIndexMemory = VK_NULL_HANDLE;
  // [0] full detail (pieces near you), [1] reduced (far away or lying on the floor).
  mesh::Range meshRanges[2][mesh::kCount];
  uint32_t statPeakParts = 0, statPeakIndices = 0;  // for the stats line (largest frame since)
  VkCommandPool pool = VK_NULL_HANDLE;
  Eye eyes[2];
  Slot slots[kSlots];
  uint32_t slot = 0;
  XrCompositionLayerProjectionView layerViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                    {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  char dumpPath[256] = {};
  VkBuffer dumpBuffer = VK_NULL_HANDLE;
  VkDeviceMemory dumpMemory = VK_NULL_HANDLE;
  void* dumpMapped = nullptr;
};
State s;

float frand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(s.rng); }
int irand(int n) { return std::uniform_int_distribution<int>(0, n - 1)(s.rng); }

void quat_axis_angle(const float axis[3], float angle, float out[4]) {
  const float h = 0.5f * angle, sh = std::sin(h);
  out[0] = axis[0] * sh;
  out[1] = axis[1] * sh;
  out[2] = axis[2] * sh;
  out[3] = std::cos(h);
}

void quat_mul(const float a[4], const float b[4], float out[4]) {
  const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
  const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
  const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
  const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
  out[0] = x;
  out[1] = y;
  out[2] = z;
  out[3] = w;
}

void random_unit(float out[3]) {
  const float z = frand(-1.0f, 1.0f), a = frand(0.0f, 6.2831853f), r = std::sqrt(std::max(0.0f, 1.0f - z * z));
  out[0] = r * std::cos(a);
  out[1] = r * std::sin(a);
  out[2] = z;
}

void random_quat(float out[4]) {
  float axis[3];
  random_unit(axis);
  quat_axis_angle(axis, frand(0.0f, 6.2831853f), out);
}

// Game (x, y, z) -> LOCAL, the mapping xr_quest.cpp's m4_game_to_room uses.
XrVector3f game_to_room(float scale, float yaw, const XrVector3f& origin, float gx, float gy, float gz) {
  const float a = gx, b = gz, c = -gy;
  const float cs = std::cos(yaw), sn = std::sin(yaw);
  return XrVector3f{(a * cs + c * sn) * scale + origin.x, b * scale + origin.y, (-a * sn + c * cs) * scale + origin.z};
}

XrVector3f head_of(const Scene& sc) {
  return XrVector3f{0.5f * (sc.views[0].pose.position.x + sc.views[1].pose.position.x),
                    0.5f * (sc.views[0].pose.position.y + sc.views[1].pose.position.y),
                    0.5f * (sc.views[0].pose.position.z + sc.views[1].pose.position.z)};
}

// Size of everything: the room is small, the stadium is not.
struct Scale {
  float paper, piece, spark;
  float paperFall, pieceFall;
  float streamer;           // streamer length
  float rainRadius, rainHeight;
  float rainRate;
  float cannonSpeed;
  int cannonPapers, cannonPieces;
  float flutter;
  float burstSpeed;         // firework sparks
  int burstSparks;
  float jetSpeed, jetSpark; // pyro jets
};
Scale scale_for(const Scene& sc) {
  if (sc.mr) {
    return Scale{0.035f, 0.075f, 0.022f, 0.7f, 1.05f, 0.24f, 1.8f, 0.0f, 260.0f, 3.4f, 240, 30, 0.38f,
                 1.1f, 90, 1.6f, 0.02f};
  }
  return Scale{0.12f, 0.26f, 0.65f, 2.0f, 2.8f, 0.5f, 8.0f, 10.0f, 650.0f, 10.0f, 420, 45, 0.95f,
               11.0f, 160, 15.0f, 0.55f};
}

bool room_full() { return s.parts.size() >= kMaxParticles; }

void add_paper(const Scene& sc, const Scale& k, const XrVector3f& p, const float v[3], bool streamer = false) {
  if (room_full()) return;
  Particle q{};
  q.p[0] = p.x; q.p[1] = p.y; q.p[2] = p.z;
  std::memcpy(q.v, v, sizeof(q.v));
  random_quat(q.q);
  random_unit(q.axis);
  q.kind = kKindPaper;
  q.shape = kPaper;
  q.mesh = streamer ? mesh::kStreamer : static_cast<uint8_t>(mesh::kPaperBent + irand(3));
  if (streamer) {
    // Long thin ribbons that drift down slowly, curling.
    q.size = k.streamer * frand(0.75f, 1.2f);
    q.aspect = frand(0.05f, 0.08f);
    q.spin = frand(1.5f, 4.0f);
    q.fallSpeed = k.paperFall * frand(0.4f, 0.6f);
    q.flutterAmp = k.flutter * frand(0.9f, 1.4f);
  } else {
    q.size = k.paper * frand(0.8f, 1.25f);
    q.aspect = frand(0.45f, 0.75f);
    q.spin = frand(3.0f, 9.0f);
    q.fallSpeed = k.paperFall * frand(0.75f, 1.2f);
    q.flutterAmp = k.flutter * frand(0.5f, 1.0f);
  }
  if (!streamer && irand(100) < 18) {
    // Metallic: gold or silver foil that glints as it turns.
    const bool gold = irand(2) == 0;
    q.shape = kMetal;
    q.c1 = pack(gold ? 0xFFD34A : 0xEDEEF5);
    q.c2 = pack(gold ? 0xC8901E : 0xA4A9B4);
  } else {
    const int a = irand(3), b = (a + 1 + irand(2)) % 3;
    q.c1 = pack(s.theme.paper[a]);
    q.c2 = pack(s.theme.paper[b]);
  }
  q.life = sc.mr ? 6.0f : 3.5f;
  q.flutterPhase = frand(0.0f, 6.28f);
  q.flutterFreq = frand(1.5f, 3.2f);
  s.parts.push_back(q);
}

void add_piece(const Scene& sc, const Scale& k, const XrVector3f& p, const float v[3]) {
  if (room_full()) return;
  Particle q{};
  q.p[0] = p.x; q.p[1] = p.y; q.p[2] = p.z;
  std::memcpy(q.v, v, sizeof(q.v));
  random_quat(q.q);
  random_unit(q.axis);
  q.spin = frand(1.2f, 3.5f);
  q.size = k.piece * frand(0.85f, 1.15f);
  q.aspect = 1.0f;
  // Super Strikes mix gold stars in with the signature piece.
  const bool star = s.superStrike && irand(3) == 0;
  q.shape = star ? kStar : s.theme.piece;
  q.mesh = mesh_for(static_cast<Shape>(q.shape));
  q.c1 = pack(star ? 0xFFD21F : s.theme.pieceMain);
  q.c2 = pack(star ? 0xFFF2A0 : s.theme.pieceDetail);
  q.kind = kKindPiece;
  q.life = sc.mr ? 6.0f : 3.5f;
  q.flutterPhase = frand(0.0f, 6.28f);
  q.flutterFreq = frand(0.8f, 1.6f);
  q.flutterAmp = k.flutter * 0.35f;
  q.fallSpeed = k.pieceFall * frand(0.85f, 1.15f);
  s.parts.push_back(q);
}

void add_spark(const XrVector3f& p, const float v[3], float size, uint32_t core, uint32_t outer, float life,
               float drag, float gravity) {
  if (room_full()) return;
  Particle q{};
  q.p[0] = p.x; q.p[1] = p.y; q.p[2] = p.z;
  std::memcpy(q.v, v, sizeof(q.v));
  q.size = size;
  q.aspect = 1.0f;
  q.c1 = pack(core);
  q.c2 = pack(outer);
  q.shape = kSpark;
  q.kind = kKindSpark;
  q.glow = 1.0f;
  q.life = life;
  q.drag = drag;
  q.gravity = gravity;
  s.parts.push_back(q);
}

// A firework: sphere, ring, willow (long falling gold trails) or two-colour sphere.
void add_burst(const Scale& k, const XrVector3f& p) {
  const uint32_t main = s.theme.paper[irand(3)] == 0xFFFFFF ? s.theme.pieceMain : s.theme.paper[irand(3)];
  const uint32_t other = s.theme.paper[irand(3)];
  const Burst style = static_cast<Burst>(irand(4));
  float ringAxis[3];
  random_unit(ringAxis);
  const int n = style == kBurstWillow ? k.burstSparks * 3 / 4 : k.burstSparks;
  for (int i = 0; i < n && !room_full(); ++i) {
    float d[3];
    random_unit(d);
    if (style == kBurstRing) {
      // Flatten onto the plane across ringAxis.
      const float along = d[0] * ringAxis[0] + d[1] * ringAxis[1] + d[2] * ringAxis[2];
      for (int a = 0; a < 3; ++a) d[a] -= along * ringAxis[a];
      const float len = std::max(1e-3f, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
      for (float& c : d) c /= len;
    }
    const float sp = k.burstSpeed * (style == kBurstRing ? frand(0.95f, 1.05f) : frand(0.6f, 1.0f));
    const float v[3] = {d[0] * sp, d[1] * sp, d[2] * sp};
    if (style == kBurstWillow) {
      add_spark(p, v, k.spark * frand(0.7f, 1.0f), 0xFFF4C0, 0xE09A20, frand(2.2f, 3.2f), 2.2f, 0.55f);
    } else {
      const uint32_t outer = (style == kBurstDouble && (i & 1)) ? other : main;
      add_spark(p, v, k.spark * frand(0.8f, 1.3f), 0xFFFFFF, outer, frand(1.1f, 1.8f), 1.3f, 0.25f);
    }
  }
}

void add_rocket(const Scale& k, const XrVector3f& from, float burstHeight, float speed) {
  if (room_full()) return;
  Particle q{};
  q.p[0] = from.x; q.p[1] = from.y; q.p[2] = from.z;
  q.v[0] = frand(-0.08f, 0.08f) * speed;
  q.v[1] = speed;
  q.v[2] = frand(-0.08f, 0.08f) * speed;
  q.size = k.spark * 1.4f;
  q.aspect = 1.0f;
  q.c1 = pack(0xFFFFFF);
  q.c2 = pack(0xFFC83A);
  q.shape = kSpark;
  q.kind = kKindRocket;
  q.glow = 1.0f;
  q.life = 6.0f;
  q.burstHeight = burstHeight;
  s.parts.push_back(q);
}

// Where the cannons and pyro jets stand: the four corners of the Tabletop field in the room,
// otherwise four spots around you (front left/right, back left/right).
int launch_points(const Scene& sc, XrVector3f out[4], XrVector3f outward[4]) {
  const XrVector3f head = head_of(sc);
  if (sc.tabletop && sc.mr) {
    for (int i = 0; i < 4; ++i) {
      const float gx = (i & 1) ? 21.0f : -21.0f, gy = (i & 2) ? 12.5f : -12.5f;
      out[i] = game_to_room(sc.tableScale, sc.tableYaw, sc.tablePos, gx, gy, 0.5f);
    }
  } else {
    const XrQuaternionf& o = sc.views[0].pose.orientation;
    float fwd[3] = {-(2 * (o.x * o.z + o.w * o.y)), 0.0f, -(1 - 2 * (o.x * o.x + o.y * o.y))};
    const float len = std::max(1e-3f, std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]));
    fwd[0] /= len; fwd[2] /= len;
    const float right[3] = {-fwd[2], 0.0f, fwd[0]};
    const float side = sc.mr ? 0.9f : 4.5f, ahead = sc.mr ? 0.8f : 4.0f;
    const float y = sc.mr ? head.y - 0.6f : sc.floorY;
    for (int i = 0; i < 4; ++i) {
      const float sx = (i & 1) ? 1.0f : -1.0f, sz = (i & 2) ? -0.8f : 1.0f;
      out[i] = XrVector3f{head.x + right[0] * side * sx + fwd[0] * ahead * sz, y,
                          head.z + right[2] * side * sx + fwd[2] * ahead * sz};
    }
  }
  XrVector3f c{0.0f, 0.0f, 0.0f};
  for (int i = 0; i < 4; ++i) { c.x += out[i].x * 0.25f; c.z += out[i].z * 0.25f; }
  for (int i = 0; i < 4; ++i) {
    const float dx = out[i].x - c.x, dz = out[i].z - c.z;
    const float len = std::max(1e-3f, std::sqrt(dx * dx + dz * dz));
    outward[i] = XrVector3f{dx / len, 0.0f, dz / len};
  }
  return 4;
}

void fire_cannon_wave(const Scene& sc, const Scale& k) {
  XrVector3f from[4], out[4];
  const int n = launch_points(sc, from, out);
  const int papers = static_cast<int>(k.cannonPapers * s.intensity);
  const int pieces = static_cast<int>(k.cannonPieces * s.intensity);
  for (int i = 0; i < n; ++i) {
    for (int m = 0; m < papers + pieces; ++m) {
      const float up = k.cannonSpeed * frand(0.75f, 1.15f);
      const float outward = k.cannonSpeed * frand(0.05f, 0.4f);
      const float spread = k.cannonSpeed * 0.2f;
      const float v[3] = {out[i].x * outward + frand(-spread, spread), up, out[i].z * outward + frand(-spread, spread)};
      if (m < papers) add_paper(sc, k, from[i], v, irand(100) < (sc.mr ? 15 : 7));
      else add_piece(sc, k, from[i], v);
    }
  }
  ++s.cannonWave;
}

void launch_firework(const Scene& sc, const Scale& k) {
  if (sc.mr) {
    // Small ones from the field's corners (or around you) up to just under the ceiling.
    const XrVector3f head = head_of(sc);
    const float ceiling = std::max(sc.floorY + 2.3f, head.y + 0.45f);
    XrVector3f from[4], out[4];
    launch_points(sc, from, out);
    add_rocket(k, from[irand(4)], ceiling - frand(0.2f, 0.55f), 2.4f);
    return;
  }
  // Over the stands, all around the stadium.
  const float a = frand(0.0f, 6.2831853f), r = frand(38.0f, 55.0f);
  const XrVector3f from =
      game_to_room(sc.stadiumScale, 0.0f, sc.stadiumOrigin, std::cos(a) * r * 1.2f, std::sin(a) * r * 0.8f, 8.0f);
  add_rocket(k, from, sc.floorY + frand(26.0f, 46.0f), 26.0f);
}

// Pyro: columns of fire shooting up, at the field's corners in the room, along the stands' front in
// the stadium.
void fire_jets(const Scene& sc, const Scale& k, float dt) {
  const float rate = sc.mr ? 220.0f : 900.0f;  // sparks per second, all jets
  s.jetAccum += rate * dt;
  XrVector3f from[8];
  int n = 0;
  if (sc.mr) {
    XrVector3f out[4];
    n = launch_points(sc, from, out);
  } else {
    for (int i = 0; i < 8; ++i) {
      const float a = 6.2831853f * (i + 0.5f) / 8.0f;
      from[i] = game_to_room(sc.stadiumScale, 0.0f, sc.stadiumOrigin, std::cos(a) * 30.0f, std::sin(a) * 20.0f, 2.0f);
    }
    n = 8;
  }
  while (s.jetAccum >= 1.0f) {
    s.jetAccum -= 1.0f;
    const XrVector3f& p = from[irand(n)];
    const float sp = k.jetSpeed;
    const float v[3] = {frand(-0.08f, 0.08f) * sp, sp * frand(0.8f, 1.1f), frand(-0.08f, 0.08f) * sp};
    const uint32_t outer = irand(3) == 0 ? 0xFF3A10 : 0xFF8A1A;
    add_spark(p, v, k.jetSpark * frand(0.8f, 1.3f), 0xFFE9A0, outer, frand(0.5f, 0.8f), 1.0f, 0.6f);
  }
}

// The goal flash: a wash of the scorer's colour over the whole view, fading fast.
void add_flash(const Scene& sc) {
  if (room_full()) return;
  const XrVector3f head = head_of(sc);
  const XrQuaternionf& o = sc.views[0].pose.orientation;
  const float fwd[3] = {-(2 * (o.x * o.z + o.w * o.y)), -(2 * (o.y * o.z - o.w * o.x)),
                        -(1 - 2 * (o.x * o.x + o.y * o.y))};
  Particle q{};
  q.p[0] = head.x + fwd[0] * 0.3f;
  q.p[1] = head.y + fwd[1] * 0.3f;
  q.p[2] = head.z + fwd[2] * 0.3f;
  q.size = 3.0f;
  q.aspect = 1.0f;
  const uint32_t c = s.theme.pieceMain == 0xFFFFFF ? s.theme.paper[0] : s.theme.pieceMain;
  q.c1 = pack(c);
  q.c2 = pack(c);
  q.shape = kFlash;
  q.kind = kKindFlash;
  q.glow = 0.5f;
  q.life = 0.5f;
  s.parts.push_back(q);
}

void step_particles(const Scene& sc, const Scale& k, float dt) {
  const float g = sc.mr ? 1.6f : 9.8f;
  // The Tabletop field top, to land on.
  const float tableTop = sc.tablePos.y + 0.002f;
  const float cs = std::cos(sc.tableYaw), sn = std::sin(sc.tableYaw);
  const XrVector3f head = head_of(sc);
  size_t w = 0;
  for (size_t i = 0; i < s.parts.size(); ++i) {
    Particle& q = s.parts[i];
    q.age += dt;
    bool keep = true;
    if (q.kind == kKindFlash) {
      // Stays in front of you while it fades.
      const XrQuaternionf& o = sc.views[0].pose.orientation;
      q.p[0] = head.x - 0.3f * (2 * (o.x * o.z + o.w * o.y));
      q.p[1] = head.y - 0.3f * (2 * (o.y * o.z - o.w * o.x));
      q.p[2] = head.z - 0.3f * (1 - 2 * (o.x * o.x + o.y * o.y));
      keep = q.age < q.life;
    } else if (q.kind == kKindSpark) {
      q.v[1] -= g * q.gravity * dt;
      const float keepV = std::exp(-q.drag * dt);
      for (float& c : q.v) c *= keepV;
      for (int a = 0; a < 3; ++a) q.p[a] += q.v[a] * dt;
      keep = q.age < q.life;
    } else if (q.kind == kKindRocket) {
      for (int a = 0; a < 3; ++a) q.p[a] += q.v[a] * dt;
      // A short trail behind the rocket.
      if (irand(2) == 0) {
        const float tv[3] = {frand(-0.1f, 0.1f) * k.burstSpeed * 0.1f, -0.05f * k.burstSpeed,
                             frand(-0.1f, 0.1f) * k.burstSpeed * 0.1f};
        add_spark(XrVector3f{q.p[0], q.p[1], q.p[2]}, tv, k.spark * 0.6f, 0xFFE0A0, 0xFF8A1A, 0.4f, 1.5f, 0.3f);
      }
      if (q.p[1] >= q.burstHeight || q.age > 3.0f) {
        add_burst(k, XrVector3f{q.p[0], q.p[1], q.p[2]});
        keep = false;
      }
    } else if (!q.landed) {
      // Falls at its own pace, drifting side to side, tumbling.
      const float settle = 1.0f - std::exp(-2.6f * dt);
      q.v[1] += (-q.fallSpeed - q.v[1]) * settle;
      if (q.v[1] > 0.0f) q.v[1] -= g * dt;  // still rising out of a cannon
      const float t = q.age * q.flutterFreq + q.flutterPhase;
      const float tx = std::sin(t) * q.flutterAmp, tz = std::cos(t * 0.8f + 1.3f) * q.flutterAmp * 0.7f;
      const float side = 1.0f - std::exp(-1.6f * dt);
      q.v[0] += (tx - q.v[0]) * side;
      q.v[2] += (tz - q.v[2]) * side;
      const float prevY = q.p[1];
      for (int a = 0; a < 3; ++a) q.p[a] += q.v[a] * dt;
      float dq[4];
      quat_axis_angle(q.axis, q.spin * dt, dq);
      float nq[4];
      quat_mul(dq, q.q, nq);
      std::memcpy(q.q, nq, sizeof(nq));
      // Come to rest the way the piece would: paper and coins flat, a mushroom standing, an egg on its
      // side, sitting on the surface rather than half through it; turned at random.
      const Rest rest = rest_for(q.shape);
      const float lift = rest.half * q.size;
      float landY = sc.floorY + 0.003f + lift;
      if (sc.tabletop && sc.mr && prevY >= tableTop + lift && q.p[1] < tableTop + lift) {
        // Over the field? Back into game units, where the pitch is a plain rectangle.
        const float dx = (q.p[0] - sc.tablePos.x) / sc.tableScale, dz = (q.p[2] - sc.tablePos.z) / sc.tableScale;
        const float gx = dx * cs - dz * sn, gy = -(dx * sn + dz * cs);
        if (std::fabs(gx) < 23.0f && std::fabs(gy) < 14.0f) landY = tableTop + lift;
      }
      if (q.p[1] <= landY) {
        q.p[1] = landY;
        q.landed = true;
        q.age = 0.0f;
        const float xAxis[3] = {1.0f, 0.0f, 0.0f}, yAxis[3] = {0.0f, 1.0f, 0.0f}, zAxis[3] = {0.0f, 0.0f, 1.0f};
        float lay[4] = {0.0f, 0.0f, 0.0f, 1.0f}, yaw[4];
        if (rest.up == 2) quat_axis_angle(xAxis, -1.5707963f, lay);
        else if (rest.up == 0) quat_axis_angle(zAxis, 1.5707963f, lay);
        quat_axis_angle(yAxis, frand(0.0f, 6.2831853f), yaw);
        quat_mul(yaw, lay, q.q);
      }
      keep = q.age < 25.0f && q.p[1] > sc.floorY - 1.0f;
    } else {
      keep = q.age < q.life;
    }
    if (keep) s.parts[w++] = q;
  }
  s.parts.resize(w);
}

// ---------------------------------------------------------------------------------------------
// Vulkan
// ---------------------------------------------------------------------------------------------

bool vk_ok(VkResult r, const char* what) {
  if (r == VK_SUCCESS) return true;
  FXERR("fx: %s failed: VkResult %d", what, static_cast<int>(r));
  return false;
}

bool xr_ok(XrResult r, const char* what) {
  if (XR_SUCCEEDED(r)) return true;
  FXERR("fx: %s failed: XrResult %d", what, static_cast<int>(r));
  return false;
}

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < s.dev.memProps->memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (s.dev.memProps->memoryTypes[i].propertyFlags & want) == want) return i;
  }
  return UINT32_MAX;
}

bool make_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buffer, VkDeviceMemory* memory,
                      void** mapped) {
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (!vk_ok(vkCreateBuffer(s.dev.device, &info, nullptr, buffer), "vkCreateBuffer")) return false;
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(s.dev.device, *buffer, &req);
  const uint32_t type =
      memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (type == UINT32_MAX) return false;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type;
  return vk_ok(vkAllocateMemory(s.dev.device, &alloc, nullptr, memory), "vkAllocateMemory") &&
         vk_ok(vkBindBufferMemory(s.dev.device, *buffer, *memory, 0), "vkBindBufferMemory") &&
         vk_ok(vkMapMemory(s.dev.device, *memory, 0, VK_WHOLE_SIZE, 0, mapped), "vkMapMemory");
}

VkShaderModule make_shader(const uint32_t* code, size_t bytes) {
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = bytes;
  info.pCode = code;
  VkShaderModule m = VK_NULL_HANDLE;
  vk_ok(vkCreateShaderModule(s.dev.device, &info, nullptr, &m), "vkCreateShaderModule");
  return m;
}

bool choose_format() {
  uint32_t count = 0;
  xrEnumerateSwapchainFormats(s.dev.session, 0, &count, nullptr);
  std::vector<int64_t> formats(count);
  if (count > 0) xrEnumerateSwapchainFormats(s.dev.session, count, &count, formats.data());
  for (VkFormat want : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB}) {
    if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(want)) != formats.end()) {
      s.colorFormat = want;
      return true;
    }
  }
  FXERR("fx: no sRGB swapchain format");
  return false;
}

bool make_render_pass() {
  VkAttachmentDescription att[2]{};
  att[0].format = s.colorFormat;
  att[0].samples = VK_SAMPLE_COUNT_1_BIT;
  att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;  // what OpenXR wants back
  att[1].format = VK_FORMAT_D16_UNORM;
  att[1].samples = VK_SAMPLE_COUNT_1_BIT;
  att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &colorRef;
  sub.pDepthStencilAttachment = &depthRef;
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
  VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = 2;
  info.pAttachments = att;
  info.subpassCount = 1;
  info.pSubpasses = &sub;
  info.dependencyCount = 1;
  info.pDependencies = &dep;
  return vk_ok(vkCreateRenderPass(s.dev.device, &info, nullptr, &s.renderPass), "vkCreateRenderPass");
}

struct PushConstants {
  float viewProj[16];
  float camRight[4];
  float camUp[4];
  float camPos[4];  // the eye, for the 3D pieces' highlights
};

bool make_pipelines() {
  VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants)};
  VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &range;
  if (!vk_ok(vkCreatePipelineLayout(s.dev.device, &layoutInfo, nullptr, &s.layout), "vkCreatePipelineLayout")) {
    return false;
  }
  VkShaderModule fxVert = make_shader(kXrFxVert, sizeof(kXrFxVert));
  VkShaderModule fxFrag = make_shader(kXrFxFrag, sizeof(kXrFxFrag));
  VkShaderModule ocVert = make_shader(kXrFxOccluderVert, sizeof(kXrFxOccluderVert));
  VkShaderModule ocFrag = make_shader(kXrFxOccluderFrag, sizeof(kXrFxOccluderFrag));
  VkShaderModule meshVert = make_shader(kXrFxMeshVert, sizeof(kXrFxMeshVert));
  VkShaderModule meshFrag = make_shader(kXrFxMeshFrag, sizeof(kXrFxMeshFrag));
  if (!fxVert || !fxFrag || !ocVert || !ocFrag || !meshVert || !meshFrag) return false;

  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dyn;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Effects: instanced quads, premultiplied alpha, tested against the stand-ins' depth.
  VkVertexInputBindingDescription fxBinding{0, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE};
  const VkVertexInputAttributeDescription fxAttrs[] = {
      {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, posSize)},
      {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, quat)},
      {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Instance, color1)},
      {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Instance, color2)},
      {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, params)},
  };
  VkPipelineVertexInputStateCreateInfo fxInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  fxInput.vertexBindingDescriptionCount = 1;
  fxInput.pVertexBindingDescriptions = &fxBinding;
  fxInput.vertexAttributeDescriptionCount = 5;
  fxInput.pVertexAttributeDescriptions = fxAttrs;
  VkPipelineInputAssemblyStateCreateInfo strip{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  strip.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkPipelineDepthStencilStateCreateInfo fxDepth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  fxDepth.depthTestEnable = VK_TRUE;
  fxDepth.depthWriteEnable = VK_FALSE;
  fxDepth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  VkPipelineColorBlendAttachmentState fxBlend{};
  fxBlend.blendEnable = VK_TRUE;
  fxBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  fxBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  fxBlend.colorBlendOp = VK_BLEND_OP_ADD;
  fxBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  fxBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  fxBlend.alphaBlendOp = VK_BLEND_OP_ADD;
  fxBlend.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo fxBlendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  fxBlendState.attachmentCount = 1;
  fxBlendState.pAttachments = &fxBlend;
  VkPipelineShaderStageCreateInfo fxStages[2]{};
  fxStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  fxStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  fxStages[0].module = fxVert;
  fxStages[0].pName = "main";
  fxStages[1] = fxStages[0];
  fxStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  fxStages[1].module = fxFrag;
  VkGraphicsPipelineCreateInfo fxInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  fxInfo.stageCount = 2;
  fxInfo.pStages = fxStages;
  fxInfo.pVertexInputState = &fxInput;
  fxInfo.pInputAssemblyState = &strip;
  fxInfo.pViewportState = &viewport;
  fxInfo.pRasterizationState = &raster;
  fxInfo.pMultisampleState = &ms;
  fxInfo.pDepthStencilState = &fxDepth;
  fxInfo.pColorBlendState = &fxBlendState;
  fxInfo.pDynamicState = &dynamic;
  fxInfo.layout = s.layout;
  fxInfo.renderPass = s.renderPass;

  // Stand-ins: plain triangles, depth only.
  VkVertexInputBindingDescription ocBinding{0, sizeof(float) * 3, VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription ocAttr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
  VkPipelineVertexInputStateCreateInfo ocInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  ocInput.vertexBindingDescriptionCount = 1;
  ocInput.pVertexBindingDescriptions = &ocBinding;
  ocInput.vertexAttributeDescriptionCount = 1;
  ocInput.pVertexAttributeDescriptions = &ocAttr;
  VkPipelineInputAssemblyStateCreateInfo list{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  list.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineDepthStencilStateCreateInfo ocDepth = fxDepth;
  ocDepth.depthWriteEnable = VK_TRUE;
  ocDepth.depthCompareOp = VK_COMPARE_OP_LESS;
  VkPipelineColorBlendAttachmentState ocBlend{};
  ocBlend.colorWriteMask = 0;
  VkPipelineColorBlendStateCreateInfo ocBlendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  ocBlendState.attachmentCount = 1;
  ocBlendState.pAttachments = &ocBlend;
  VkPipelineShaderStageCreateInfo ocStages[2] = {fxStages[0], fxStages[1]};
  ocStages[0].module = ocVert;
  ocStages[1].module = ocFrag;
  VkGraphicsPipelineCreateInfo ocInfo = fxInfo;
  ocInfo.pStages = ocStages;
  ocInfo.pVertexInputState = &ocInput;
  ocInfo.pInputAssemblyState = &list;
  ocInfo.pDepthStencilState = &ocDepth;
  ocInfo.pColorBlendState = &ocBlendState;

  // The player marker draws over everything: it must read even behind other players or the fence.
  VkPipelineDepthStencilStateCreateInfo markerDepth = fxDepth;
  markerDepth.depthTestEnable = VK_FALSE;
  VkGraphicsPipelineCreateInfo markerInfo = fxInfo;
  markerInfo.pDepthStencilState = &markerDepth;

  // The 3D pieces: mesh vertices (binding 0) times the same instances (binding 1), solid, so they
  // write depth and hide each other and their own far side. Both faces drawn (paper is one sheet).
  const VkVertexInputBindingDescription meshBindings[] = {
      {0, sizeof(mesh::Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
      {1, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE},
  };
  const VkVertexInputAttributeDescription meshAttrs[] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(mesh::Vertex, pos)},
      {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(mesh::Vertex, wid)},
      {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(mesh::Vertex, nrm)},
      {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(mesh::Vertex, mat)},
      {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, posSize)},
      {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, quat)},
      {6, 1, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Instance, color1)},
      {7, 1, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Instance, color2)},
      {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, params)},
  };
  VkPipelineVertexInputStateCreateInfo meshInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  meshInput.vertexBindingDescriptionCount = 2;
  meshInput.pVertexBindingDescriptions = meshBindings;
  meshInput.vertexAttributeDescriptionCount = 9;
  meshInput.pVertexAttributeDescriptions = meshAttrs;
  VkPipelineDepthStencilStateCreateInfo meshDepth = fxDepth;
  meshDepth.depthWriteEnable = VK_TRUE;
  VkPipelineShaderStageCreateInfo meshStages[2] = {fxStages[0], fxStages[1]};
  meshStages[0].module = meshVert;
  meshStages[1].module = meshFrag;
  VkGraphicsPipelineCreateInfo meshInfo = fxInfo;
  meshInfo.pStages = meshStages;
  meshInfo.pVertexInputState = &meshInput;
  meshInfo.pInputAssemblyState = &list;
  meshInfo.pDepthStencilState = &meshDepth;

  const bool ok =
      vk_ok(vkCreateGraphicsPipelines(s.dev.device, VK_NULL_HANDLE, 1, &fxInfo, nullptr, &s.fxPipeline),
            "vkCreateGraphicsPipelines(fx)") &&
      vk_ok(vkCreateGraphicsPipelines(s.dev.device, VK_NULL_HANDLE, 1, &meshInfo, nullptr, &s.meshPipeline),
            "vkCreateGraphicsPipelines(mesh)") &&
      vk_ok(vkCreateGraphicsPipelines(s.dev.device, VK_NULL_HANDLE, 1, &markerInfo, nullptr, &s.markerPipeline),
            "vkCreateGraphicsPipelines(marker)") &&
      vk_ok(vkCreateGraphicsPipelines(s.dev.device, VK_NULL_HANDLE, 1, &ocInfo, nullptr, &s.occluderPipeline),
            "vkCreateGraphicsPipelines(occluder)");
  vkDestroyShaderModule(s.dev.device, fxVert, nullptr);
  vkDestroyShaderModule(s.dev.device, fxFrag, nullptr);
  vkDestroyShaderModule(s.dev.device, ocVert, nullptr);
  vkDestroyShaderModule(s.dev.device, ocFrag, nullptr);
  vkDestroyShaderModule(s.dev.device, meshVert, nullptr);
  vkDestroyShaderModule(s.dev.device, meshFrag, nullptr);
  return ok;
}

// The pieces' meshes at two levels of detail, built once into two host-visible buffers.
bool make_meshes() {
  mesh::Set set = mesh::build(1.0f);
  const mesh::Set far = mesh::build(0.4f);
  std::memcpy(s.meshRanges[0], set.ranges, sizeof(s.meshRanges[0]));
  const uint32_t vbase = static_cast<uint32_t>(set.verts.size());
  const uint32_t ibase = static_cast<uint32_t>(set.indices.size());
  set.verts.insert(set.verts.end(), far.verts.begin(), far.verts.end());
  for (uint16_t i : far.indices) set.indices.push_back(static_cast<uint16_t>(vbase + i));
  for (uint32_t m = 0; m < mesh::kCount; ++m) {
    s.meshRanges[1][m] = mesh::Range{far.ranges[m].firstIndex + ibase, far.ranges[m].indexCount};
  }
  if (set.verts.size() > 65535) {
    FXERR("fx: piece meshes too large (%zu vertices)", set.verts.size());
    return false;
  }
  void* mapped = nullptr;
  const VkDeviceSize vbytes = sizeof(mesh::Vertex) * set.verts.size();
  const VkDeviceSize ibytes = sizeof(uint16_t) * set.indices.size();
  if (!make_host_buffer(vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s.meshVerts, &s.meshVertMemory, &mapped)) {
    return false;
  }
  std::memcpy(mapped, set.verts.data(), vbytes);
  vkUnmapMemory(s.dev.device, s.meshVertMemory);
  if (!make_host_buffer(ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &s.meshIndices, &s.meshIndexMemory, &mapped)) {
    return false;
  }
  std::memcpy(mapped, set.indices.data(), ibytes);
  vkUnmapMemory(s.dev.device, s.meshIndexMemory);
  FXLOG("fx: piece meshes ready (%zu vertices, %zu triangles)", set.verts.size(), set.indices.size() / 3);
  return true;
}

bool make_eye(Eye& eye) {
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  info.format = s.colorFormat;
  info.sampleCount = 1;
  info.width = s.dev.width;
  info.height = s.dev.height;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  if (!xr_ok(xrCreateSwapchain(s.dev.session, &info, &eye.swapchain), "xrCreateSwapchain(fx)")) return false;
  uint32_t count = 0;
  xrEnumerateSwapchainImages(eye.swapchain, 0, &count, nullptr);
  eye.images.assign(count, XrSwapchainImageVulkanKHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  if (!xr_ok(xrEnumerateSwapchainImages(eye.swapchain, count, &count,
                                        reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data())),
             "xrEnumerateSwapchainImages(fx)")) {
    return false;
  }

  VkImageCreateInfo di{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  di.imageType = VK_IMAGE_TYPE_2D;
  di.format = VK_FORMAT_D16_UNORM;
  di.extent = {s.dev.width, s.dev.height, 1};
  di.mipLevels = 1;
  di.arrayLayers = 1;
  di.samples = VK_SAMPLE_COUNT_1_BIT;
  di.tiling = VK_IMAGE_TILING_OPTIMAL;
  di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  di.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!vk_ok(vkCreateImage(s.dev.device, &di, nullptr, &eye.depth), "vkCreateImage(fx depth)")) return false;
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(s.dev.device, eye.depth, &req);
  uint32_t type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
  if (type == UINT32_MAX) type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type == UINT32_MAX) return false;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type;
  if (!vk_ok(vkAllocateMemory(s.dev.device, &alloc, nullptr, &eye.depthMemory), "vkAllocateMemory(fx depth)") ||
      !vk_ok(vkBindImageMemory(s.dev.device, eye.depth, eye.depthMemory, 0), "vkBindImageMemory(fx depth)")) {
    return false;
  }
  VkImageViewCreateInfo dv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  dv.image = eye.depth;
  dv.viewType = VK_IMAGE_VIEW_TYPE_2D;
  dv.format = VK_FORMAT_D16_UNORM;
  dv.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  if (!vk_ok(vkCreateImageView(s.dev.device, &dv, nullptr, &eye.depthView), "vkCreateImageView(fx depth)")) {
    return false;
  }

  for (const auto& img : eye.images) {
    VkImageViewCreateInfo cv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    cv.image = img.image;
    cv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    cv.format = s.colorFormat;
    cv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (!vk_ok(vkCreateImageView(s.dev.device, &cv, nullptr, &view), "vkCreateImageView(fx)")) return false;
    eye.views.push_back(view);
    const VkImageView attachments[2] = {view, eye.depthView};
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = s.renderPass;
    fb.attachmentCount = 2;
    fb.pAttachments = attachments;
    fb.width = s.dev.width;
    fb.height = s.dev.height;
    fb.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (!vk_ok(vkCreateFramebuffer(s.dev.device, &fb, nullptr, &framebuffer), "vkCreateFramebuffer(fx)")) {
      return false;
    }
    eye.framebuffers.push_back(framebuffer);
  }
  return true;
}

bool make_slots() {
  VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = s.dev.queueFamily;
  if (!vk_ok(vkCreateCommandPool(s.dev.device, &poolInfo, nullptr, &s.pool), "vkCreateCommandPool(fx)")) return false;
  for (Slot& slot : s.slots) {
    if (!make_host_buffer(sizeof(Instance) * kMaxParticles, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &slot.instances,
                          &slot.instanceMemory, &slot.instanceMapped) ||
        !make_host_buffer(sizeof(float) * 3 * kMaxOccluderVerts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &slot.occluders,
                          &slot.occluderMemory, &slot.occluderMapped)) {
      return false;
    }
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = s.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!vk_ok(vkAllocateCommandBuffers(s.dev.device, &alloc, &slot.cmd), "vkAllocateCommandBuffers(fx)") ||
        !vk_ok(vkCreateFence(s.dev.device, &fence, nullptr, &slot.fence), "vkCreateFence(fx)")) {
      return false;
    }
  }
  return true;
}

// Column-major 4x4 (GLSL's layout).
void mat_mul(const float a[16], const float b[16], float out[16]) {
  float r[16];
  for (int c = 0; c < 4; ++c)
    for (int rr = 0; rr < 4; ++rr) {
      float v = 0.0f;
      for (int k = 0; k < 4; ++k) v += a[k * 4 + rr] * b[c * 4 + k];
      r[c * 4 + rr] = v;
    }
  std::memcpy(out, r, sizeof(r));
}

// Vulkan clip space (y down, z 0..1) from an OpenXR field of view, as the OpenXR SDK's
// XrMatrix4x4f_CreateProjectionFov does for GRAPHICS_VULKAN.
void projection(const XrFovf& fov, float n, float f, float out[16]) {
  const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
  const float u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
  const float w = r - l, h = d - u;
  std::memset(out, 0, sizeof(float) * 16);
  out[0] = 2.0f / w;
  out[8] = (r + l) / w;
  out[5] = 2.0f / h;
  out[9] = (u + d) / h;
  out[10] = -f / (f - n);
  out[14] = -(f * n) / (f - n);
  out[11] = -1.0f;
}

void rotate(const XrQuaternionf& q, const float v[3], float out[3]) {
  const float t[3] = {2 * (q.y * v[2] - q.z * v[1]), 2 * (q.z * v[0] - q.x * v[2]), 2 * (q.x * v[1] - q.y * v[0])};
  out[0] = v[0] + q.w * t[0] + (q.y * t[2] - q.z * t[1]);
  out[1] = v[1] + q.w * t[1] + (q.z * t[0] - q.x * t[2]);
  out[2] = v[2] + q.w * t[2] + (q.x * t[1] - q.y * t[0]);
}

// LOCAL -> eye (inverse of the pose), column-major.
void view_matrix(const XrPosef& pose, float out[16]) {
  const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
  float rx[3], ry[3], rz[3];
  rotate(pose.orientation, ex, rx);
  rotate(pose.orientation, ey, ry);
  rotate(pose.orientation, ez, rz);
  const float p[3] = {pose.position.x, pose.position.y, pose.position.z};
  // Rows of the inverse rotation are the rotated axes.
  out[0] = rx[0]; out[4] = rx[1]; out[8] = rx[2];  out[12] = -(rx[0] * p[0] + rx[1] * p[1] + rx[2] * p[2]);
  out[1] = ry[0]; out[5] = ry[1]; out[9] = ry[2];  out[13] = -(ry[0] * p[0] + ry[1] * p[1] + ry[2] * p[2]);
  out[2] = rz[0]; out[6] = rz[1]; out[10] = rz[2]; out[14] = -(rz[0] * p[0] + rz[1] * p[1] + rz[2] * p[2]);
  out[3] = 0;     out[7] = 0;     out[11] = 0;     out[15] = 1;
}

// The stand-ins: the Tabletop field as a box up to the fence top, and the screen.
uint32_t write_occluders(const Scene& sc, float* out) {
  uint32_t n = 0;
  auto tri = [&](const XrVector3f& a, const XrVector3f& b, const XrVector3f& c) {
    if (n + 3 > kMaxOccluderVerts) return;
    for (const XrVector3f* v : {&a, &b, &c}) {
      out[n * 3 + 0] = v->x;
      out[n * 3 + 1] = v->y;
      out[n * 3 + 2] = v->z;
      ++n;
    }
  };
  if (sc.tabletop) {
    XrVector3f c[8];
    for (int i = 0; i < 8; ++i) {
      c[i] = game_to_room(sc.tableScale, sc.tableYaw, sc.tablePos, (i & 1) ? 22.5f : -22.5f, (i & 2) ? 13.5f : -13.5f,
                          (i & 4) ? 2.0f : -0.2f);
    }
    const int faces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    for (const auto& f : faces) {
      tri(c[f[0]], c[f[1]], c[f[2]]);
      tri(c[f[0]], c[f[2]], c[f[3]]);
    }
  }
  if (sc.screenValid) {
    const float hw = 0.5f * sc.screenWidth, hh = 0.5f * sc.screenHeight;
    const float corners[4][3] = {{-hw, -hh, 0}, {hw, -hh, 0}, {hw, hh, 0}, {-hw, hh, 0}};
    XrVector3f w[4];
    for (int i = 0; i < 4; ++i) {
      float r[3];
      rotate(sc.screenPose.orientation, corners[i], r);
      w[i] = XrVector3f{r[0] + sc.screenPose.position.x, r[1] + sc.screenPose.position.y,
                        r[2] + sc.screenPose.position.z};
    }
    tri(w[0], w[1], w[2]);
    tri(w[0], w[2], w[3]);
  }
  return n;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------------------------

bool init(const DeviceInfo& device) {
  if (s.initTried) return s.ready;
  s.initTried = true;
  s.dev = device;
  s.ready = s.dev.device != VK_NULL_HANDLE && s.dev.width > 0 && choose_format() && make_render_pass() &&
            make_pipelines() && make_meshes() && make_eye(s.eyes[0]) && make_eye(s.eyes[1]) && make_slots();
  if (s.ready) {
    FXLOG("fx: celebration renderer ready (%ux%u per eye, format %d)", s.dev.width, s.dev.height,
          static_cast<int>(s.colorFormat));
    s.parts.reserve(kMaxParticles);
  } else {
    FXERR("fx: celebration renderer failed to start; goals will not show effects");
  }
  return s.ready;
}

void shutdown() {
  if (s.dev.device == VK_NULL_HANDLE) return;
  vkDeviceWaitIdle(s.dev.device);
  for (Slot& slot : s.slots) {
    if (slot.instanceMapped) vkUnmapMemory(s.dev.device, slot.instanceMemory);
    if (slot.occluderMapped) vkUnmapMemory(s.dev.device, slot.occluderMemory);
    if (slot.instances) vkDestroyBuffer(s.dev.device, slot.instances, nullptr);
    if (slot.occluders) vkDestroyBuffer(s.dev.device, slot.occluders, nullptr);
    if (slot.instanceMemory) vkFreeMemory(s.dev.device, slot.instanceMemory, nullptr);
    if (slot.occluderMemory) vkFreeMemory(s.dev.device, slot.occluderMemory, nullptr);
    if (slot.fence) vkDestroyFence(s.dev.device, slot.fence, nullptr);
  }
  for (Eye& eye : s.eyes) {
    for (VkFramebuffer fb : eye.framebuffers) vkDestroyFramebuffer(s.dev.device, fb, nullptr);
    for (VkImageView v : eye.views) vkDestroyImageView(s.dev.device, v, nullptr);
    if (eye.depthView) vkDestroyImageView(s.dev.device, eye.depthView, nullptr);
    if (eye.depth) vkDestroyImage(s.dev.device, eye.depth, nullptr);
    if (eye.depthMemory) vkFreeMemory(s.dev.device, eye.depthMemory, nullptr);
    if (eye.swapchain) xrDestroySwapchain(eye.swapchain);
  }
  if (s.dumpMapped) vkUnmapMemory(s.dev.device, s.dumpMemory);
  if (s.dumpBuffer) vkDestroyBuffer(s.dev.device, s.dumpBuffer, nullptr);
  if (s.dumpMemory) vkFreeMemory(s.dev.device, s.dumpMemory, nullptr);
  if (s.pool) vkDestroyCommandPool(s.dev.device, s.pool, nullptr);
  if (s.fxPipeline) vkDestroyPipeline(s.dev.device, s.fxPipeline, nullptr);
  if (s.markerPipeline) vkDestroyPipeline(s.dev.device, s.markerPipeline, nullptr);
  if (s.meshPipeline) vkDestroyPipeline(s.dev.device, s.meshPipeline, nullptr);
  if (s.meshVerts) vkDestroyBuffer(s.dev.device, s.meshVerts, nullptr);
  if (s.meshIndices) vkDestroyBuffer(s.dev.device, s.meshIndices, nullptr);
  if (s.meshVertMemory) vkFreeMemory(s.dev.device, s.meshVertMemory, nullptr);
  if (s.meshIndexMemory) vkFreeMemory(s.dev.device, s.meshIndexMemory, nullptr);
  if (s.occluderPipeline) vkDestroyPipeline(s.dev.device, s.occluderPipeline, nullptr);
  if (s.layout) vkDestroyPipelineLayout(s.dev.device, s.layout, nullptr);
  if (s.renderPass) vkDestroyRenderPass(s.dev.device, s.renderPass, nullptr);
  s = State{};
}

// The cannons fire in waves this far apart; the Touch buzz follows them (cannon_waves_fired).
constexpr float kWaveGap = 0.45f;

void goal(int team, int character, bool superStrike, bool hyper, bool ownGoal) {
  s.theme = (character >= 0 && character < 12) ? kThemes[character] : kNeutral;
  s.superStrike = superStrike || hyper;
  s.intensity = ownGoal ? 0.45f : (superStrike ? 1.4f : 1.0f);
  s.stage = Stage::Raining;
  s.stageTime = 0.0f;
  s.sinceGoal = 0.0f;
  // Super Bowl, every goal: four cannons in three waves, a fireworks barrage, pyro, a flash.
  s.cannonWavesLeft = ownGoal ? 1 : 3;
  s.cannonTimer = 0.0f;
  s.cannonWave = 0;
  s.pendingRockets = ownGoal ? 2 : (superStrike ? 24 : 14);
  s.rocketTimer = 0.0f;
  s.jetTime = ownGoal ? 0.0f : (superStrike ? 4.0f : 2.6f);
  s.pendingFlash = !ownGoal;
  FXLOG("fx: goal for team %d, %s theme%s%s", team,
        (character >= 0 && character < 12) ? kNames[character] : "neutral", superStrike ? ", Super Strike" : "",
        ownGoal ? ", own goal (toned down)" : "");
}

void phase(int p) {
  switch (p) {
  case 1:  // replay started: the jumbotron comes down, and a second volley goes up
    s.dropTarget = 1.0f;
    if (s.stage == Stage::Raining && s.intensity > 0.5f) {
      s.cannonWavesLeft += 2;
      s.pendingRockets += 8;
      s.jetTime = std::max(s.jetTime, 1.6f);
    }
    break;
  case 2:  // replay played out: taper off, a few stragglers
  case 4:  // play live again
    if (s.stage == Stage::Raining) {
      s.stage = Stage::Tapering;
      s.stageTime = 0.0f;
    }
    s.dropTarget = 0.0f;
    break;
  case 3:  // skipped: everything stops at once
    s.parts.clear();
    s.stage = Stage::Idle;
    s.stageTime = 0.0f;
    s.cannonWavesLeft = 0;
    s.pendingRockets = 0;
    s.jetTime = 0.0f;
    s.pendingFlash = false;
    s.drop = s.dropTarget = s.dropVel = 0.0f;
    break;
  default:
    break;
  }
}

int cannon_waves_fired() { return s.cannonWave; }

void set_marker(bool shown, float x, float y, float z, int teamCaptain) {
  s.markerShown = shown;
  s.markerGame[0] = x;
  s.markerGame[1] = y;
  s.markerGame[2] = z;
  s.markerColour = marker_colour(teamCaptain);
}

static bool marker_active(const Scene& sc) { return s.markerShown && sc.tabletop; }

// The game reports its flash once a game frame; between reports (the display runs faster, or the
// game frame is slow) the last one holds, and it is over once they stop.
static bool wash_active() { return s.washAge < 0.1f && s.washAlpha > 0.003f; }

void screen_flash(float r, float g, float b, float a) {
  if (!wash_active() && a > 0.003f) {
    FXLOG("fx: game screen flash (%.2f %.2f %.2f) -> whole view", r, g, b);
    s.washPeak = 0.0f;
  }
  s.washRgb[0] = r;
  s.washRgb[1] = g;
  s.washRgb[2] = b;
  // A touch under full: a solid white field right at the eyes is harsher than on a TV.
  s.washAlpha = std::min(std::max(a, 0.0f), 1.0f) * 0.9f;
  s.washAge = 0.0f;
  s.washPeak = std::max(s.washPeak, s.washAlpha);
  if (s.washDumpArmed && s.washAlpha >= 0.5f) {
    s.washDumpArmed = false;
    request_dump("/storage/emulated/0/Strikers/xr_fx.ppm");
  }
}

void dump_at_flash() { s.washDumpArmed = true; }

bool update(const Scene& sc) {
  const float dt = std::min(std::max(sc.dt, 0.0f), 0.1f);
  s.stageTime += dt;
  s.sinceGoal += dt;
  s.markerTime += dt;
  s.washAge += dt;
  const bool washing = wash_active();

  // The jumbotron: a slightly bouncy spring.
  const float acc = 70.0f * (s.dropTarget - s.drop) - 10.0f * s.dropVel;
  s.dropVel += acc * dt;
  s.drop += s.dropVel * dt;
  if (s.dropTarget == 0.0f && s.drop < 0.002f && std::fabs(s.dropVel) < 0.01f) s.drop = s.dropVel = 0.0f;

  if (s.stage == Stage::Idle && s.parts.empty()) return s.drop > 0.0f || marker_active(sc) || washing;
  const Scale k = scale_for(sc);

  if (s.stage == Stage::Raining && s.stageTime > 30.0f) {  // in case the replay never reports
    s.stage = Stage::Tapering;
    s.stageTime = 0.0f;
  }

  if (s.pendingFlash) {
    s.pendingFlash = false;
    add_flash(sc);
  }
  if (s.cannonWavesLeft > 0) {
    s.cannonTimer -= dt;
    if (s.cannonTimer <= 0.0f) {
      fire_cannon_wave(sc, k);
      --s.cannonWavesLeft;
      s.cannonTimer = kWaveGap;
    }
  }
  if (s.jetTime > 0.0f) {
    s.jetTime -= dt;
    fire_jets(sc, k, dt);
  }

  // Rain from above, streamers among it.
  float rate = 0.0f;
  if (s.stage == Stage::Raining) {
    rate = k.rainRate * s.intensity * std::min(1.0f, 0.4f + s.stageTime);
  } else if (s.stage == Stage::Tapering) {
    rate = k.rainRate * s.intensity * std::exp(-s.stageTime / 1.3f);
    if (s.stageTime < 8.0f) rate += (sc.mr ? 2.0f : 8.0f) * (1.0f - s.stageTime / 8.0f);  // stragglers
    if (s.stageTime > 8.0f) s.stage = Stage::Idle;
  }
  s.rainAccum += rate * dt;
  const XrVector3f head = head_of(sc);
  XrVector3f center = head;
  if (sc.tabletop && sc.mr) {
    center.x = 0.5f * (head.x + sc.tablePos.x);
    center.z = 0.5f * (head.z + sc.tablePos.z);
  }
  const float top = sc.mr ? std::max(sc.floorY + 2.3f, head.y + 0.45f) : sc.floorY + k.rainHeight;
  while (s.rainAccum >= 1.0f) {
    s.rainAccum -= 1.0f;
    const float a = frand(0.0f, 6.2831853f), r = k.rainRadius * std::sqrt(frand(0.0f, 1.0f));
    const XrVector3f p{center.x + std::cos(a) * r, top + frand(0.0f, sc.mr ? 0.2f : 2.0f), center.z + std::sin(a) * r};
    const float v[3] = {0.0f, -k.paperFall * 0.5f, 0.0f};
    const int roll = irand(100);
    if (roll < 9) add_piece(sc, k, p, v);
    else add_paper(sc, k, p, v, roll < (sc.mr ? 21 : 13));  // streamers: fewer in the stadium
  }
  // A few signature pieces drift down close around you, where their shape reads best (never
  // closer than 40 cm to your head).
  if (s.stage == Stage::Raining) {
    s.nearAccum += (sc.mr ? 6.0f : 10.0f) * s.intensity * dt;
    while (s.nearAccum >= 1.0f) {
      s.nearAccum -= 1.0f;
      const float a = frand(0.0f, 6.2831853f), r = sc.mr ? frand(0.4f, 1.0f) : frand(0.7f, 2.0f);
      const XrVector3f p{head.x + std::cos(a) * r, top + frand(0.0f, sc.mr ? 0.2f : 1.0f), head.z + std::sin(a) * r};
      const float v[3] = {0.0f, -k.pieceFall * 0.5f, 0.0f};
      add_piece(sc, k, p, v);
    }
  }

  // Fireworks: a barrage at the goal, then steadily while the party lasts.
  if (s.stage == Stage::Raining || (s.stage == Stage::Tapering && s.stageTime < 1.0f)) {
    s.rocketTimer -= dt;
    if (s.rocketTimer <= 0.0f) {
      launch_firework(sc, k);
      if (s.pendingRockets > 0) {
        --s.pendingRockets;
        s.rocketTimer = sc.mr ? frand(0.2f, 0.35f) : frand(0.08f, 0.18f);
      } else if (sc.mr) {
        s.rocketTimer = s.superStrike ? frand(0.4f, 0.6f) : frand(0.8f, 1.2f);
      } else {
        s.rocketTimer = s.superStrike ? frand(0.15f, 0.25f) : frand(0.3f, 0.45f);
      }
    }
  }

  step_particles(sc, k, dt);
  return !s.parts.empty() || s.stage != Stage::Idle || s.drop > 0.0f || marker_active(sc) || washing;
}

bool render(const Scene& sc, XrCompositionLayerProjection* layer) {
  const bool marker = marker_active(sc);
  const bool washing = wash_active();
  if (!s.ready || (s.parts.empty() && !marker && !washing)) return false;
  Slot& slot = s.slots[s.slot];
  s.slot = (s.slot + 1) % kSlots;
  if (slot.pending) {
    vkWaitForFences(s.dev.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(s.dev.device, 1, &slot.fence);
    slot.pending = false;
  }

  // Instances, grouped: the 3D pieces mesh by mesh, then the quads (sparks, rockets, the goal
  // flash), then the marker and the screen flash, for which two slots stay free.
  // Buckets: full-detail meshes, reduced meshes (pieces far away or lying still), then quads.
  constexpr uint32_t kQuadBucket = 2 * mesh::kCount;
  const XrVector3f head = head_of(sc);
  const float farSq = sc.mr ? 1.1f * 1.1f : 4.0f * 4.0f;
  auto bucket_of = [&](const Particle& q) -> uint32_t {
    if ((q.kind != kKindPaper && q.kind != kKindPiece) || q.mesh >= mesh::kCount) return kQuadBucket;
    if (q.kind == kKindPaper) return q.mesh;  // a few triangles either way
    const float dx = q.p[0] - head.x, dy = q.p[1] - head.y, dz = q.p[2] - head.z;
    const bool far = q.landed || dx * dx + dy * dy + dz * dz > farSq;
    return q.mesh + (far ? mesh::kCount : 0u);
  };
  uint32_t first[kQuadBucket + 1] = {}, filled[kQuadBucket + 1] = {};
  uint32_t count = 0;
  for (const Particle& q : s.parts) {
    if (count >= kMaxParticles - 2) break;
    ++filled[bucket_of(q)];
    ++count;
  }
  for (uint32_t b = 0, at = 0; b <= kQuadBucket; ++b) {
    first[b] = at;
    at += filled[b];
    filled[b] = 0;
  }
  auto* inst = static_cast<Instance*>(slot.instanceMapped);
  uint32_t written = 0;
  for (const Particle& q : s.parts) {
    if (written++ >= count) break;
    float alpha = 1.0f;
    if (q.kind == kKindSpark) {
      const float t = q.age / q.life;
      alpha = 1.0f - t * t;
    } else if (q.landed && q.age > q.life - 1.0f) {
      alpha = std::max(0.0f, q.life - q.age);
    }
    const uint32_t b = bucket_of(q);
    Instance& o = inst[first[b] + filled[b]++];
    o.posSize[0] = q.p[0];
    o.posSize[1] = q.p[1];
    o.posSize[2] = q.p[2];
    o.posSize[3] = q.size;
    std::memcpy(o.quat, q.q, sizeof(o.quat));
    o.color1 = q.c1;
    o.color2 = q.c2;
    o.params[0] = static_cast<float>(q.shape);
    o.params[1] = q.aspect;
    o.params[2] = alpha;
    o.params[3] = q.glow;
  }
  // The player marker, after the particles: an arrow bobbing over his head, scaled with the table
  // (but never too small to read). r24 also had a ring at his feet; Lance: the arrow suffices.
  uint32_t markerCount = 0;
  if (marker) {
    const float scale = sc.tableScale;
    const float bob = 0.25f * std::sin(s.markerTime * 5.0f);
    const XrVector3f above = game_to_room(scale, sc.tableYaw, sc.tablePos, s.markerGame[0], s.markerGame[1],
                                          s.markerGame[2] + 3.3f * sc.charScale + bob);
    const uint32_t colour = pack(s.markerColour);
    Instance& arrow = inst[count + markerCount++];
    arrow.posSize[0] = above.x;
    arrow.posSize[1] = above.y;
    arrow.posSize[2] = above.z;
    arrow.posSize[3] = std::max(0.03f, 1.7f * scale);
    std::memset(arrow.quat, 0, sizeof(arrow.quat));  // faces you from any side
    arrow.color1 = colour;
    arrow.color2 = colour;
    arrow.params[0] = static_cast<float>(kMarkerArrow);
    arrow.params[1] = 1.0f;
    arrow.params[2] = 1.0f;
    arrow.params[3] = 0.0f;
  }
  // The game's screen flash, last, over everything in your view.
  if (washing) {
    Instance& wash = inst[count + markerCount++];
    std::memset(&wash, 0, sizeof(wash));
    auto byte = [](float c) { return static_cast<uint32_t>(std::min(std::max(c, 0.0f), 1.0f) * 255.0f + 0.5f); };
    wash.color1 = pack((byte(s.washRgb[0]) << 16) | (byte(s.washRgb[1]) << 8) | byte(s.washRgb[2]));
    wash.color2 = wash.color1;
    wash.params[0] = static_cast<float>(kWash);
    wash.params[1] = 1.0f;
    wash.params[2] = s.washAlpha;
  }
  uint32_t indices = 0;
  for (uint32_t b = 0; b < kQuadBucket; ++b) indices += filled[b] * s.meshRanges[b / mesh::kCount][b % mesh::kCount].indexCount;
  s.statPeakParts = std::max(s.statPeakParts, count);
  s.statPeakIndices = std::max(s.statPeakIndices, indices);
  const uint32_t occluderVerts = write_occluders(sc, static_cast<float*>(slot.occluderMapped));

  uint32_t index[2] = {0, 0};
  for (int e = 0; e < 2; ++e) {
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!xr_ok(xrAcquireSwapchainImage(s.eyes[e].swapchain, &acquire, &index[e]), "xrAcquireSwapchainImage(fx)")) {
      return false;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    xr_ok(xrWaitSwapchainImage(s.eyes[e].swapchain, &wait), "xrWaitSwapchainImage(fx)");
  }

  vkResetCommandBuffer(slot.cmd, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(slot.cmd, &begin);
  for (int e = 0; e < 2; ++e) {
    VkClearValue clears[2];
    clears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = s.renderPass;
    rp.framebuffer = s.eyes[e].framebuffers[index[e]];
    rp.renderArea = {{0, 0}, {s.dev.width, s.dev.height}};
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(slot.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(s.dev.width), static_cast<float>(s.dev.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {s.dev.width, s.dev.height}};
    vkCmdSetViewport(slot.cmd, 0, 1, &vp);
    vkCmdSetScissor(slot.cmd, 0, 1, &scissor);

    PushConstants pc{};
    float proj[16], view[16];
    projection(sc.views[e].fov, 0.05f, 600.0f, proj);
    view_matrix(sc.views[e].pose, view);
    mat_mul(proj, view, pc.viewProj);
    const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0};
    rotate(sc.views[e].pose.orientation, ex, pc.camRight);
    rotate(sc.views[e].pose.orientation, ey, pc.camUp);
    pc.camPos[0] = sc.views[e].pose.position.x;
    pc.camPos[1] = sc.views[e].pose.position.y;
    pc.camPos[2] = sc.views[e].pose.position.z;
    const VkDeviceSize zero = 0;
    if (occluderVerts > 0) {
      vkCmdBindPipeline(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.occluderPipeline);
      vkCmdPushConstants(slot.cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
      vkCmdBindVertexBuffers(slot.cmd, 0, 1, &slot.occluders, &zero);
      vkCmdDraw(slot.cmd, occluderVerts, 1, 0, 0);
    }
    // The 3D pieces first (solid, depth written), then the glowing quads among them.
    if (first[kQuadBucket] > 0) {
      vkCmdBindPipeline(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.meshPipeline);
      vkCmdPushConstants(slot.cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
      const VkBuffer buffers[2] = {s.meshVerts, slot.instances};
      const VkDeviceSize offsets[2] = {0, 0};
      vkCmdBindVertexBuffers(slot.cmd, 0, 2, buffers, offsets);
      vkCmdBindIndexBuffer(slot.cmd, s.meshIndices, 0, VK_INDEX_TYPE_UINT16);
      for (uint32_t b = 0; b < kQuadBucket; ++b) {
        const mesh::Range& range = s.meshRanges[b / mesh::kCount][b % mesh::kCount];
        if (filled[b] == 0 || range.indexCount == 0) continue;
        vkCmdDrawIndexed(slot.cmd, range.indexCount, filled[b], range.firstIndex, 0, first[b]);
      }
    }
    vkCmdBindPipeline(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.fxPipeline);
    vkCmdPushConstants(slot.cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
    vkCmdBindVertexBuffers(slot.cmd, 0, 1, &slot.instances, &zero);
    if (filled[kQuadBucket] > 0) {
      vkCmdDraw(slot.cmd, 4, filled[kQuadBucket], 0, first[kQuadBucket]);
    }
    if (markerCount > 0) {
      vkCmdBindPipeline(slot.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.markerPipeline);
      vkCmdPushConstants(slot.cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
      vkCmdDraw(slot.cmd, 4, markerCount, 0, count);
    }
    vkCmdEndRenderPass(slot.cmd);
  }
  // Self-test: copy the left eye out before it goes back to the runtime.
  const bool dumping = s.dumpPath[0] != '\0' &&
                       (s.dumpBuffer != VK_NULL_HANDLE ||
                        make_host_buffer(VkDeviceSize(s.dev.width) * s.dev.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         &s.dumpBuffer, &s.dumpMemory, &s.dumpMapped));
  if (dumping) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s.eyes[0].images[index[0]].image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {s.dev.width, s.dev.height, 1};
    vkCmdCopyImageToBuffer(slot.cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.dumpBuffer, 1, &region);
    VkImageMemoryBarrier back = b;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &back);
  }
  vkEndCommandBuffer(slot.cmd);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &slot.cmd;
  if (vk_ok(vkQueueSubmit(s.dev.queue, 1, &submit, slot.fence), "vkQueueSubmit(fx)")) slot.pending = true;
  if (dumping && slot.pending) {
    vkWaitForFences(s.dev.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    if (FILE* f = std::fopen(s.dumpPath, "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", s.dev.width, s.dev.height);
      const auto* px = static_cast<const uint8_t*>(s.dumpMapped);
      std::vector<uint8_t> row(size_t(s.dev.width) * 3);
      const bool bgra = s.colorFormat == VK_FORMAT_B8G8R8A8_SRGB;
      for (uint32_t y = 0; y < s.dev.height; ++y) {
        for (uint32_t x = 0; x < s.dev.width; ++x) {
          const uint8_t* p = px + (size_t(y) * s.dev.width + x) * 4;
          const uint8_t r = bgra ? p[2] : p[0], gch = p[1], bch = bgra ? p[0] : p[2], a = p[3];
          // Premultiplied over mid grey.
          row[x * 3 + 0] = static_cast<uint8_t>(std::min(255, r + (128 * (255 - a)) / 255));
          row[x * 3 + 1] = static_cast<uint8_t>(std::min(255, gch + (128 * (255 - a)) / 255));
          row[x * 3 + 2] = static_cast<uint8_t>(std::min(255, bch + (128 * (255 - a)) / 255));
        }
        std::fwrite(row.data(), 1, row.size(), f);
      }
      std::fclose(f);
      FXLOG("fx: dumped left eye (%zu pieces) to %s", s.parts.size(), s.dumpPath);
    }
    s.dumpPath[0] = '\0';
  }

  for (int e = 0; e < 2; ++e) {
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xr_ok(xrReleaseSwapchainImage(s.eyes[e].swapchain, &release), "xrReleaseSwapchainImage(fx)");
    s.layerViews[e] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    s.layerViews[e].pose = sc.views[e].pose;
    s.layerViews[e].fov = sc.views[e].fov;
    s.layerViews[e].subImage.swapchain = s.eyes[e].swapchain;
    s.layerViews[e].subImage.imageRect = {{0, 0}, {static_cast<int32_t>(s.dev.width), static_cast<int32_t>(s.dev.height)}};
    s.layerViews[e].subImage.imageArrayIndex = 0;
  }
  *layer = XrCompositionLayerProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  layer->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  layer->space = s.dev.local;
  layer->viewCount = 2;
  layer->views = s.layerViews;
  return true;
}

float jumbotron_drop() { return s.drop; }

bool take_stats(uint32_t* peakParts, uint32_t* peakIndices) {
  *peakParts = s.statPeakParts;
  *peakIndices = s.statPeakIndices;
  s.statPeakParts = s.statPeakIndices = 0;
  return *peakParts > 0;
}

void request_dump(const char* path) {
  std::snprintf(s.dumpPath, sizeof(s.dumpPath), "%s", path);
}

}  // namespace xrfx

#endif  // __ANDROID__ && STRIKERS_XR
