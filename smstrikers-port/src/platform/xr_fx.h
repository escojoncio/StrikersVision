// strikers-quest: goal celebration effects for the headset (xr_fx.cpp), driven by xr_quest.cpp.
//
// Confetti in the scorer's colours with their signature piece mixed in (small lit 3D meshes,
// xr_fx_mesh.h), cannons, fireworks, landing on the floor (and on the Tabletop field), and the
// game's own screen flashes over your whole view, drawn by a small renderer of its own on the OpenXR
// Vulkan device into a projection layer of its own, composited over everything else. It works the
// same in all four modes and at the display rate, whatever the game's frame rate. The Tabletop field
// box and the screen are drawn depth-only first, so pieces behind them stay hidden.
//
// Include after the Vulkan/OpenXR platform defines (VK_USE_PLATFORM_ANDROID_KHR,
// XR_USE_PLATFORM_ANDROID, XR_USE_GRAPHICS_API_VULKAN).
#pragma once

#include <vulkan/vulkan.h>
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>

namespace xrfx {

struct DeviceInfo {
  XrInstance instance = XR_NULL_HANDLE;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local = XR_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  const VkPhysicalDeviceMemoryProperties* memProps = nullptr;
  uint32_t width = 0, height = 0;  // per eye
};

// Where things are this display frame, in LOCAL space (meters, Y up).
struct Scene {
  float dt = 0.0f;                 // seconds since the last update
  XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  float floorY = -1.6f;
  bool mr = true;                  // passthrough (Living Room) or Stadium
  bool tabletop = false;
  // Tabletop field: the game's (x, y, z) pitch maps to the room as (x, z, -y) * scale, turned by
  // yaw about +Y, then moved to pos. The pitch is 41.2 x 24.2 game units, ground at z = 0.
  XrVector3f tablePos{0.0f, 0.0f, 0.0f};
  float tableYaw = 0.0f;
  float tableScale = 0.03f;
  float charScale = 1.0f;          // the players are drawn this much bigger (the marker rides higher)
  // The life-size stadium: same mapping with this scale and origin.
  float stadiumScale = 1.0f;
  XrVector3f stadiumOrigin{0.0f, 0.0f, 0.0f};
  // The screen quad, for occlusion.
  bool screenValid = false;
  XrPosef screenPose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
  float screenWidth = 0.0f, screenHeight = 0.0f;
};

bool init(const DeviceInfo& device);
void shutdown();

// team 0/1; character = eCharacterClass (0 Birdo .. 11 Yoshi, -1 unknown).
void goal(int team, int character, bool superStrike, bool hyper, bool ownGoal);
// 1 replay started, 2 replay played out, 3 skipped, 4 play live again.
void phase(int phase);

// Steps the effects. True while anything is alive or still to come.
bool update(const Scene& scene);
// Draws into the effects swapchains and fills `layer` (space and views). False if nothing to show.
bool render(const Scene& scene, XrCompositionLayerProjection* layer);

// The replay jumbotron, 0 = up and out of the way, 1 = all the way down (eased, may overshoot).
float jumbotron_drop();

// Cannon waves fired since the last goal (xr_quest.cpp buzzes the controllers on each new one).
int cannon_waves_fired();

// The Tabletop "that's you" marker: the controlled player's position in game units and his team's
// captain (eCharacterClass) for the colour. Drawn only while scene.tabletop.
void set_marker(bool shown, float x, float y, float z, int teamCaptain);

// The game's own full-screen flash (colour 0..1, opacity), reported every game frame it draws one:
// shown over your whole view while the reports keep coming.
void screen_flash(float r, float g, float b, float a);
// Self-test: dump the effects layer (xr_fx.ppm) when the next screen flash is at its height.
void dump_at_flash();

// For the stats line: the most pieces drawn in one frame, and mesh indices per eye, since the last
// call. False when nothing was drawn.
bool take_stats(uint32_t* peakParts, uint32_t* peakIndices);

// Self-test: the next rendered left eye of the effects layer is written to `path` as a PPM, the
// pieces over mid grey so the transparent parts read.
void request_dump(const char* path);

}  // namespace xrfx
