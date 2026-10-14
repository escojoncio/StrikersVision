// StrikersVision: the goal celebrations in the headset, Metal. A port of src/platform/xr_fx.cpp (the
// Quest renderer, Vulkan): the simulation below is that file's, unchanged (same themes, timings,
// phases, cannon waves, fireworks, pyro, streamers, foil, the replay jumbotron spring, the marker and
// the game's screen flashes); only the drawing is new. Quest drew the effects into a projection layer
// of their own; here they are encoded into the composite's render pass for each drawable view
// (sv_xr.mm), after the game's picture.
//
// Shaders: the MSL source in sv_shaders.h, compiled at run time (newLibraryWithSource) when init()
// runs; nothing goes into default.metallib, so the build needs no Metal compile step and a shader
// error lands in the game log with its line number.
//
// Depth: the pass's depth attachment is reverse-Z (cleared to 0, nearer = greater). Unless
// Scene::depthFromEyes says it already holds the real scene, the Tabletop field and the screen are
// first drawn depth-only as stand-ins so pieces behind them stay hidden.
//
// Self-test without a Mac: the live setting STRIKERS_XR_FX_TEST = "<n>:<commands>" (the app's match
// controls send it; a new <n> runs the commands once). Commands, separated by ';': "goal <class>
// [super] [hyper] [own]" (eCharacterClass 0 Birdo .. 11 Yoshi, -1 neutral), "replay", "end",
// "skip", "live", "state" (logs what is alive). They go through PortXrGoal /
// PortXrCelebrationPhase, the game's own path, so the controller buzz and the jumbotron follow.
//
// Built with ARC (-fobjc-arc).

#import <Metal/Metal.h>

#include "sv_xr.h"
#include "sv_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

#include "../../smstrikers-port/src/platform/xr_fx_mesh.h"

// The game's celebration hooks (include/port/xr_quest.h, implemented by sv_xr.mm), used by the
// self-test so it runs exactly what a real goal runs.
extern "C" void PortXrGoal(int team, int character, int superStrike, int hyper, int ownGoal, int humanPad);
extern "C" void PortXrCelebrationPhase(int phase);

namespace svfx {
namespace {

using xrfx::mesh::Vertex;
namespace mesh = xrfx::mesh;

// ---------------------------------------------------------------------------------------------
// Themes: the scorer's colours and signature piece
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

// Every goal is a big celebration.
constexpr uint32_t kMaxParticles = 16000;
constexpr uint32_t kMaxOccluderVerts = 64;

struct State {
  bool initTried = false;  // init() compiles shaders: never retried every frame after a failure
  bool ready = false;

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
  // your whole view instead of on the game picture. The game reports it every
  // frame it draws one; it is gone once the reports stop.
  float washRgb[3] = {1.0f, 1.0f, 1.0f};
  float washAlpha = 0.0f;
  float washAge = 1.0f;
  float washPeak = 0.0f;
  float nearAccum = 0.0f;      // pieces dropped close around you
  std::vector<Particle> parts;
  std::mt19937 rng{20261010u};

  // [0] full detail (pieces near you), [1] reduced (far away or lying on the floor).
  mesh::Range meshRanges[2][mesh::kCount];
  uint32_t statPeakParts = 0, statPeakIndices = 0;  // for the stats line (largest frame since)
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
V3 game_to_room(float scale, float yaw, const V3& origin, float gx, float gy, float gz) {
  const float a = gx, b = gz, c = -gy;
  const float cs = std::cos(yaw), sn = std::sin(yaw);
  return V3{(a * cs + c * sn) * scale + origin.x, b * scale + origin.y, (-a * sn + c * cs) * scale + origin.z};
}

V3 head_of(const Scene& sc) {
  return V3{0.5f * (sc.views[0].position.x + sc.views[1].position.x),
            0.5f * (sc.views[0].position.y + sc.views[1].position.y),
            0.5f * (sc.views[0].position.z + sc.views[1].position.z)};
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

void add_paper(const Scene& sc, const Scale& k, const V3& p, const float v[3], bool streamer = false) {
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

void add_piece(const Scene& sc, const Scale& k, const V3& p, const float v[3]) {
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

void add_spark(const V3& p, const float v[3], float size, uint32_t core, uint32_t outer, float life,
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
void add_burst(const Scale& k, const V3& p) {
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

void add_rocket(const Scale& k, const V3& from, float burstHeight, float speed) {
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
int launch_points(const Scene& sc, V3 out[4], V3 outward[4]) {
  const V3 head = head_of(sc);
  if (sc.tabletop && sc.mr) {
    for (int i = 0; i < 4; ++i) {
      const float gx = (i & 1) ? 21.0f : -21.0f, gy = (i & 2) ? 12.5f : -12.5f;
      out[i] = game_to_room(sc.tableScale, sc.tableYaw, sc.tablePos, gx, gy, 0.5f);
    }
  } else {
    // Where the left eye looks, flattened (the eye looks down its -Z).
    float fwd[3] = {-sc.views[0].back.x, 0.0f, -sc.views[0].back.z};
    const float len = std::max(1e-3f, std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]));
    fwd[0] /= len; fwd[2] /= len;
    const float right[3] = {-fwd[2], 0.0f, fwd[0]};
    const float side = sc.mr ? 0.9f : 4.5f, ahead = sc.mr ? 0.8f : 4.0f;
    const float y = sc.mr ? head.y - 0.6f : sc.floorY;
    for (int i = 0; i < 4; ++i) {
      const float sx = (i & 1) ? 1.0f : -1.0f, sz = (i & 2) ? -0.8f : 1.0f;
      out[i] = V3{head.x + right[0] * side * sx + fwd[0] * ahead * sz, y,
                          head.z + right[2] * side * sx + fwd[2] * ahead * sz};
    }
  }
  V3 c{0.0f, 0.0f, 0.0f};
  for (int i = 0; i < 4; ++i) { c.x += out[i].x * 0.25f; c.z += out[i].z * 0.25f; }
  for (int i = 0; i < 4; ++i) {
    const float dx = out[i].x - c.x, dz = out[i].z - c.z;
    const float len = std::max(1e-3f, std::sqrt(dx * dx + dz * dz));
    outward[i] = V3{dx / len, 0.0f, dz / len};
  }
  return 4;
}

void fire_cannon_wave(const Scene& sc, const Scale& k) {
  V3 from[4], out[4];
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
    const V3 head = head_of(sc);
    const float ceiling = std::max(sc.floorY + 2.3f, head.y + 0.45f);
    V3 from[4], out[4];
    launch_points(sc, from, out);
    add_rocket(k, from[irand(4)], ceiling - frand(0.2f, 0.55f), 2.4f);
    return;
  }
  // Over the stands, all around the stadium.
  const float a = frand(0.0f, 6.2831853f), r = frand(38.0f, 55.0f);
  const V3 from =
      game_to_room(sc.stadiumScale, 0.0f, sc.stadiumOrigin, std::cos(a) * r * 1.2f, std::sin(a) * r * 0.8f, 8.0f);
  add_rocket(k, from, sc.floorY + frand(26.0f, 46.0f), 26.0f);
}

// Pyro: columns of fire shooting up, at the field's corners in the room, along the stands' front in
// the stadium.
void fire_jets(const Scene& sc, const Scale& k, float dt) {
  const float rate = sc.mr ? 220.0f : 900.0f;  // sparks per second, all jets
  s.jetAccum += rate * dt;
  V3 from[8];
  int n = 0;
  if (sc.mr) {
    V3 out[4];
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
    const V3& p = from[irand(n)];
    const float sp = k.jetSpeed;
    const float v[3] = {frand(-0.08f, 0.08f) * sp, sp * frand(0.8f, 1.1f), frand(-0.08f, 0.08f) * sp};
    const uint32_t outer = irand(3) == 0 ? 0xFF3A10 : 0xFF8A1A;
    add_spark(p, v, k.jetSpark * frand(0.8f, 1.3f), 0xFFE9A0, outer, frand(0.5f, 0.8f), 1.0f, 0.6f);
  }
}

// The goal flash: a wash of the scorer's colour over the whole view, fading fast.
void add_flash(const Scene& sc) {
  if (room_full()) return;
  const V3 head = head_of(sc);
  const float fwd[3] = {-sc.views[0].back.x, -sc.views[0].back.y, -sc.views[0].back.z};
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
  const V3 head = head_of(sc);
  size_t w = 0;
  for (size_t i = 0; i < s.parts.size(); ++i) {
    Particle& q = s.parts[i];
    q.age += dt;
    bool keep = true;
    if (q.kind == kKindFlash) {
      // Stays in front of you while it fades.
      q.p[0] = head.x - 0.3f * sc.views[0].back.x;
      q.p[1] = head.y - 0.3f * sc.views[0].back.y;
      q.p[2] = head.z - 0.3f * sc.views[0].back.z;
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
        add_spark(V3{q.p[0], q.p[1], q.p[2]}, tv, k.spark * 0.6f, 0xFFE0A0, 0xFF8A1A, 0.4f, 1.5f, 0.3f);
      }
      if (q.p[1] >= q.burstHeight || q.age > 3.0f) {
        add_burst(k, V3{q.p[0], q.p[1], q.p[2]});
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
// Metal
// ---------------------------------------------------------------------------------------------

// Same layout as xr_fx.vert's push constants (and the shaders' FxPC).
struct PushConstants {
  float viewProj[16];
  float camRight[4];
  float camUp[4];
  float camPos[4];
};

constexpr int kFrames = 3;  // per-frame instance buffers in flight

struct FrameBuffers {
  id<MTLBuffer> instances;
  id<MTLBuffer> occluders;
};

struct Gpu {
  id<MTLDevice> device;
  id<MTLRenderPipelineState> fxPipeline;    // quads: premultiplied blend
  id<MTLRenderPipelineState> meshPipeline;  // 3D pieces: premultiplied blend, depth written
  id<MTLRenderPipelineState> occluderPipeline;
  id<MTLDepthStencilState> testNoWrite;     // quads
  id<MTLDepthStencilState> testWrite;       // meshes, occluders
  id<MTLDepthStencilState> always;          // marker, screen flash
  id<MTLBuffer> meshVerts;
  id<MTLBuffer> meshIndices;
  FrameBuffers frames[kFrames];
  dispatch_semaphore_t inFlight;
  uint32_t format = 0;         // the colour format the pipelines were built for
  int frame = 0;
  bool prepared = false;       // this frame's buffers are filled (prepare), encode may run
  bool holding = false;        // frames[frame] is taken from inFlight and not yet handed to a
                               // command buffer (frame_committed); the next prepare reuses it
  // What prepare() laid out for encode().
  uint32_t first[2 * mesh::kCount + 1] = {};
  uint32_t filled[2 * mesh::kCount + 1] = {};
  uint32_t count = 0;
  uint32_t markerCount = 0;
  uint32_t occluderVerts = 0;
};
Gpu g;

// Column-major helpers.
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

void rotate(const Quat& q, const float v[3], float out[3]) {
  const float t[3] = {2 * (q.y * v[2] - q.z * v[1]), 2 * (q.z * v[0] - q.x * v[2]), 2 * (q.x * v[1] - q.y * v[0])};
  out[0] = v[0] + q.w * t[0] + (q.y * t[2] - q.z * t[1]);
  out[1] = v[1] + q.w * t[1] + (q.z * t[0] - q.x * t[2]);
  out[2] = v[2] + q.w * t[2] + (q.x * t[1] - q.y * t[0]);
}

// The stand-ins (depth only). The Tabletop field: walls up to the fence top, the lid at the pitch, so
// pieces that land on the field stay visible (xr_fx.cpp's closed box hid them under its lid). The
// screen: a flat quad (a curved screen bends away from you, so the flat chord never hides more).
uint32_t write_occluders(const Scene& sc, bool table, float* out) {
  uint32_t n = 0;
  auto tri = [&](const V3& a, const V3& b, const V3& c) {
    if (n + 3 > kMaxOccluderVerts) return;
    for (const V3* v : {&a, &b, &c}) {
      out[n * 3 + 0] = v->x;
      out[n * 3 + 1] = v->y;
      out[n * 3 + 2] = v->z;
      ++n;
    }
  };
  if (table && sc.tabletop) {
    V3 c[8];
    for (int i = 0; i < 8; ++i) {
      c[i] = game_to_room(sc.tableScale, sc.tableYaw, sc.tablePos, (i & 1) ? 22.5f : -22.5f, (i & 2) ? 13.5f : -13.5f,
                          (i & 4) ? 2.0f : -0.2f);
    }
    // Bottom, the four walls; then the lid at the pitch (z = 0).
    const int faces[5][4] = {{0, 1, 3, 2}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    for (const auto& f : faces) {
      tri(c[f[0]], c[f[1]], c[f[2]]);
      tri(c[f[0]], c[f[2]], c[f[3]]);
    }
    V3 lid[4];
    for (int i = 0; i < 4; ++i) {
      lid[i] = game_to_room(sc.tableScale, sc.tableYaw, sc.tablePos, (i & 1) ? 22.5f : -22.5f,
                            (i & 2) ? 13.5f : -13.5f, 0.0f);
    }
    tri(lid[0], lid[1], lid[3]);
    tri(lid[0], lid[3], lid[2]);
  }
  if (sc.screenValid && sc.screenWidth > 0.0f && sc.screenHeight > 0.0f) {
    const float hw = 0.5f * sc.screenWidth, hh = 0.5f * sc.screenHeight;
    const float corners[4][3] = {{-hw, -hh, 0.0f}, {hw, -hh, 0.0f}, {hw, hh, 0.0f}, {-hw, hh, 0.0f}};
    V3 w[4];
    for (int i = 0; i < 4; ++i) {
      float r[3];
      rotate(sc.screenOrientation, corners[i], r);
      w[i] = V3{r[0] + sc.screenPos.x, r[1] + sc.screenPos.y, r[2] + sc.screenPos.z};
    }
    tri(w[0], w[1], w[2]);
    tri(w[0], w[2], w[3]);
  }
  return n;
}

id<MTLRenderPipelineState> make_pipeline(id<MTLLibrary> lib, NSString* vs, NSString* fs, MTLPixelFormat color,
                                         bool blend, bool writeColor, const char* what) {
  MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
  d.vertexFunction = [lib newFunctionWithName:vs];
  d.fragmentFunction = [lib newFunctionWithName:fs];
  if (d.vertexFunction == nil || d.fragmentFunction == nil) {
    SVERR("fx: shader function missing for %s", what);
    return nil;
  }
  d.colorAttachments[0].pixelFormat = color;
  d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
  if (!writeColor) d.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
  if (blend) {
    // Premultiplied alpha, as xr_fx.cpp's pipelines.
    d.colorAttachments[0].blendingEnabled = YES;
    d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    d.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    d.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
  }
  NSError* err = nil;
  id<MTLRenderPipelineState> p = [g.device newRenderPipelineStateWithDescriptor:d error:&err];
  if (p == nil) SVERR("fx: pipeline %s failed: %s", what, err.localizedDescription.UTF8String);
  return p;
}

id<MTLDepthStencilState> make_depth(MTLCompareFunction f, bool write) {
  MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
  d.depthCompareFunction = f;
  d.depthWriteEnabled = write ? YES : NO;
  return [g.device newDepthStencilStateWithDescriptor:d];
}

// The pieces' meshes at two levels of detail, one vertex and one index buffer.
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
    SVERR("fx: piece meshes too large (%zu vertices)", set.verts.size());
    return false;
  }
  g.meshVerts = [g.device newBufferWithBytes:set.verts.data()
                                      length:sizeof(Vertex) * set.verts.size()
                                     options:MTLResourceStorageModeShared];
  g.meshIndices = [g.device newBufferWithBytes:set.indices.data()
                                        length:sizeof(uint16_t) * set.indices.size()
                                       options:MTLResourceStorageModeShared];
  if (g.meshVerts == nil || g.meshIndices == nil) return false;
  SVLOG("fx: piece meshes ready (%zu vertices, %zu triangles)", set.verts.size(), set.indices.size() / 3);
  return true;
}

void fill_pc(const Scene& sc, int view, PushConstants* pc) {
  const EyeView& e = sc.views[view];
  std::memcpy(pc->viewProj, e.viewProj, sizeof(pc->viewProj));
  pc->camRight[0] = e.right.x; pc->camRight[1] = e.right.y; pc->camRight[2] = e.right.z; pc->camRight[3] = 0.0f;
  pc->camUp[0] = e.up.x; pc->camUp[1] = e.up.y; pc->camUp[2] = e.up.z; pc->camUp[3] = 0.0f;
  pc->camPos[0] = e.position.x; pc->camPos[1] = e.position.y; pc->camPos[2] = e.position.z; pc->camPos[3] = 1.0f;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------------------------

// Waits until the GPU has finished with every frame's buffers.
static void drain() {
  if (g.inFlight == nil) return;
  if (g.holding) {
    // A slot taken by prepare() but never handed to a command buffer: give it back.
    dispatch_semaphore_signal(g.inFlight);
    g.holding = false;
  }
  for (int i = 0; i < kFrames; ++i) dispatch_semaphore_wait(g.inFlight, DISPATCH_TIME_FOREVER);
  for (int i = 0; i < kFrames; ++i) dispatch_semaphore_signal(g.inFlight);
}

bool init(void* mtlDevice, uint32_t colorPixelFormat) {
  id<MTLDevice> device = (__bridge id<MTLDevice>)mtlDevice;
  if (device == nil) return false;
  if (s.initTried && g.device == device && g.format == colorPixelFormat) return s.ready;
  if (s.initTried) {
    // A new renderer with another device or colour format (the immersive space reopened): rebuild
    // the GPU side, keep the simulation.
    SVLOG("fx: renderer changed (format %u -> %u), rebuilding the pipelines", g.format, colorPixelFormat);
    drain();
    g = Gpu{};
    s.ready = false;
  }
  s.initTried = true;
  g.device = device;
  g.format = colorPixelFormat;
  NSError* err = nil;
  id<MTLLibrary> lib = [g.device newLibraryWithSource:[NSString stringWithUTF8String:kSvShadersMsl]
                                              options:[MTLCompileOptions new]
                                                error:&err];
  if (lib == nil) {
    SVERR("fx: shader library failed to compile: %s", err.localizedDescription.UTF8String);
    return false;
  }
  const MTLPixelFormat color = static_cast<MTLPixelFormat>(colorPixelFormat);
  g.fxPipeline = make_pipeline(lib, @"fx_vertex", @"fx_fragment", color, true, true, "fx");
  g.meshPipeline = make_pipeline(lib, @"mesh_vertex", @"mesh_fragment", color, true, true, "mesh");
  g.occluderPipeline = make_pipeline(lib, @"occluder_vertex", @"occluder_fragment", color, false, false, "occluder");
  // Reverse-Z (the drawable's projection): nearer is greater.
  g.testNoWrite = make_depth(MTLCompareFunctionGreaterEqual, false);
  g.testWrite = make_depth(MTLCompareFunctionGreaterEqual, true);
  g.always = make_depth(MTLCompareFunctionAlways, false);
  if (g.fxPipeline == nil || g.meshPipeline == nil || g.occluderPipeline == nil || !make_meshes()) {
    SVERR("fx: celebration renderer failed to start; goals will not show effects");
    return false;
  }
  for (FrameBuffers& f : g.frames) {
    f.instances = [g.device newBufferWithLength:sizeof(Instance) * kMaxParticles options:MTLResourceStorageModeShared];
    f.occluders = [g.device newBufferWithLength:sizeof(float) * 3 * kMaxOccluderVerts
                                        options:MTLResourceStorageModeShared];
    if (f.instances == nil || f.occluders == nil) {
      SVERR("fx: out of memory for the instance buffers; goals will not show effects");
      return false;
    }
  }
  g.inFlight = dispatch_semaphore_create(kFrames);
  s.parts.reserve(kMaxParticles);
  s.ready = true;
  SVLOG("fx: celebration renderer ready (colour format %u)", colorPixelFormat);
  return true;
}

void shutdown() {
  drain();  // every frame in flight finishes before the buffers go
  g = Gpu{};
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
  SVLOG("fx: goal for team %d, %s theme%s%s", team,
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
    SVLOG("fx: game screen flash (%.2f %.2f %.2f) -> whole view", r, g, b);
    s.washPeak = 0.0f;
  }
  s.washRgb[0] = r;
  s.washRgb[1] = g;
  s.washRgb[2] = b;
  // A touch under full: a solid white field right at the eyes is harsher than on a TV.
  s.washAlpha = std::min(std::max(a, 0.0f), 1.0f) * 0.9f;
  s.washAge = 0.0f;
  s.washPeak = std::max(s.washPeak, s.washAlpha);
}

// ---- Self-test (see the top of the file) ----

static std::string g_testSeen;  // the last STRIKERS_XR_FX_TEST value run

static void log_state() {
  int papers = 0, pieces = 0, sparks = 0, rockets = 0, landed = 0;
  for (const Particle& q : s.parts) {
    if (q.kind == kKindPaper) ++papers;
    else if (q.kind == kKindPiece) ++pieces;
    else if (q.kind == kKindSpark) ++sparks;
    else if (q.kind == kKindRocket) ++rockets;
    if (q.landed) ++landed;
  }
  static const char* const kStage[] = {"idle", "raining", "tapering"};
  SVLOG("fx: state %s %.1fs: %zu alive (paper %d, pieces %d, sparks %d, rockets %d, landed %d), waves %d+%d, "
        "rockets %d to go, pyro %.1fs, jumbotron %.2f -> %.0f, ready %d",
        kStage[static_cast<int>(s.stage)], s.stageTime, s.parts.size(), papers, pieces, sparks, rockets, landed,
        s.cannonWave, s.cannonWavesLeft, s.pendingRockets, std::max(0.0f, s.jetTime), s.drop, s.dropTarget,
        s.ready ? 1 : 0);
}

static void run_self_test() {
  const char* v = std::getenv("STRIKERS_XR_FX_TEST");
  if (v == nullptr || *v == '\0' || g_testSeen == v) return;
  g_testSeen = v;
  const char* cmds = std::strchr(v, ':');
  cmds = cmds != nullptr ? cmds + 1 : v;
  SVLOG("fx: self-test \"%s\"", cmds);
  std::string all(cmds);
  size_t at = 0;
  while (at <= all.size()) {
    size_t end = all.find(';', at);
    if (end == std::string::npos) end = all.size();
    std::string c = all.substr(at, end - at);
    at = end + 1;
    while (!c.empty() && c.front() == ' ') c.erase(c.begin());
    if (c.rfind("goal", 0) == 0) {
      const int who = std::atoi(c.c_str() + 4);
      const bool own = c.find("own") != std::string::npos;
      PortXrGoal(own ? 1 : 0, who, c.find("super") != std::string::npos ? 1 : 0,
                 c.find("hyper") != std::string::npos ? 1 : 0, own ? 1 : 0, -1);
    } else if (c.rfind("replay", 0) == 0) {
      PortXrCelebrationPhase(1);
    } else if (c.rfind("end", 0) == 0) {
      PortXrCelebrationPhase(2);
    } else if (c.rfind("skip", 0) == 0) {
      PortXrCelebrationPhase(3);
    } else if (c.rfind("live", 0) == 0) {
      PortXrCelebrationPhase(4);
    } else if (c.rfind("state", 0) == 0) {
      log_state();
    } else if (!c.empty()) {
      SVWARN("fx: self-test: unknown command \"%s\"", c.c_str());
    }
  }
}

bool update(const Scene& sc) {
  run_self_test();
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
  const V3 head = head_of(sc);
  V3 center = head;
  if (sc.tabletop && sc.mr) {
    center.x = 0.5f * (head.x + sc.tablePos.x);
    center.z = 0.5f * (head.z + sc.tablePos.z);
  }
  const float top = sc.mr ? std::max(sc.floorY + 2.3f, head.y + 0.45f) : sc.floorY + k.rainHeight;
  while (s.rainAccum >= 1.0f) {
    s.rainAccum -= 1.0f;
    const float a = frand(0.0f, 6.2831853f), r = k.rainRadius * std::sqrt(frand(0.0f, 1.0f));
    const V3 p{center.x + std::cos(a) * r, top + frand(0.0f, sc.mr ? 0.2f : 2.0f), center.z + std::sin(a) * r};
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
      const V3 p{head.x + std::cos(a) * r, top + frand(0.0f, sc.mr ? 0.2f : 1.0f), head.z + std::sin(a) * r};
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

bool prepare(const Scene& sc) {
  g.prepared = false;
  const bool marker = marker_active(sc);
  const bool washing = wash_active();
  if (!s.ready || (s.parts.empty() && !marker && !washing)) return false;
  // One buffer set per command buffer in flight; waits only when the GPU is three frames behind. A
  // set taken earlier and not yet handed to a command buffer is simply refilled (nothing reads it).
  if (!g.holding) {
    dispatch_semaphore_wait(g.inFlight, DISPATCH_TIME_FOREVER);
    g.holding = true;
    g.frame = (g.frame + 1) % kFrames;
  }
  FrameBuffers& fb = g.frames[g.frame];

  // Instances, grouped: the 3D pieces mesh by mesh, then the quads (sparks, rockets, the goal
  // flash), then the marker and the screen flash, for which two slots stay free.
  // Buckets: full-detail meshes, reduced meshes (pieces far away or lying still), then quads.
  constexpr uint32_t kQuadBucket = 2 * mesh::kCount;
  const V3 head = head_of(sc);
  const float farSq = sc.mr ? 1.1f * 1.1f : 4.0f * 4.0f;
  auto bucket_of = [&](const Particle& q) -> uint32_t {
    if ((q.kind != kKindPaper && q.kind != kKindPiece) || q.mesh >= mesh::kCount) return kQuadBucket;
    if (q.kind == kKindPaper) return q.mesh;  // a few triangles either way
    const float dx = q.p[0] - head.x, dy = q.p[1] - head.y, dz = q.p[2] - head.z;
    const bool far = q.landed || dx * dx + dy * dy + dz * dz > farSq;
    return q.mesh + (far ? mesh::kCount : 0u);
  };
  uint32_t* first = g.first;
  uint32_t* filled = g.filled;
  std::memset(g.first, 0, sizeof(g.first));
  std::memset(g.filled, 0, sizeof(g.filled));
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
  auto* inst = static_cast<Instance*>(fb.instances.contents);
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
  // (but never too small to read).
  uint32_t markerCount = 0;
  if (marker) {
    const float scale = sc.tableScale;
    const float bob = 0.25f * std::sin(s.markerTime * 5.0f);
    const V3 above = game_to_room(scale, sc.tableYaw, sc.tablePos, s.markerGame[0], s.markerGame[1],
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
  for (uint32_t b = 0; b < kQuadBucket; ++b) {
    indices += filled[b] * s.meshRanges[b / mesh::kCount][b % mesh::kCount].indexCount;
  }
  s.statPeakParts = std::max(s.statPeakParts, count);
  s.statPeakIndices = std::max(s.statPeakIndices, indices);
  g.count = count;
  g.markerCount = markerCount;
  // With the real 3D scene already in the depth buffer the table box would only be a coarser copy.
  g.occluderVerts = write_occluders(sc, !sc.depthFromEyes, static_cast<float*>(fb.occluders.contents));
  g.prepared = true;
  return true;
}

void encode(void* renderEncoder, const Scene& sc, int view) {
  if (!g.prepared || view < 0 || view > 1) return;
  id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;
  FrameBuffers& fb = g.frames[g.frame];
  constexpr uint32_t kQuadBucket = 2 * mesh::kCount;
  PushConstants pc{};
  fill_pc(sc, view, &pc);
  [enc pushDebugGroup:@"celebration"];
  [enc setCullMode:MTLCullModeNone];
  if (g.occluderVerts > 0) {
    [enc setRenderPipelineState:g.occluderPipeline];
    [enc setDepthStencilState:g.testWrite];
    [enc setVertexBuffer:fb.occluders offset:0 atIndex:0];
    [enc setVertexBytes:&pc length:sizeof(pc) atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:g.occluderVerts];
  }
  // The 3D pieces first (solid, depth written), then the glowing quads among them.
  if (g.first[kQuadBucket] > 0) {
    [enc setRenderPipelineState:g.meshPipeline];
    [enc setDepthStencilState:g.testWrite];
    [enc setVertexBuffer:g.meshVerts offset:0 atIndex:0];
    [enc setVertexBytes:&pc length:sizeof(pc) atIndex:2];
    for (uint32_t b = 0; b < kQuadBucket; ++b) {
      const mesh::Range& range = s.meshRanges[b / mesh::kCount][b % mesh::kCount];
      if (g.filled[b] == 0 || range.indexCount == 0) continue;
      // The instance buffer's offset picks the bucket's first instance ([[instance_id]] starts at 0).
      [enc setVertexBuffer:fb.instances offset:sizeof(Instance) * g.first[b] atIndex:1];
      [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:range.indexCount
                       indexType:MTLIndexTypeUInt16
                     indexBuffer:g.meshIndices
               indexBufferOffset:sizeof(uint16_t) * range.firstIndex
                   instanceCount:g.filled[b]];
    }
  }
  [enc setRenderPipelineState:g.fxPipeline];
  [enc setVertexBytes:&pc length:sizeof(pc) atIndex:1];
  if (g.filled[kQuadBucket] > 0) {
    [enc setDepthStencilState:g.testNoWrite];
    [enc setVertexBuffer:fb.instances offset:sizeof(Instance) * g.first[kQuadBucket] atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:g.filled[kQuadBucket]];
  }
  if (g.markerCount > 0) {
    // The marker and the screen flash draw over everything: they must read behind other players.
    [enc setDepthStencilState:g.always];
    [enc setVertexBuffer:fb.instances offset:sizeof(Instance) * g.count atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:g.markerCount];
  }
  [enc popDebugGroup];
}

void frame_committed(void* commandBuffer) {
  g.prepared = false;
  if (!g.holding || commandBuffer == nullptr) return;
  g.holding = false;
  id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)commandBuffer;
  dispatch_semaphore_t sem = g.inFlight;
  [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
    dispatch_semaphore_signal(sem);
  }];
}

float jumbotron_drop() { return s.drop; }

bool take_stats(uint32_t* peakParts, uint32_t* peakIndices) {
  *peakParts = s.statPeakParts;
  *peakIndices = s.statPeakIndices;
  s.statPeakParts = s.statPeakIndices = 0;
  return *peakParts > 0;
}

}  // namespace svfx
