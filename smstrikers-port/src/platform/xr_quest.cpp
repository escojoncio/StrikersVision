// See include/port/xr_quest.h.
//
// Built as its own object library (strikers_xr in CMakeLists.txt) rather than with the rest of
// src/platform: it is C++20 and must not get the port prelude, which the Vulkan and OpenXR headers
// were never written to sit under.
//
// Two picture paths:
//  - The flat game frame (Jumbotron, and the scoreboard over the Tabletop): Aurora reads the
//    presented frame back to the CPU every game frame (aurora_xr_set_frame_callback), and this file
//    uploads it into a quad-layer swapchain on its own Vulkan device.
//  - The 3D modes (Tabletop, Stadium): glPlat.cpp re-sends the frame's render lists once per eye
//    with the head's matrices (PortXrStereo*), into opaque-fd VkImages this device exports and
//    Dawn imports (create_pass_external); a SyncFD fence from Dawn gates a blit into the eye
//    swapchains (ref/TPVR-XR-EXTRACTION.md §3).

#include "../../include/port/xr_quest.h"

#if defined(__ANDROID__) && defined(STRIKERS_XR)

#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>

#include <jni.h>
#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_VULKAN 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <android/log.h>
#include <time.h>
#include <unistd.h>

#include "xr_fx.h"  // goal celebration effects
#include "xr_net.hpp" // online 1v1 (include/port/xr_net.h)
#include "../../include/port/xr_net.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <dirent.h>
#include <cstdarg>
#include <cstdio>

#include <SDL3/SDL_system.h>
#include <aurora/aurora.h>
#include <aurora/gfx.hpp>
#include "dolphin/pad.h"
#include "../../extern/aurora/lib/webgpu/gpu.hpp"

// src/platform/audio_out.cpp, for the underrun count in the stats line.
extern "C" void PortAudioStats(unsigned long* outBuffers, unsigned long* outUnderruns, int* outEverNonSilent);
// extern/aurora/lib/gfx/encoding.cpp: per-frame peak and reserved size of the fixed staging pools
// (vertex, uniform, index, storage, texture upload). The eye re-sends multiply the first three.
extern "C" void aurora_gfx_pool_stats(uint32_t* peakBytes, uint32_t* reservedBytes, size_t count);
// extern/aurora/lib/gfx/recording.cpp: bytes used so far this frame in those first four pools.
extern "C" void aurora_gfx_pool_usage(uint32_t* usedBytes, size_t count);
// Where Aurora's threads waited since the last call (extern/aurora/lib/xr_waits.hpp order).
extern "C" void aurora_xr_take_waits(uint64_t* outNs, int count);
// Decode-once vertex path (extern/aurora/lib/gx/vertex_decode.hpp; dolphin/gx/GXAurora.h).
extern "C" void aurora_xr_set_vertex_decode(int mode);
extern "C" void aurora_xr_take_decode_stats(uint32_t out[6]);
extern "C" unsigned long long aurora_xr_decoded_bytes(void);
extern "C" unsigned long long aurora_xr_decoded_bytes_last_frame(void);
// extern/aurora/lib/gx/gx.cpp: alpha = coverage for the eye passes over passthrough.
extern "C" void aurora_gx_set_xr_coverage(bool enabled);
// extern/aurora/lib/gfx/texture.cpp: anisotropic filtering for every mipmapped texture.
extern "C" void aurora_xr_set_texture_anisotropy(unsigned int n);
// src/platform/xr_layers.cpp: stadium objects the per-view culling left out since the last call,
// and the self-test switches that leave categories out of the eye sends.
unsigned long PortXrLayersTakeCulled();
void PortXrTuneSkip(int crowd, int stands, int stadiumField);
void PortXrProbe();
void PortXrSetStadiumEconomy(float minPx, float eyeHalfWidthPx, int skipTallField, int bothLayers);
// Below: starts the celebration renderer when first needed.
static void ensure_fx();

namespace {

constexpr const char* kTag = "strikers_xr";
// Bumped every headset round; the first log line of a session says which build is running.
constexpr const char* kBuildTag = "STRIKERS_XR build=r39b";

// r39: every line also goes to the game log (stderr -> /sdcard/Strikers/strikers-log.txt): the
// headset's system log turns over within minutes, so a session's stats and errors were lost.
void xr_file_log(const char* level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void xr_file_log(const char* level, const char* fmt, ...) {
  char line[768];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  std::fprintf(stderr, "[xr%s] %s\n", level, line);
}
#define XLOG(...)                                            \
  do {                                                       \
    __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__); \
    xr_file_log("", __VA_ARGS__);                            \
  } while (0)
#define XWARN(...)                                            \
  do {                                                        \
    __android_log_print(ANDROID_LOG_WARN, kTag, __VA_ARGS__); \
    xr_file_log(" warn", __VA_ARGS__);                        \
  } while (0)
#define XERR(...)                                              \
  do {                                                         \
    __android_log_print(ANDROID_LOG_ERROR, kTag, __VA_ARGS__); \
    xr_file_log(" error", __VA_ARGS__);                        \
  } while (0)

bool xr_ok(XrResult r, const char* what) {
  if (XR_SUCCEEDED(r)) {
    return true;
  }
  XERR("%s failed: XrResult %d", what, static_cast<int>(r));
  return false;
}

bool vk_ok(VkResult r, const char* what) {
  if (r == VK_SUCCESS) {
    return true;
  }
  XERR("%s failed: VkResult %d", what, static_cast<int>(r));
  return false;
}

template <typename T>
bool get_proc(XrInstance instance, const char* name, T* out) {
  *out = nullptr;
  const XrResult r = xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(out));
  return XR_SUCCEEDED(r) && *out != nullptr;
}

// The three screen sizes the right stick click cycles through: width in meters, distance in meters.
struct ScreenPreset {
  float width;
  float distance;
};
constexpr ScreenPreset kScreenPresets[] = {
    {2.4f, 2.5f},  // living-room TV, a bit big
    {3.6f, 3.0f},  // big screen (default)
    {6.0f, 4.0f},  // cinema
};
constexpr int kScreenPresetCount = sizeof(kScreenPresets) / sizeof(kScreenPresets[0]);
constexpr int kDefaultScreenPreset = 1;

// One game frame every 1/60 s, whatever the headset refreshes at.
constexpr int64_t kGameTickNs = 16666667;

struct Actions {
  XrActionSet set = XR_NULL_HANDLE;
  XrAction a = XR_NULL_HANDLE, b = XR_NULL_HANDLE, x = XR_NULL_HANDLE, y = XR_NULL_HANDLE;
  XrAction menu = XR_NULL_HANDLE;
  XrAction triggerL = XR_NULL_HANDLE, triggerR = XR_NULL_HANDLE;
  XrAction squeezeL = XR_NULL_HANDLE, squeezeR = XR_NULL_HANDLE;
  XrAction stickL = XR_NULL_HANDLE, stickR = XR_NULL_HANDLE;
  XrAction stickClickL = XR_NULL_HANDLE, stickClickR = XR_NULL_HANDLE;
  XrAction gripPoseL = XR_NULL_HANDLE;  // the hands that grab the Tabletop field
  XrSpace gripSpaceL = XR_NULL_HANDLE;
  XrAction gripPoseR = XR_NULL_HANDLE;
  XrSpace gripSpaceR = XR_NULL_HANDLE;
  XrAction haptic = XR_NULL_HANDLE;     // both controllers: the game's pad rumble
};

// The game's rumble on pad 0, handed to the Touch controllers on the next input sync.
// RumbleActions.cpp picks the strength from the contact preset; cPlatPad starts and stops it.
struct Rumble {
  float amplitude = 0.5f;     // of the action that is running, 0..1
  bool startPending = false;
  float seconds = 0.0f;       // of the pending start
  bool stopPending = false;
};

// One eye's image for one frame slot: an optimally tiled VkImage on the XR device with exportable
// opaque-fd memory, the same memory imported into Dawn, and the sync objects of the hand-off.
struct EyeImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  wgpu::SharedTextureMemory shared;
  wgpu::Texture texture;
  wgpu::TextureView renderView;  // in aurora's scene format (RGBA8Unorm over the sRGB image)
  VkSemaphore dawnDone = VK_NULL_HANDLE;
  VkImageLayout dawnEndLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  bool accessOpen = false;
};

// The screen upload's staging buffer, command buffer and fence, two deep: the copy of game frame N
// is still on the GPU while frame N+1 is made, and nothing waits for it. Waiting on the copy every
// frame (r2-r6) also waited for everything queued before it, all of the frame's 3D, so the CPU and
// the GPU took turns instead of overlapping.
struct UploadSlot {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  VkDeviceSize size = 0;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pending = false;
};
constexpr int kUploadSlots = 2;

// Aurora's fixed staging pools (vertex, uniform, index, storage) that the eye re-sends fill.
constexpr int kPools = 4;

// Eye image sets in flight. r37 tried three: the game thread's ~7 ms wait for this fence just moved
// into xrWaitFrame (the runtime paces to the GPU, which is the real limit), no frame-rate change.
constexpr uint32_t kStereoSlots = 2;
struct StereoSlot {
  EyeImage eye[2];
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pending = false;
};

// Where the Tabletop field sits in the room (LOCAL space) and how big it is.
struct Table {
  XrVector3f position{0.0f, -0.55f, -0.95f};  // about waist height in front of where you started
  float yaw = 0.0f;                           // radians about the room's up axis
  float scale = 0.032f;                       // room meters per game unit: the 41 m pitch is ~1.3 m
  int grabHands = 0;                          // holding it: bit 0 left hand, bit 1 right hand
  bool rightGrab = false;                     // this right-grip squeeze started on the field
  XrVector3f grabOffset{0.0f, 0.0f, 0.0f};    // one hand: field position minus hand position
  // Two hands, at the moment the second one took hold: hand distance, the field's size, turn and
  // position, and the hands' midpoint; turn = how far the hands have turned around each other since.
  float twoDist = 1.0f, twoScale = 0.032f, twoYaw = 0.0f, prevAngle = 0.0f, turn = 0.0f;
  XrVector3f twoPos{0.0f, 0.0f, 0.0f}, twoMid{0.0f, 0.0f, 0.0f};
};

struct Passthrough {
  bool available = false;
  XrPassthroughFB handle = XR_NULL_HANDLE;
  XrPassthroughLayerFB layer = XR_NULL_HANDLE;
  bool layerRunning = false;
  PFN_xrCreatePassthroughFB create = nullptr;
  PFN_xrPassthroughStartFB start = nullptr;
  PFN_xrCreatePassthroughLayerFB createLayer = nullptr;
  PFN_xrPassthroughLayerPauseFB pauseLayer = nullptr;
  PFN_xrPassthroughLayerResumeFB resumeLayer = nullptr;
};

struct State {
  bool active = false;
  bool quit = false;

  JavaVM* vm = nullptr;
  jobject activity = nullptr;  // global ref

  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local = XR_NULL_HANDLE;
  XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
  bool running = false;
  bool focused = false;

  bool hasPassthroughExt = false;
  bool hasRefreshRateExt = false;
  bool hasPerfSettingsExt = false;
  bool hasThreadSettingsExt = false;

  VkInstance vkInstance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memProps{};

  VkBuffer staging = VK_NULL_HANDLE;  // the eye dump's
  VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
  void* stagingMapped = nullptr;
  VkDeviceSize stagingSize = 0;
  UploadSlot uploads[kUploadSlots];
  int uploadSlot = 0;

  XrSwapchain swapchain = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
  uint32_t swapchainWidth = 0, swapchainHeight = 0;
  int64_t swapchainFormat = 0;
  bool swapchainBgra = false;
  bool swapchainHasImage = false;

  Passthrough pt;
  bool mrMode = true;  // passthrough on; VR mode is the dark room until the stadium lands

  PFN_xrEnumerateDisplayRefreshRatesFB enumerateRefreshRates = nullptr;
  PFN_xrRequestDisplayRefreshRateFB requestRefreshRate = nullptr;

  Actions actions;
  Rumble rumble;
  uint64_t rumblePulses = 0;  // since the last stats line
  bool prevStickClickL = false;
  bool prevStickClickR = false;
  uint64_t pendingTapL = 0;  // time of an unanswered first tap, 0 when none
  uint64_t pendingTapR = 0;
  int screenPreset = kDefaultScreenPreset;
  bool tabletop = false;     // Jumbotron (false) or Tabletop (true)
  unsigned long lastUnderruns = 0;

  // --- 3D modes ---
  bool hasExtMemFd = false;
  bool hasExtSemFd = false;
  bool stereoFailed = false;          // set once; the 3D modes then fall back to the Jumbotron
  uint32_t eyeWidth = 0, eyeHeight = 0;
  XrSwapchain eyeSwapchain[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  std::vector<XrSwapchainImageVulkanKHR> eyeImages[2];
  StereoSlot slots[kStereoSlots];
  uint32_t slot = 0;                  // the slot this frame renders into
  XrSpace stage = XR_NULL_HANDLE;     // floor-level space, for the life-size stadium
  float floorY = -1.6f;               // LOCAL-space height of the floor (STAGE origin) when known
  XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  bool viewsValid = false;            // this display frame located both eyes
  bool stereoThisFrame = false;       // glPlat re-sent the frame per eye this game frame
  int stereoLayers = 0;
  bool eyePassOpen[2] = {false, false};
  XrCompositionLayerProjectionView projViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                   {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  bool hasProjection = false;         // projViews hold a submitted pair the compositor can reuse
  Table table;
  uint64_t lastTriggerPollNs = 0;
  bool dumpRequested = false;
  uint64_t stereoNsSinceStats = 0;
  uint64_t eyeSendStartNs = 0;
  uint64_t eyeSendNsSinceStats = 0;   // game-thread time inside the per-eye re-sends
  // Pool guard: what one eye send of each layer kind costs in each pool (running estimate), the
  // layer kinds this frame sends, and the open per-layer measurement.
  uint32_t poolReserved[kPools] = {};
  float perSend[2][kPools] = {};
  // r38: the players' decoded vertices land in the vertex pool once per frame (in whichever send
  // draws them first), not once per send: kept out of perSend, counted once in layers_fit.
  unsigned long long decodedAtMeasure = 0;  // aurora_xr_decoded_bytes when the open window began
  float decodedPerFrame = 0.0f;             // bytes, rise at once / fall slowly
  int frameLayerKind[2] = {0, 0};
  bool measuring = false;
  int measuringKind = 0;
  uint32_t measureStart[kPools] = {};
  uint64_t layersDroppedSinceStats = 0;
  // main.cpp's loop phases summed over game frames since the last stats line (PortXrNoteLoopTimes).
  uint64_t loopNs[5] = {};
  uint64_t xrWaitNsSinceStats = 0;    // inside xrWaitFrame, every display frame
  // Headset-side waits and costs since the last stats line: the eye slot's fence, the eye
  // swapchains' acquire + wait, the CPU fallback wait for Dawn, the effects layer, xrEndFrame.
  uint64_t eyeFenceNs = 0, swapNs = 0, cpuWaitNs = 0, fxNs = 0, xrEndNs = 0;
  bool gameplayLive = false;          // the match is being played (not a cutscene/replay/menu)
  bool fxReady = false;               // the celebration renderer is up (started at the first goal)
  bool dropLogged = false;            // the jumbotron's down position was logged for this replay
  bool bringTable = false;            // put the Tabletop field in front of you (left stick tap)
  bool celebBuzz = false;             // this goal is the player's: buzz with the cannon waves
  bool testWipe = false;              // self-test: Game.cpp plays the white-flash wipe once
  bool testItem = false;              // self-test: Game.cpp gives your team a green shell
  bool testSts = false;               // self-test: Game.cpp shows the Super Strike meter for 3 s
  struct {
    bool shown = false;               // the Super Strike meter is up in live play (PortXrSetStsMeter)
    bool logged = false;              // its Tabletop panel was logged for this Super Strike
    float game[3] = {0.0f, 0.0f, 0.0f};  // where the game hangs it, game units
    float halfU = 0.1f, halfV = 0.2f;    // its square in the middle of the picture
  } sts;
  float charScale = 1.5f;             // Tabletop: players drawn this much bigger (xr_tune "charscale")
  int msaaTable = 4;                  // eye-pass multisampling, table only / with the stadium layer
  int msaaStadium = 4;                // (1 or 4; xr_tune "msaa <table> <stadium>")
  uint64_t testPressXNs = 0;          // self-test: press X (use item) from this time for 150 ms
  bool prevGripR = false;             // right grip last input sync (its squeeze decides grab or Z)
  PADStatus localPad{};               // this headset's pad, this input sync
  bool port1Active = false;           // pad 1 fed from the network (online match)
  double loopIntervalEma = 0.0;       // ns between headset loop iterations, smoothed
  bool onlineHalfRate = false;        // online and too slow: 3D view every other game frame
  uint32_t onlineParity = 0;
  uint32_t prevItemButtons = 0;       // X / Y / Z last input sync, for the press log
  int buzzWave = 0;                   // cannon waves already buzzed
  uint32_t eyeViewMask = 0xFFFFFFFFu; // GLV views the eye sends may draw (self-test tuning)
  uint32_t eyeViewMaskTable = 0xFFFFFFFFu;    // ... in the table layer only
  uint32_t eyeViewMaskStadium = 0xFFFFFFFFu;  // ... in the stadium layer only
  float stadiumYawDebug = 0.0f;       // self-test: turn the life-size stadium (radians) to see behind
  float lodMinPx = 0.0f;              // stadium details smaller than this in an eye are skipped
  bool skipTallField = false;         // Tabletop · Stadium: no life-size goals/benches/fence
  uint64_t lastFxNs = 0;

  XrFrameState frameState{XR_TYPE_FRAME_STATE};
  bool frameBegun = false;
  int64_t tickAccumNs = 0;
  uint64_t lastTickNs = 0;

  uint64_t framesShown = 0;
  uint64_t lastStatsNs = 0;
  uint64_t gameFramesSinceStats = 0;
  uint64_t displayFramesSinceStats = 0;
  uint64_t uploadNsSinceStats = 0;
};
State g;

// The latest game frame, written by the render worker inside aurora_end_frame's work, read on the
// game thread once aurora::gfx::synchronize() has returned.
std::mutex g_frameMutex;
std::vector<uint8_t> g_frame;  // tightly packed RGBA8
uint32_t g_frameWidth = 0, g_frameHeight = 0;
uint64_t g_frameSerial = 0;
uint64_t g_frameUploaded = 0;

uint64_t now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

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
// Instance, system, Vulkan device, session
// ---------------------------------------------------------------------------------------------

bool init_loader() {
  PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
  if (!get_proc(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &initializeLoader)) {
    XERR("xrInitializeLoaderKHR not found");
    return false;
  }
  XrLoaderInitInfoAndroidKHR info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
  info.applicationVM = g.vm;
  info.applicationContext = g.activity;
  return xr_ok(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&info)),
               "xrInitializeLoaderKHR");
}

bool create_instance() {
  uint32_t count = 0;
  xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
  std::vector<XrExtensionProperties> props(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
  if (count > 0) {
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data());
  }
  auto has = [&](const char* name) {
    for (const auto& p : props) {
      if (std::strcmp(p.extensionName, name) == 0) {
        return true;
      }
    }
    return false;
  };

  std::vector<const char*> exts;
  if (!has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) || !has(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME)) {
    XERR("runtime lacks XR_KHR_vulkan_enable2 or XR_KHR_android_create_instance");
    return false;
  }
  exts.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
  exts.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
  g.hasPassthroughExt = has(XR_FB_PASSTHROUGH_EXTENSION_NAME);
  if (g.hasPassthroughExt) {
    exts.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
  }
  g.hasRefreshRateExt = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  if (g.hasRefreshRateExt) {
    exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  }
  g.hasPerfSettingsExt = has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
  if (g.hasPerfSettingsExt) {
    exts.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
  }
  g.hasThreadSettingsExt = has(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
  if (g.hasThreadSettingsExt) {
    exts.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
  }
  XLOG("extensions: passthrough=%d refresh_rate=%d perf_settings=%d thread_settings=%d", g.hasPassthroughExt ? 1 : 0,
       g.hasRefreshRateExt ? 1 : 0, g.hasPerfSettingsExt ? 1 : 0, g.hasThreadSettingsExt ? 1 : 0);

  XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
  android.applicationVM = g.vm;
  android.applicationActivity = g.activity;

  XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
  info.next = &android;
  std::strncpy(info.applicationInfo.applicationName, "Strikers MR", XR_MAX_APPLICATION_NAME_SIZE - 1);
  info.applicationInfo.applicationVersion = 2;
  std::strncpy(info.applicationInfo.engineName, "Aurora", XR_MAX_ENGINE_NAME_SIZE - 1);
  info.applicationInfo.engineVersion = 1;
  info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  info.enabledExtensionCount = static_cast<uint32_t>(exts.size());
  info.enabledExtensionNames = exts.data();
  if (!xr_ok(xrCreateInstance(&info, &g.instance), "xrCreateInstance")) {
    return false;
  }

  XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
  systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  return xr_ok(xrGetSystem(g.instance, &systemInfo, &g.system), "xrGetSystem");
}

bool create_vulkan_device() {
  PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
  PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
  PFN_xrGetVulkanGraphicsDevice2KHR getDevice = nullptr;
  PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
  if (!get_proc(g.instance, "xrGetVulkanGraphicsRequirements2KHR", &getRequirements) ||
      !get_proc(g.instance, "xrCreateVulkanInstanceKHR", &createInstance) ||
      !get_proc(g.instance, "xrGetVulkanGraphicsDevice2KHR", &getDevice) ||
      !get_proc(g.instance, "xrCreateVulkanDeviceKHR", &createDevice)) {
    XERR("XR_KHR_vulkan_enable2 entry points missing");
    return false;
  }

  XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  if (!xr_ok(getRequirements(g.instance, g.system, &requirements), "xrGetVulkanGraphicsRequirements2KHR")) {
    return false;
  }
  // XrVersion is not a Vulkan version: convert, and ask for at least 1.1.
  const XrVersion xrMin = requirements.minApiVersionSupported;
  const XrVersion xrMax = requirements.maxApiVersionSupported;
  const uint32_t vkMin = VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(xrMin), XR_VERSION_MINOR(xrMin), 0);
  const uint32_t vkMax = VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(xrMax), XR_VERSION_MINOR(xrMax), 0);
  uint32_t apiVersion = vkMin < VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : vkMin;
  if (xrMax != 0 && apiVersion > vkMax) {
    apiVersion = vkMax;
  }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "Strikers MR";
  app.applicationVersion = 2;
  app.pEngineName = "Aurora";
  app.apiVersion = apiVersion;
  VkInstanceCreateInfo vkInstanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  vkInstanceInfo.pApplicationInfo = &app;

  XrVulkanInstanceCreateInfoKHR xrVkInstance{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
  xrVkInstance.systemId = g.system;
  xrVkInstance.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
  xrVkInstance.vulkanCreateInfo = &vkInstanceInfo;
  VkResult vkResult = VK_SUCCESS;
  if (!xr_ok(createInstance(g.instance, &xrVkInstance, &g.vkInstance, &vkResult), "xrCreateVulkanInstanceKHR") ||
      !vk_ok(vkResult, "vkCreateInstance (via OpenXR)")) {
    return false;
  }

  XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  deviceGetInfo.systemId = g.system;
  deviceGetInfo.vulkanInstance = g.vkInstance;
  if (!xr_ok(getDevice(g.instance, &deviceGetInfo, &g.physical), "xrGetVulkanGraphicsDevice2KHR")) {
    return false;
  }

  uint32_t familyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &familyCount, nullptr);
  std::vector<VkQueueFamilyProperties> families(familyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &familyCount, families.data());
  bool found = false;
  for (uint32_t i = 0; i < familyCount; ++i) {
    if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
      g.queueFamily = i;
      found = true;
      break;
    }
  }
  if (!found) {
    XERR("no graphics queue family");
    return false;
  }

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queueInfo.queueFamilyIndex = g.queueFamily;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &priority;

  // The 3D modes share images with Dawn's device: exportable opaque-fd memory one way, a sync-fd
  // semaphore back. Each enabled only if the device lists it (several are core in 1.1 anyway).
  uint32_t extCount = 0;
  vkEnumerateDeviceExtensionProperties(g.physical, nullptr, &extCount, nullptr);
  std::vector<VkExtensionProperties> extProps(extCount);
  if (extCount > 0) {
    vkEnumerateDeviceExtensionProperties(g.physical, nullptr, &extCount, extProps.data());
  }
  auto hasExt = [&](const char* name) {
    for (const auto& e : extProps) {
      if (std::strcmp(e.extensionName, name) == 0) {
        return true;
      }
    }
    return false;
  };
  std::vector<const char*> deviceExts;
  for (const char* name : {VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                           VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
                           VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
                           VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME}) {
    if (hasExt(name)) {
      deviceExts.push_back(name);
    }
  }
  g.hasExtMemFd = hasExt(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
  g.hasExtSemFd = hasExt(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
  XLOG("XR device: external memory fd=%d, semaphore fd=%d", g.hasExtMemFd ? 1 : 0, g.hasExtSemFd ? 1 : 0);

  VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  deviceInfo.queueCreateInfoCount = 1;
  deviceInfo.pQueueCreateInfos = &queueInfo;
  deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
  deviceInfo.ppEnabledExtensionNames = deviceExts.data();

  XrVulkanDeviceCreateInfoKHR xrVkDevice{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
  xrVkDevice.systemId = g.system;
  xrVkDevice.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
  xrVkDevice.vulkanPhysicalDevice = g.physical;
  xrVkDevice.vulkanCreateInfo = &deviceInfo;
  if (!xr_ok(createDevice(g.instance, &xrVkDevice, &g.device, &vkResult), "xrCreateVulkanDeviceKHR") ||
      !vk_ok(vkResult, "vkCreateDevice (via OpenXR)")) {
    return false;
  }
  vkGetDeviceQueue(g.device, g.queueFamily, 0, &g.queue);
  vkGetPhysicalDeviceMemoryProperties(g.physical, &g.memProps);

  VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = g.queueFamily;
  if (!vk_ok(vkCreateCommandPool(g.device, &poolInfo, nullptr, &g.pool), "vkCreateCommandPool")) {
    return false;
  }
  VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocInfo.commandPool = g.pool;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;
  if (!vk_ok(vkAllocateCommandBuffers(g.device, &allocInfo, &g.cmd), "vkAllocateCommandBuffers")) {
    return false;
  }
  VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  return vk_ok(vkCreateFence(g.device, &fenceInfo, nullptr, &g.fence), "vkCreateFence");
}

bool create_session() {
  XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
  binding.instance = g.vkInstance;
  binding.physicalDevice = g.physical;
  binding.device = g.device;
  binding.queueFamilyIndex = g.queueFamily;
  binding.queueIndex = 0;
  XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
  info.next = &binding;
  info.systemId = g.system;
  if (!xr_ok(xrCreateSession(g.instance, &info, &g.session), "xrCreateSession")) {
    return false;
  }
  XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  space.poseInReferenceSpace.orientation.w = 1.0f;
  if (!xr_ok(xrCreateReferenceSpace(g.session, &space, &g.local), "xrCreateReferenceSpace(LOCAL)")) {
    return false;
  }
  // The floor, for the life-size stadium: STAGE's origin is on it. Optional.
  space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  if (XR_FAILED(xrCreateReferenceSpace(g.session, &space, &g.stage))) {
    g.stage = XR_NULL_HANDLE;
    XWARN("no STAGE space; assuming the floor is 1.6 m below the start pose");
  }

  // The eye images: the runtime's recommended size, scaled (STRIKERS_XR_EYE_SCALE) and kept to a
  // multiple of 8. Native size since r32 (Lance: the Tabletop looked grainy at 0.75; the eye passes
  // cost draws, not pixels).
  uint32_t viewCount = 0;
  xrEnumerateViewConfigurationViews(g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
                                    nullptr);
  std::vector<XrViewConfigurationView> configViews(viewCount, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
  if (viewCount >= 2 &&
      XR_SUCCEEDED(xrEnumerateViewConfigurationViews(g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                     viewCount, &viewCount, configViews.data()))) {
    const char* e = std::getenv("STRIKERS_XR_EYE_SCALE");
    float scale = (e != nullptr && *e != '\0') ? static_cast<float>(std::atof(e)) : 1.0f;
    scale = std::max(0.3f, std::min(1.5f, scale));
    g.eyeWidth = (static_cast<uint32_t>(configViews[0].recommendedImageRectWidth * scale) / 8) * 8;
    g.eyeHeight = (static_cast<uint32_t>(configViews[0].recommendedImageRectHeight * scale) / 8) * 8;
    XLOG("eye images %ux%u (recommended %ux%u x %.2f)", g.eyeWidth, g.eyeHeight,
         configViews[0].recommendedImageRectWidth, configViews[0].recommendedImageRectHeight, scale);
  }
  return true;
}

void create_passthrough() {
  if (!g.hasPassthroughExt) {
    return;
  }
  Passthrough& pt = g.pt;
  if (!get_proc(g.instance, "xrCreatePassthroughFB", &pt.create) ||
      !get_proc(g.instance, "xrPassthroughStartFB", &pt.start) ||
      !get_proc(g.instance, "xrCreatePassthroughLayerFB", &pt.createLayer) ||
      !get_proc(g.instance, "xrPassthroughLayerPauseFB", &pt.pauseLayer) ||
      !get_proc(g.instance, "xrPassthroughLayerResumeFB", &pt.resumeLayer)) {
    XWARN("passthrough entry points missing");
    return;
  }
  XrPassthroughCreateInfoFB info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
  info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
  if (!xr_ok(pt.create(g.session, &info, &pt.handle), "xrCreatePassthroughFB")) {
    return;
  }
  XrPassthroughLayerCreateInfoFB layerInfo{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
  layerInfo.passthrough = pt.handle;
  layerInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
  layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
  if (!xr_ok(pt.createLayer(g.session, &layerInfo, &pt.layer), "xrCreatePassthroughLayerFB")) {
    return;
  }
  pt.available = true;
  pt.layerRunning = true;
  XLOG("passthrough ready");
}

const char* mode_name() {
  if (g.tabletop) {
    return g.mrMode ? "Tabletop · Living Room" : "Tabletop · Stadium";
  }
  return g.mrMode ? "Jumbotron · Living Room" : "Jumbotron · Stadium";
}

void set_mr_mode(bool mr) {
  g.mrMode = mr;
  Passthrough& pt = g.pt;
  if (!pt.available) {
    return;
  }
  if (mr && !pt.layerRunning) {
    pt.layerRunning = xr_ok(pt.resumeLayer(pt.layer), "xrPassthroughLayerResumeFB");
  } else if (!mr && pt.layerRunning) {
    if (xr_ok(pt.pauseLayer(pt.layer), "xrPassthroughLayerPauseFB")) {
      pt.layerRunning = false;
    }
  }
  XLOG("mode: %s", mode_name());
}

void init_refresh_rate() {
  if (!g.hasRefreshRateExt) {
    return;
  }
  if (!get_proc(g.instance, "xrEnumerateDisplayRefreshRatesFB", &g.enumerateRefreshRates) ||
      !get_proc(g.instance, "xrRequestDisplayRefreshRateFB", &g.requestRefreshRate)) {
    return;
  }
  uint32_t count = 0;
  g.enumerateRefreshRates(g.session, 0, &count, nullptr);
  std::vector<float> rates(count);
  if (count > 0) {
    g.enumerateRefreshRates(g.session, count, &count, rates.data());
  }
  std::string list;
  for (float r : rates) {
    list += std::to_string(static_cast<int>(r + 0.5f)) + " ";
  }
  XLOG("display refresh rates: %s", list.c_str());
  // r2 stays at 72 Hz: one game frame in six is shown twice, but every display frame that runs a
  // game frame also carries the CPU readback, and 72 Hz is the budget that fits it.
  if (std::find(rates.begin(), rates.end(), 72.0f) != rates.end()) {
    xr_ok(g.requestRefreshRate(g.session, 72.0f), "xrRequestDisplayRefreshRateFB(72)");
  }
}

// ---------------------------------------------------------------------------------------------
// Input: Touch controllers -> GameCube pad (port 0)
// ---------------------------------------------------------------------------------------------

XrPath path(const char* s) {
  XrPath p = XR_NULL_PATH;
  xrStringToPath(g.instance, s, &p);
  return p;
}

bool make_action(XrActionType type, const char* name, const char* localized, XrAction* out) {
  XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
  info.actionType = type;
  std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
  std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
  return xr_ok(xrCreateAction(g.actions.set, &info, out), name);
}

bool create_actions() {
  Actions& a = g.actions;
  XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strncpy(setInfo.actionSetName, "strikers", XR_MAX_ACTION_SET_NAME_SIZE - 1);
  std::strncpy(setInfo.localizedActionSetName, "Strikers", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
  if (!xr_ok(xrCreateActionSet(g.instance, &setInfo, &a.set), "xrCreateActionSet")) {
    return false;
  }
  bool ok = true;
  ok &= make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "A", &a.a);
  ok &= make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "B", &a.b);
  ok &= make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", "X", &a.x);
  ok &= make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", "Y", &a.y);
  ok &= make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_start", "Start", &a.menu);
  ok &= make_action(XR_ACTION_TYPE_FLOAT_INPUT, "trigger_l", "L", &a.triggerL);
  ok &= make_action(XR_ACTION_TYPE_FLOAT_INPUT, "trigger_r", "R", &a.triggerR);
  ok &= make_action(XR_ACTION_TYPE_FLOAT_INPUT, "squeeze_l", "Grab", &a.squeezeL);
  ok &= make_action(XR_ACTION_TYPE_FLOAT_INPUT, "squeeze_r", "Z", &a.squeezeR);
  ok &= make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "stick_l", "Control stick", &a.stickL);
  ok &= make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "stick_r", "C-stick", &a.stickR);
  ok &= make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click_l", "MR/VR switch", &a.stickClickL);
  ok &= make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click_r", "Screen size", &a.stickClickR);
  ok &= make_action(XR_ACTION_TYPE_POSE_INPUT, "grip_pose_l", "Left hand", &a.gripPoseL);
  ok &= make_action(XR_ACTION_TYPE_POSE_INPUT, "grip_pose_r", "Right hand", &a.gripPoseR);
  ok &= make_action(XR_ACTION_TYPE_VIBRATION_OUTPUT, "rumble", "Rumble", &a.haptic);
  if (!ok) {
    return false;
  }

  const XrActionSuggestedBinding bindings[] = {
      {a.a, path("/user/hand/right/input/a/click")},
      {a.b, path("/user/hand/right/input/b/click")},
      {a.x, path("/user/hand/left/input/x/click")},
      {a.y, path("/user/hand/left/input/y/click")},
      {a.menu, path("/user/hand/left/input/menu/click")},
      {a.triggerL, path("/user/hand/left/input/trigger/value")},
      {a.triggerR, path("/user/hand/right/input/trigger/value")},
      {a.squeezeL, path("/user/hand/left/input/squeeze/value")},
      {a.squeezeR, path("/user/hand/right/input/squeeze/value")},
      {a.stickL, path("/user/hand/left/input/thumbstick")},
      {a.stickR, path("/user/hand/right/input/thumbstick")},
      {a.stickClickL, path("/user/hand/left/input/thumbstick/click")},
      {a.stickClickR, path("/user/hand/right/input/thumbstick/click")},
      {a.gripPoseL, path("/user/hand/left/input/grip/pose")},
      {a.gripPoseR, path("/user/hand/right/input/grip/pose")},
      {a.haptic, path("/user/hand/left/output/haptic")},
      {a.haptic, path("/user/hand/right/output/haptic")},
  };
  XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  suggested.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
  suggested.countSuggestedBindings = static_cast<uint32_t>(sizeof(bindings) / sizeof(bindings[0]));
  suggested.suggestedBindings = bindings;
  if (!xr_ok(xrSuggestInteractionProfileBindings(g.instance, &suggested), "xrSuggestInteractionProfileBindings")) {
    return false;
  }
  XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach.countActionSets = 1;
  attach.actionSets = &a.set;
  if (!xr_ok(xrAttachSessionActionSets(g.session, &attach), "xrAttachSessionActionSets")) {
    return false;
  }
  XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
  spaceInfo.action = a.gripPoseL;
  spaceInfo.poseInActionSpace.orientation.w = 1.0f;
  if (XR_FAILED(xrCreateActionSpace(g.session, &spaceInfo, &a.gripSpaceL))) {
    a.gripSpaceL = XR_NULL_HANDLE;
  }
  spaceInfo.action = a.gripPoseR;
  if (XR_FAILED(xrCreateActionSpace(g.session, &spaceInfo, &a.gripSpaceR))) {
    a.gripSpaceR = XR_NULL_HANDLE;
  }
  return true;
}

bool get_bool(XrAction action) {
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  info.action = action;
  XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
  if (XR_FAILED(xrGetActionStateBoolean(g.session, &info, &state))) {
    return false;
  }
  return state.isActive && state.currentState;
}

float get_float(XrAction action) {
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  info.action = action;
  XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
  if (XR_FAILED(xrGetActionStateFloat(g.session, &info, &state)) || !state.isActive) {
    return 0.0f;
  }
  return state.currentState;
}

XrVector2f get_vec2(XrAction action) {
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  info.action = action;
  XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
  if (XR_FAILED(xrGetActionStateVector2f(g.session, &info, &state)) || !state.isActive) {
    return XrVector2f{0.0f, 0.0f};
  }
  return state.currentState;
}

int8_t stick_axis(float v) {
  const float c = std::max(-1.0f, std::min(1.0f, v));
  return static_cast<int8_t>(std::lround(c * 127.0f));
}

void apply_stick(XrVector2f v, int8_t* outX, int8_t* outY) {
  constexpr float kDeadzone = 0.12f;
  const float mag = std::sqrt(v.x * v.x + v.y * v.y);
  if (mag < kDeadzone) {
    *outX = 0;
    *outY = 0;
    return;
  }
  // Rescale past the deadzone so a small push still moves, and full push still reaches the rim.
  const float scaled = std::min(1.0f, (mag - kDeadzone) / (1.0f - kDeadzone)) / mag;
  *outX = stick_axis(v.x * scaled);
  *outY = stick_axis(v.y * scaled);  // OpenXR +y is up, as is the GameCube's
}

uint8_t trigger_analog(float v) {
  if (v < 0.05f) {
    return 0;
  }
  return static_cast<uint8_t>(std::lround(std::min(1.0f, v) * 255.0f));
}

// The game's pad rumble on both Touch controllers. A start replaces whatever is buzzing (the
// runtime does the same), so the stop only goes out when no new start came with it.
void apply_rumble() {
  Rumble& r = g.rumble;
  if (!r.startPending && !r.stopPending) {
    return;
  }
  XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
  info.action = g.actions.haptic;
  if (r.startPending) {
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = r.amplitude;
    vibration.duration = static_cast<XrDuration>(std::max(0.02f, r.seconds) * 1e9f);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    xr_ok(xrApplyHapticFeedback(g.session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration)),
          "xrApplyHapticFeedback");
    ++g.rumblePulses;
  } else {
    xr_ok(xrStopHapticFeedback(g.session, &info), "xrStopHapticFeedback");
  }
  r.startPending = false;
  r.stopPending = false;
}

bool locate_hand(XrSpace space, XrVector3f* out) {
  if (space == XR_NULL_HANDLE) return false;
  XrSpaceLocation hand{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(xrLocateSpace(space, g.local, g.frameState.predictedDisplayTime, &hand)) ||
      (hand.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0) {
    return false;
  }
  *out = hand.pose.position;
  return true;
}

// The hand on or right next to the Tabletop field (within 8 cm of the pitch and its fence, from a
// little under the field to a little over the fence).
bool near_table(const XrVector3f& p) {
  const float dx = p.x - g.table.position.x, dz = p.z - g.table.position.z;
  const float cs = std::cos(g.table.yaw), sn = std::sin(g.table.yaw), s = g.table.scale;
  // Back into game units (the inverse of the field's room mapping, as xr_fx.cpp does).
  const float gx = (dx * cs - dz * sn) / s, gy = -(dx * sn + dz * cs) / s;
  const float margin = 0.08f / s;
  const float dy = p.y - g.table.position.y;
  return std::fabs(gx) < 22.5f + margin && std::fabs(gy) < 13.5f + margin && dy > -0.12f && dy < 2.5f * s + 0.15f;
}

void sync_input() {
  PADStatus status{};
  status.err = PAD_ERR_NONE;

  if (g.focused) {
    XrActiveActionSet active{g.actions.set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_SUCCEEDED(xrSyncActions(g.session, &sync))) {
      const Actions& a = g.actions;
      if (get_bool(a.a)) status.button |= PAD_BUTTON_A;
      if (get_bool(a.b)) status.button |= PAD_BUTTON_B;
      if (get_bool(a.x)) status.button |= PAD_BUTTON_X;
      if (get_bool(a.y)) status.button |= PAD_BUTTON_Y;
      if (get_bool(a.menu)) status.button |= PAD_BUTTON_START;

      const float tl = get_float(a.triggerL);
      const float tr = get_float(a.triggerR);
      status.triggerLeft = trigger_analog(tl);
      status.triggerRight = trigger_analog(tr);
      if (tl > 0.9f) status.button |= PAD_TRIGGER_L;
      if (tr > 0.9f) status.button |= PAD_TRIGGER_R;

      // Tabletop: grabbing the field (Lance, 2026-10-10). The left grip grabs it from anywhere; the
      // right grip grabs it when the hand is on or right next to it, and anywhere else stays the
      // GameCube Z (the item swap). One hand carries it, up and down too; with both hands, pulling
      // them apart or together resizes it and turning them around each other turns it. The left
      // hand alone still resizes (stick up/down) and turns (left/right) it with the stick.
      XrVector2f stickL = get_vec2(a.stickL);
      XrVector3f handL{}, handR{};
      const bool haveL = locate_hand(a.gripSpaceL, &handL);
      const bool haveR = locate_hand(a.gripSpaceR, &handR);
      const bool gripR = get_float(a.squeezeR) > 0.5f;
      if (gripR && !g.prevGripR) {
        g.table.rightGrab = g.tabletop && !g.stereoFailed && haveR && near_table(handR);
      }
      if (!gripR) g.table.rightGrab = false;
      if (gripR && !g.table.rightGrab) status.button |= PAD_TRIGGER_Z;
      g.prevGripR = gripR;
      const bool holdL = g.tabletop && get_float(a.squeezeL) > 0.6f;
      const int hands = (holdL && haveL ? 1 : 0) | (g.table.rightGrab && haveR ? 2 : 0);
      if (hands != g.table.grabHands) {
        // Taking hold or letting go with one hand: fresh baselines, so the field never jumps.
        if (hands == 3) {
          const float dx = handR.x - handL.x, dy = handR.y - handL.y, dz = handR.z - handL.z;
          g.table.twoDist = std::max(0.05f, std::sqrt(dx * dx + dy * dy + dz * dz));
          g.table.prevAngle = std::atan2(dz, dx);
          g.table.turn = 0.0f;
          g.table.twoScale = g.table.scale;
          g.table.twoYaw = g.table.yaw;
          g.table.twoPos = g.table.position;
          g.table.twoMid = {0.5f * (handL.x + handR.x), 0.5f * (handL.y + handR.y), 0.5f * (handL.z + handR.z)};
        } else if (hands != 0) {
          const XrVector3f& p = hands == 1 ? handL : handR;
          g.table.grabOffset = {g.table.position.x - p.x, g.table.position.y - p.y, g.table.position.z - p.z};
        } else {
          XLOG("table at (%.2f, %.2f, %.2f) yaw %.2f scale %.4f", g.table.position.x, g.table.position.y,
               g.table.position.z, g.table.yaw, g.table.scale);
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
        g.table.scale = std::max(0.008f, std::min(0.25f, g.table.twoScale * dist / g.table.twoDist));
        const float k = g.table.scale / g.table.twoScale;
        // The room's angle of the field's axes is -yaw, so turning the hands one way turns it with them.
        g.table.yaw = g.table.twoYaw - g.table.turn;
        // Around the hands' midpoint: the field keeps its place relative to your hands.
        const XrVector3f mid{0.5f * (handL.x + handR.x), 0.5f * (handL.y + handR.y), 0.5f * (handL.z + handR.z)};
        const float vx = g.table.twoPos.x - g.table.twoMid.x, vz = g.table.twoPos.z - g.table.twoMid.z;
        const float cs = std::cos(g.table.turn), sn = std::sin(g.table.turn);
        g.table.position = {mid.x + (vx * cs - vz * sn) * k, mid.y + (g.table.twoPos.y - g.table.twoMid.y) * k,
                            mid.z + (vx * sn + vz * cs) * k};
      } else if (hands != 0) {
        const XrVector3f& p = hands == 1 ? handL : handR;
        g.table.position = {p.x + g.table.grabOffset.x, p.y + g.table.grabOffset.y, p.z + g.table.grabOffset.z};
      }
      if (holdL) {
        if (hands == 1 && std::fabs(stickL.y) > 0.2f) {
          g.table.scale = std::max(0.008f, std::min(0.25f, g.table.scale * std::exp(stickL.y * 0.025f)));
        }
        if (hands == 1 && std::fabs(stickL.x) > 0.2f) {
          g.table.yaw -= stickL.x * 0.035f;
        }
        stickL = XrVector2f{0.0f, 0.0f};
      }
      // For the item check: which of the item buttons arrive (X use, Z swap, Y hit).
      const uint32_t itemButtons = status.button & (PAD_BUTTON_X | PAD_BUTTON_Y | PAD_TRIGGER_Z);
      if ((itemButtons & ~g.prevItemButtons) != 0) {
        XLOG("touch: %s%s%s", (itemButtons & ~g.prevItemButtons & PAD_BUTTON_X) ? "X (use item) " : "",
             (itemButtons & ~g.prevItemButtons & PAD_TRIGGER_Z) ? "Z (swap item) " : "",
             (itemButtons & ~g.prevItemButtons & PAD_BUTTON_Y) ? "Y " : "");
      }
      g.prevItemButtons = itemButtons;
      // Self-test (xr_fx "item"): X pressed for a moment, through this same path.
      const uint64_t tNow = now_ns();
      if (g.testPressXNs != 0 && tNow >= g.testPressXNs) {
        status.button |= PAD_BUTTON_X;
        if (tNow >= g.testPressXNs + 150000000ull) {
          g.testPressXNs = 0;
          XLOG("item test: X released");
        }
      }
      apply_stick(stickL, &status.stickX, &status.stickY);
      apply_stick(get_vec2(a.stickR), &status.substickX, &status.substickY);

      // Stick clicks, the two buttons a GameCube pad never had. Double taps switch modes, so a
      // stray click mid-play does nothing:
      //   left  double tap: Living Room <-> Stadium (MR <-> VR)
      //   right double tap: Jumbotron <-> Tabletop
      //   right single tap: next screen size (Jumbotron)
      // A single tap is only acted on once the double-tap window has passed without a second.
      constexpr uint64_t kDoubleTapNs = 350000000ull;
      const uint64_t t = now_ns();
      const bool clickL = get_bool(a.stickClickL);
      const bool clickR = get_bool(a.stickClickR);
      if (clickL && !g.prevStickClickL) {
        if (g.pendingTapL != 0 && t - g.pendingTapL < kDoubleTapNs) {
          g.pendingTapL = 0;
          set_mr_mode(!g.mrMode);
        } else {
          g.pendingTapL = t;
        }
      }
      if (g.pendingTapL != 0 && t - g.pendingTapL >= kDoubleTapNs) {
        g.pendingTapL = 0;
        if (g.tabletop) {
          g.bringTable = true;  // single left tap: the table comes to you
        }
      }
      if (clickR && !g.prevStickClickR) {
        if (g.pendingTapR != 0 && t - g.pendingTapR < kDoubleTapNs) {
          g.pendingTapR = 0;
          g.tabletop = !g.tabletop;
          XLOG("mode: %s", mode_name());
        } else {
          g.pendingTapR = t;
        }
      }
      if (g.pendingTapR != 0 && t - g.pendingTapR >= kDoubleTapNs) {
        g.pendingTapR = 0;
        g.screenPreset = (g.screenPreset + 1) % kScreenPresetCount;
        XLOG("screen preset %d (%.1f m wide at %.1f m)", g.screenPreset, kScreenPresets[g.screenPreset].width,
             kScreenPresets[g.screenPreset].distance);
      }
      g.prevStickClickL = clickL;
      g.prevStickClickR = clickR;

      apply_rumble();
    }
  }
  // Every frame, neutral or not: keeps port 0 connected with no Bluetooth pad paired. A paired
  // pad still works alongside; Aurora merges the two (pad.cpp merge_virtual_status).
  // Handed to the pads at the end of PortXrBeginFrame, where an online match decides the frame.
  g.localPad = status;
}

// ---------------------------------------------------------------------------------------------
// Screen swapchain + upload
// ---------------------------------------------------------------------------------------------

uint32_t find_memory_type(uint32_t typeBits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < g.memProps.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) != 0 && (g.memProps.memoryTypes[i].propertyFlags & want) == want) {
      return i;
    }
  }
  return UINT32_MAX;
}

void destroy_staging() {
  if (g.stagingMapped != nullptr) {
    vkUnmapMemory(g.device, g.stagingMemory);
    g.stagingMapped = nullptr;
  }
  if (g.staging != VK_NULL_HANDLE) {
    vkDestroyBuffer(g.device, g.staging, nullptr);
    g.staging = VK_NULL_HANDLE;
  }
  if (g.stagingMemory != VK_NULL_HANDLE) {
    vkFreeMemory(g.device, g.stagingMemory, nullptr);
    g.stagingMemory = VK_NULL_HANDLE;
  }
  g.stagingSize = 0;
}

// A host-visible, mapped transfer buffer of at least `size` bytes, (re)made in place.
bool ensure_host_buffer(VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped, VkDeviceSize& current,
                        VkDeviceSize size, const char* what) {
  if (buffer != VK_NULL_HANDLE && current >= size) {
    return true;
  }
  if (mapped != nullptr) vkUnmapMemory(g.device, memory);
  if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(g.device, buffer, nullptr);
  if (memory != VK_NULL_HANDLE) vkFreeMemory(g.device, memory, nullptr);
  mapped = nullptr;
  buffer = VK_NULL_HANDLE;
  memory = VK_NULL_HANDLE;
  current = 0;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (!vk_ok(vkCreateBuffer(g.device, &info, nullptr, &buffer), what)) {
    return false;
  }
  VkMemoryRequirements reqs{};
  vkGetBufferMemoryRequirements(g.device, buffer, &reqs);
  uint32_t type = find_memory_type(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  if (type == UINT32_MAX) {
    type = find_memory_type(reqs.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  }
  if (type == UINT32_MAX) {
    XERR("no host-visible memory for %s", what);
    return false;
  }
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = reqs.size;
  alloc.memoryTypeIndex = type;
  if (!vk_ok(vkAllocateMemory(g.device, &alloc, nullptr, &memory), what) ||
      !vk_ok(vkBindBufferMemory(g.device, buffer, memory, 0), what) ||
      !vk_ok(vkMapMemory(g.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), what)) {
    return false;
  }
  current = size;
  return true;
}

// The eye dump's buffer (a copy destination: it needs TRANSFER_DST, which r4-r6 lacked).
bool ensure_staging(VkDeviceSize size) {
  return ensure_host_buffer(g.staging, g.stagingMemory, g.stagingMapped, g.stagingSize, size, "staging");
}

void destroy_swapchain() {
  if (g.swapchain != XR_NULL_HANDLE) {
    xrDestroySwapchain(g.swapchain);
    g.swapchain = XR_NULL_HANDLE;
  }
  g.swapchainImages.clear();
  g.swapchainWidth = g.swapchainHeight = 0;
  g.swapchainHasImage = false;
}

bool ensure_swapchain(uint32_t width, uint32_t height) {
  if (g.swapchain != XR_NULL_HANDLE && g.swapchainWidth == width && g.swapchainHeight == height) {
    return true;
  }
  destroy_swapchain();

  uint32_t count = 0;
  xrEnumerateSwapchainFormats(g.session, 0, &count, nullptr);
  std::vector<int64_t> formats(count);
  if (count > 0) {
    xrEnumerateSwapchainFormats(g.session, count, &count, formats.data());
  }
  // The game's pixels are already display-encoded, so they go into an sRGB image byte for byte and
  // the compositor shows them as the TV would have.
  const int64_t preferred[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB};
  g.swapchainFormat = 0;
  for (int64_t want : preferred) {
    if (std::find(formats.begin(), formats.end(), want) != formats.end()) {
      g.swapchainFormat = want;
      break;
    }
  }
  if (g.swapchainFormat == 0) {
    XERR("no sRGB RGBA8/BGRA8 swapchain format (runtime offers %u formats)", count);
    return false;
  }
  g.swapchainBgra = g.swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB;

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  info.format = g.swapchainFormat;
  info.sampleCount = 1;
  info.width = width;
  info.height = height;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  if (!xr_ok(xrCreateSwapchain(g.session, &info, &g.swapchain), "xrCreateSwapchain")) {
    return false;
  }
  uint32_t imageCount = 0;
  xrEnumerateSwapchainImages(g.swapchain, 0, &imageCount, nullptr);
  g.swapchainImages.assign(imageCount, XrSwapchainImageVulkanKHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  if (!xr_ok(xrEnumerateSwapchainImages(g.swapchain, imageCount, &imageCount,
                                        reinterpret_cast<XrSwapchainImageBaseHeader*>(g.swapchainImages.data())),
             "xrEnumerateSwapchainImages")) {
    destroy_swapchain();
    return false;
  }
  g.swapchainWidth = width;
  g.swapchainHeight = height;
  XLOG("screen swapchain %ux%u format %lld (%u images)", width, height, static_cast<long long>(g.swapchainFormat),
       imageCount);
  return true;
}

// Copies the newest game frame into the screen swapchain. Game thread, after synchronize().
void upload_latest_frame() {
  const uint64_t start = now_ns();
  uint32_t width = 0, height = 0;
  UploadSlot& u = g.uploads[g.uploadSlot];
  {
    std::lock_guard<std::mutex> lock(g_frameMutex);
    if (g_frameSerial == g_frameUploaded || g_frameWidth == 0 || g_frameHeight == 0) {
      return;
    }
    width = g_frameWidth;
    height = g_frameHeight;
    if (u.cmd == VK_NULL_HANDLE) {
      VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      allocInfo.commandPool = g.pool;
      allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocInfo.commandBufferCount = 1;
      VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      if (!vk_ok(vkAllocateCommandBuffers(g.device, &allocInfo, &u.cmd), "vkAllocateCommandBuffers(upload)") ||
          !vk_ok(vkCreateFence(g.device, &fenceInfo, nullptr, &u.fence), "vkCreateFence(upload)")) {
        g_frameUploaded = g_frameSerial;
        return;
      }
    }
    if (u.pending) {
      // This slot's copy is two uploads old, long done in the normal case.
      vkWaitForFences(g.device, 1, &u.fence, VK_TRUE, UINT64_MAX);
      vkResetFences(g.device, 1, &u.fence);
      u.pending = false;
    }
    if (!ensure_swapchain(width, height) ||
        !ensure_host_buffer(u.buffer, u.memory, u.mapped, u.size, static_cast<VkDeviceSize>(width) * height * 4,
                            "upload")) {
      g_frameUploaded = g_frameSerial;
      return;
    }
    auto* dst = static_cast<uint8_t*>(u.mapped);
    const size_t bytes = static_cast<size_t>(width) * height * 4;
    if (!g.swapchainBgra) {
      std::memcpy(dst, g_frame.data(), bytes);
    } else {
      const uint8_t* src = g_frame.data();
      for (size_t i = 0; i < bytes; i += 4) {
        dst[i + 0] = src[i + 2];
        dst[i + 1] = src[i + 1];
        dst[i + 2] = src[i + 0];
        dst[i + 3] = src[i + 3];
      }
    }
    g_frameUploaded = g_frameSerial;
  }

  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (!xr_ok(xrAcquireSwapchainImage(g.swapchain, &acquire, &index), "xrAcquireSwapchainImage")) {
    return;
  }
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = XR_INFINITE_DURATION;
  if (!xr_ok(xrWaitSwapchainImage(g.swapchain, &wait), "xrWaitSwapchainImage")) {
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g.swapchain, &release);
    return;
  }
  const VkImage image = g.swapchainImages[index].image;

  VkCommandBuffer cmd = u.cmd;
  vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);

  VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toTransfer.srcAccessMask = 0;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image = image;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toTransfer);

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {width, height, 1};
  vkCmdCopyBufferToImage(cmd, u.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  // OpenXR wants color swapchain images back in COLOR_ATTACHMENT_OPTIMAL on release.
  VkImageMemoryBarrier toColor = toTransfer;
  toColor.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &toColor);
  vkEndCommandBuffer(cmd);

  // No wait: OpenXR only needs the copy submitted to the session's queue before the release; the
  // runtime orders its own reads after it.
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  if (vk_ok(vkQueueSubmit(g.queue, 1, &submit, u.fence), "vkQueueSubmit(upload)")) {
    u.pending = true;
  }
  g.uploadSlot = (g.uploadSlot + 1) % kUploadSlots;

  XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xr_ok(xrReleaseSwapchainImage(g.swapchain, &release), "xrReleaseSwapchainImage");
  g.swapchainHasImage = true;
  ++g.framesShown;
  g.uploadNsSinceStats += now_ns() - start;
}

// ---------------------------------------------------------------------------------------------
// 3D modes: eye images shared with Dawn, matrices, and the per-eye pass contract with glPlat.cpp
// ---------------------------------------------------------------------------------------------

// Which 3D layers the current mode draws. The flags match glPlat.cpp's XR_LAYER_*.
enum LayerKind { kLayerStadium, kLayerTable };
constexpr int kFlagSkipActors = 1;
constexpr int kFlagSkipSky = 2;
constexpr int kFlagTable = 4;

bool stereo_mode() { return g.tabletop || !g.mrMode; }

int layer_count() {
  if (!stereo_mode()) return 0;
  if (g.tabletop && !g.mrMode) return 2;  // Tabletop · Stadium: the stadium around you, then the field
  return 1;                               // Tabletop · Living Room (field) or Jumbotron · Stadium (stadium)
}

LayerKind layer_kind(int layer) {
  if (g.tabletop && g.mrMode) return kLayerTable;
  if (g.tabletop && !g.mrMode) return layer == 0 ? kLayerStadium : kLayerTable;
  return kLayerStadium;
}

// Row-vector 4x4 in the game's nlMatrix4 convention: v' = v * M, translation in row 3.
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

// Column-vector rotation matrix of a unit quaternion.
void quat_to_rc(const XrQuaternionf& q, float rc[3][3]) {
  const float x = q.x, y = q.y, z = q.z, w = q.w;
  rc[0][0] = 1 - 2 * (y * y + z * z); rc[0][1] = 2 * (x * y - w * z);     rc[0][2] = 2 * (x * z + w * y);
  rc[1][0] = 2 * (x * y + w * z);     rc[1][1] = 1 - 2 * (x * x + z * z); rc[1][2] = 2 * (y * z - w * x);
  rc[2][0] = 2 * (x * z - w * y);     rc[2][1] = 2 * (y * z + w * x);     rc[2][2] = 1 - 2 * (x * x + y * y);
}

// LOCAL space -> eye space, i.e. the inverse of the eye's pose: rotation part Rc, translation -p*Rc.
M4 m4_view_from_pose(const XrPosef& pose) {
  float rc[3][3];
  quat_to_rc(pose.orientation, rc);
  M4 r = m4_identity();
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.m[i][j] = rc[i][j];
  const float p[3] = {pose.position.x, pose.position.y, pose.position.z};
  for (int j = 0; j < 3; ++j) r.m[3][j] = -(p[0] * rc[0][j] + p[1] * rc[1][j] + p[2] * rc[2][j]);
  return r;
}

// Game world -> LOCAL space. The game is Z-up with the pitch in the XY plane; the room is Y-up
// looking down -Z. (x, y, z) -> (x, z, -y) keeps right-handedness and puts the game's +Y (across
// the pitch, away from the TV camera's side) in front of you. Then scale, turn about the room's
// up axis, and move to `origin`.
M4 m4_game_to_room(float scale, float yaw, const XrVector3f& origin) {
  // Row form of (x, y, z) -> (x, z, -y).
  const float gr[3][3] = {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
  // Row form of a rotation by `yaw` about +Y.
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

// GXSetProjection's perspective layout (C_MTXPerspective rows, clip z in [-w, 0]) from the eye's
// asymmetric field of view. Near/far are room meters.
void gx_projection(const XrFovf& fov, float n, float f, float out[16]) {
  const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
  const float u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
  std::memset(out, 0, sizeof(float) * 16);
  out[0 * 4 + 0] = 2.0f / (r - l);
  out[0 * 4 + 2] = (r + l) / (r - l);
  out[1 * 4 + 1] = 2.0f / (u - d);
  out[1 * 4 + 2] = (u + d) / (u - d);
  out[2 * 4 + 2] = -n / (f - n);
  out[2 * 4 + 3] = -(f * n) / (f - n);
  out[3 * 4 + 2] = -1.0f;
}

// The stadium layer's placement: you stand on the near touchline at centre field, feet on the
// floor, facing across the pitch. STRIKERS_XR_STADIUM_SCALE overrides the game-unit size (1 = 1 m).
float stadium_scale() {
  static float s_scale = 0.0f;
  if (s_scale == 0.0f) {
    const char* e = std::getenv("STRIKERS_XR_STADIUM_SCALE");
    s_scale = (e != nullptr && *e != '\0') ? static_cast<float>(std::atof(e)) : 1.0f;
    if (s_scale <= 0.0f) s_scale = 1.0f;
  }
  return s_scale;
}

// Where you stand in the life-size stadium: game y = kStandY, on the pitch 1.6 m inside the near
// touchline (the pitch is 24.2 wide), facing across it. r4-r10 stood at -15.5, just outside the
// electric fence, whose wall then filled the near view.
constexpr float kStandY = -10.5f;

M4 stadium_model() {
  // Game (0, kStandY, 0) lands on the floor under the start pose, whatever the (self-test) turn:
  // the stand point maps to (0, 0, -kStandY) before the turn and scale.
  const float s = stadium_scale(), yaw = g.stadiumYawDebug;
  const float c = -kStandY;
  return m4_game_to_room(s, yaw,
                         XrVector3f{-c * std::sin(yaw) * s, g.floorY, -c * std::cos(yaw) * s});
}

M4 table_model() { return m4_game_to_room(g.table.scale, g.table.yaw, g.table.position); }

// A game point on the Tabletop field, in LOCAL space.
XrVector3f table_point(float gx, float gy, float gz) {
  const M4 t = table_model();
  return XrVector3f{gx * t.m[0][0] + gy * t.m[1][0] + gz * t.m[2][0] + t.m[3][0],
                    gx * t.m[0][1] + gy * t.m[1][1] + gz * t.m[2][1] + t.m[3][1],
                    gx * t.m[0][2] + gy * t.m[1][2] + gz * t.m[2][2] + t.m[3][2]};
}

uint32_t find_device_local(uint32_t typeBits) {
  for (uint32_t i = 0; i < g.memProps.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) != 0 && (g.memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      return i;
    }
  }
  for (uint32_t i = 0; i < g.memProps.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) != 0) return i;
  }
  return UINT32_MAX;
}

// One exported eye image (TPVR's createExportedImage, trimmed). Same VkImageCreateInfo on both
// sides; MUTABLE + a two-entry format list so Dawn can render through an RGBA8Unorm view of the
// sRGB image while Adreno keeps its framebuffer compression.
bool create_eye_image(EyeImage& out, VkFormat srgb, VkFormat unorm) {
  const VkFormat viewFormats[2] = {srgb, unorm};
  VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
  formatList.viewFormatCount = 2;
  formatList.pViewFormats = viewFormats;
  VkExternalMemoryImageCreateInfo extInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  extInfo.pNext = &formatList;
  extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.pNext = &extInfo;
  ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = srgb;
  ci.extent = {g.eyeWidth, g.eyeHeight, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
             VK_IMAGE_USAGE_SAMPLED_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!vk_ok(vkCreateImage(g.device, &ci, nullptr, &out.image), "vkCreateImage(eye)")) return false;

  VkMemoryDedicatedRequirements dedicatedReqs{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
  VkMemoryRequirements2 reqs2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicatedReqs};
  VkImageMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
  reqInfo.image = out.image;
  vkGetImageMemoryRequirements2(g.device, &reqInfo, &reqs2);
  const VkMemoryRequirements& reqs = reqs2.memoryRequirements;
  const uint32_t typeIndex = find_device_local(reqs.memoryTypeBits);
  if (typeIndex == UINT32_MAX) {
    XERR("no memory type for the eye image");
    return false;
  }
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.image = out.image;
  VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated};
  exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exportInfo};
  alloc.allocationSize = reqs.size;
  alloc.memoryTypeIndex = typeIndex;
  if (!vk_ok(vkAllocateMemory(g.device, &alloc, nullptr, &out.memory), "vkAllocateMemory(eye)") ||
      !vk_ok(vkBindImageMemory(g.device, out.image, out.memory, 0), "vkBindImageMemory(eye)")) {
    return false;
  }

  static PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;
  if (getMemoryFd == nullptr) {
    getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(g.device, "vkGetMemoryFdKHR"));
  }
  if (getMemoryFd == nullptr) {
    XERR("vkGetMemoryFdKHR unavailable");
    return false;
  }
  VkMemoryGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
  fdInfo.memory = out.memory;
  fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  int fd = -1;
  if (!vk_ok(getMemoryFd(g.device, &fdInfo, &fd), "vkGetMemoryFdKHR") || fd < 0) return false;

  // Dawn side, inside an error scope: Aurora treats any uncaptured Dawn error as fatal.
  auto& device = aurora::webgpu::g_device;
  device.PushErrorScope(wgpu::ErrorFilter::Validation);
  {
    wgpu::SharedTextureMemoryOpaqueFDDescriptor fdDesc{};
    fdDesc.vkImageCreateInfo = &ci;
    fdDesc.memoryFD = fd;
    fdDesc.memoryTypeIndex = typeIndex;
    fdDesc.allocationSize = reqs.size;
    fdDesc.dedicatedAllocation = true;
    wgpu::SharedTextureMemoryDescriptor desc{};
    desc.nextInChain = &fdDesc;
    out.shared = device.ImportSharedTextureMemory(&desc);
    const wgpu::TextureFormat viewFormat = aurora::gfx::color_format();
    wgpu::TextureDescriptor tex{};
    tex.format = wgpu::TextureFormat::RGBA8UnormSrgb;
    tex.size = {g.eyeWidth, g.eyeHeight, 1};
    tex.usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::RenderAttachment |
                wgpu::TextureUsage::TextureBinding;
    tex.viewFormatCount = 1;
    tex.viewFormats = &viewFormat;
    if (out.shared) out.texture = out.shared.CreateTexture(&tex);
    if (out.texture) {
      wgpu::TextureViewDescriptor view{};
      view.format = viewFormat;
      view.dimension = wgpu::TextureViewDimension::e2D;
      view.mipLevelCount = 1;
      view.arrayLayerCount = 1;
      out.renderView = out.texture.CreateView(&view);
    }
  }
  close(fd);  // Dawn dup()s it on import
  bool done = false, failed = false;
  std::string message;
  const auto future = device.PopErrorScope(
      wgpu::CallbackMode::WaitAnyOnly, [&](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView msg) {
        done = true;
        if (status != wgpu::PopErrorScopeStatus::Success || type != wgpu::ErrorType::NoError) {
          failed = true;
          message = std::string{std::string_view{msg}};
        }
      });
  aurora::webgpu::g_instance.WaitAny(future, 5000000000ull);
  if (!done || failed || !out.texture || !out.renderView) {
    XERR("Dawn rejected the eye image import: %s", done ? message.c_str() : "error scope timed out");
    return false;
  }
  VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  return vk_ok(vkCreateSemaphore(g.device, &semInfo, nullptr, &out.dawnDone), "vkCreateSemaphore(eye)");
}

bool create_stereo_resources() {
  if (g.stereoFailed) return false;
  if (g.eyeSwapchain[0] != XR_NULL_HANDLE) return true;
  auto fail = [](const char* why) {
    XERR("3D modes unavailable: %s", why);
    g.stereoFailed = true;
    return false;
  };
  if (!aurora::webgpu::g_vulkanSharedImageExportSupported) return fail("Dawn has no opaque-fd/sync-fd sharing");
  if (!g.hasExtMemFd || !g.hasExtSemFd) return fail("XR device lacks external memory/semaphore fd");
  if (g.eyeWidth == 0 || g.eyeHeight == 0) return fail("no eye size from the runtime");
  if (aurora::gfx::color_format() != wgpu::TextureFormat::RGBA8Unorm) return fail("scene format is not RGBA8Unorm");

  for (int eye = 0; eye < 2; ++eye) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = VK_FORMAT_R8G8B8A8_SRGB;
    info.sampleCount = 1;
    info.width = g.eyeWidth;
    info.height = g.eyeHeight;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!xr_ok(xrCreateSwapchain(g.session, &info, &g.eyeSwapchain[eye]), "xrCreateSwapchain(eye)")) {
      return fail("eye swapchain");
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(g.eyeSwapchain[eye], 0, &count, nullptr);
    g.eyeImages[eye].assign(count, XrSwapchainImageVulkanKHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(g.eyeSwapchain[eye], count, &count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(g.eyeImages[eye].data()));
  }
  for (uint32_t s = 0; s < kStereoSlots; ++s) {
    StereoSlot& slot = g.slots[s];
    for (int eye = 0; eye < 2; ++eye) {
      if (!create_eye_image(slot.eye[eye], VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM)) {
        return fail("eye image");
      }
    }
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = g.pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!vk_ok(vkAllocateCommandBuffers(g.device, &allocInfo, &slot.cmd), "vkAllocateCommandBuffers(slot)") ||
        !vk_ok(vkCreateFence(g.device, &fenceInfo, nullptr, &slot.fence), "vkCreateFence(slot)")) {
      return fail("slot sync objects");
    }
  }
  XLOG("3D modes ready: %ux%u per eye, GPU-direct hand-off", g.eyeWidth, g.eyeHeight);
  return true;
}

// Dawn's end-of-access fence as a VkSemaphore the XR queue can wait on. False: no usable fence
// (knownComplete says whether that is because the work is already done).
bool import_dawn_fence(const wgpu::SharedTextureMemoryEndAccessState& end, VkSemaphore semaphore, bool* knownComplete) {
  *knownComplete = false;
  if (end.fenceCount == 0 || end.fences == nullptr) return false;
  wgpu::SharedFenceExportInfo typeQuery{};
  end.fences[0].ExportInfo(&typeQuery);
  int dawnFd = -1;
  VkExternalSemaphoreHandleTypeFlagBits handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  if (typeQuery.type == wgpu::SharedFenceType::SyncFD) {
    wgpu::SharedFenceSyncFDExportInfo info{};
    wgpu::SharedFenceExportInfo exportInfo{};
    exportInfo.nextInChain = &info;
    end.fences[0].ExportInfo(&exportInfo);
    dawnFd = info.handle;
    if (dawnFd < 0) {
      *knownComplete = true;  // -1: already signaled
      return false;
    }
  } else if (typeQuery.type == wgpu::SharedFenceType::VkSemaphoreOpaqueFD) {
    wgpu::SharedFenceVkSemaphoreOpaqueFDExportInfo info{};
    wgpu::SharedFenceExportInfo exportInfo{};
    exportInfo.nextInChain = &info;
    end.fences[0].ExportInfo(&exportInfo);
    dawnFd = info.handle;
    handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  } else {
    return false;
  }
  if (dawnFd < 0) return false;
  const int importFd = dup(dawnFd);  // Dawn keeps and closes its own; Vulkan takes ownership of ours
  if (importFd < 0) return false;
  static PFN_vkImportSemaphoreFdKHR importSemaphoreFd = nullptr;
  if (importSemaphoreFd == nullptr) {
    importSemaphoreFd =
        reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(g.device, "vkImportSemaphoreFdKHR"));
  }
  VkImportSemaphoreFdInfoKHR info{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
  info.semaphore = semaphore;
  info.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
  info.handleType = handleType;
  info.fd = importFd;
  if (importSemaphoreFd == nullptr || importSemaphoreFd(g.device, &info) != VK_SUCCESS) {
    close(importFd);
    return false;
  }
  return true;
}

// Writes the left eye of `slot` to /storage/emulated/0/Strikers/xr_eye.ppm (self-test aid).
void dump_eye(StereoSlot& slot, const char* modeName) {
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(g.eyeWidth) * g.eyeHeight * 4;
  if (!ensure_staging(bytes)) return;
  VkCommandBuffer cmd = g.cmd;
  vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {g.eyeWidth, g.eyeHeight, 1};
  vkCmdCopyImageToBuffer(cmd, slot.eye[0].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.staging, 1, &region);
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  if (!vk_ok(vkQueueSubmit(g.queue, 1, &submit, g.fence), "vkQueueSubmit(dump)")) return;
  vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
  vkResetFences(g.device, 1, &g.fence);
  FILE* f = std::fopen("/storage/emulated/0/Strikers/xr_eye.ppm", "wb");
  if (f == nullptr) return;
  std::fprintf(f, "P6\n%u %u\n255\n", g.eyeWidth, g.eyeHeight);
  const auto* px = static_cast<const uint8_t*>(g.stagingMapped);
  std::vector<uint8_t> row(static_cast<size_t>(g.eyeWidth) * 3);
  // Over passthrough the alpha matters: composite like the compositor (premultiplied) over magenta,
  // so anything see-through shows pink.
  const bool overBackground = g.mrMode;
  for (uint32_t y = 0; y < g.eyeHeight; ++y) {
    for (uint32_t x = 0; x < g.eyeWidth; ++x) {
      const uint8_t* p = px + (static_cast<size_t>(y) * g.eyeWidth + x) * 4;
      const int clear = overBackground ? 255 - p[3] : 0;
      row[x * 3 + 0] = static_cast<uint8_t>(std::min(255, p[0] + clear));
      row[x * 3 + 1] = p[1];
      row[x * 3 + 2] = static_cast<uint8_t>(std::min(255, p[2] + clear));
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  XLOG("dumped left eye (%s) to xr_eye.ppm", modeName);
}

// After synchronize(): hands this frame's eye images back from Dawn and blits them into the eye
// swapchains, gated on Dawn's fences. No CPU wait in the normal case.
void finish_stereo_frame() {
  StereoSlot& slot = g.slots[g.slot];
  VkSemaphore waits[2];
  uint32_t waitCount = 0;
  bool needCpuWait = false;
  for (int eye = 0; eye < 2; ++eye) {
    EyeImage& img = slot.eye[eye];
    if (!img.accessOpen) continue;
    wgpu::SharedTextureMemoryVkImageLayoutEndState layoutEnd{};
    wgpu::SharedTextureMemoryEndAccessState endState{};
    endState.nextInChain = &layoutEnd;
    img.shared.EndAccess(img.texture, &endState);
    img.accessOpen = false;
    img.dawnEndLayout = static_cast<VkImageLayout>(layoutEnd.newLayout);
    bool knownComplete = false;
    if (import_dawn_fence(endState, img.dawnDone, &knownComplete)) {
      waits[waitCount++] = img.dawnDone;
    } else if (!knownComplete) {
      needCpuWait = true;
    }
  }
  if (needCpuWait) {
    const uint64_t cpuWaitStart = now_ns();
    bool workDone = false;
    const auto future = aurora::webgpu::g_queue.OnSubmittedWorkDone(
        wgpu::CallbackMode::WaitAnyOnly, [&workDone](wgpu::QueueWorkDoneStatus, wgpu::StringView) { workDone = true; });
    aurora::webgpu::g_instance.WaitAny(future, 5000000000ull);
    g.cpuWaitNs += now_ns() - cpuWaitStart;
  }

  const uint64_t swapStart = now_ns();
  uint32_t index[2] = {0, 0};
  for (int eye = 0; eye < 2; ++eye) {
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xr_ok(xrAcquireSwapchainImage(g.eyeSwapchain[eye], &acquire, &index[eye]), "xrAcquireSwapchainImage(eye)");
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    xr_ok(xrWaitSwapchainImage(g.eyeSwapchain[eye], &wait), "xrWaitSwapchainImage(eye)");
  }
  g.swapNs += now_ns() - swapStart;

  vkResetCommandBuffer(slot.cmd, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(slot.cmd, &begin);
  for (int eye = 0; eye < 2; ++eye) {
    EyeImage& img = slot.eye[eye];
    const VkImage dst = g.eyeImages[eye][index[eye]].image;
    VkImageMemoryBarrier barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = img.dawnEndLayout;  // keeps Dawn's writes
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = img.image;
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barriers[1] = barriers[0];
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].image = dst;
    vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, barriers);
    // A same-format blit is a byte copy, and records in microseconds where vkCmdCopyImage into the
    // runtime's gralloc image costs milliseconds on Adreno (TPVR's measurement).
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(g.eyeWidth), static_cast<int32_t>(g.eyeHeight), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(g.eyeWidth), static_cast<int32_t>(g.eyeHeight), 1};
    vkCmdBlitImage(slot.cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);
    VkImageMemoryBarrier toColor = barriers[1];
    toColor.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &toColor);
  }
  vkEndCommandBuffer(slot.cmd);
  const VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = waitCount;
  submit.pWaitSemaphores = waits;
  submit.pWaitDstStageMask = waitStages;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &slot.cmd;
  if (vk_ok(vkQueueSubmit(g.queue, 1, &submit, slot.fence), "vkQueueSubmit(eyes)")) {
    slot.pending = true;
  }
  for (int eye = 0; eye < 2; ++eye) {
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xr_ok(xrReleaseSwapchainImage(g.eyeSwapchain[eye], &release), "xrReleaseSwapchainImage(eye)");
    g.projViews[eye] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    g.projViews[eye].pose = g.views[eye].pose;
    g.projViews[eye].fov = g.views[eye].fov;
    g.projViews[eye].subImage.swapchain = g.eyeSwapchain[eye];
    g.projViews[eye].subImage.imageRect.offset = {0, 0};
    g.projViews[eye].subImage.imageRect.extent = {static_cast<int32_t>(g.eyeWidth), static_cast<int32_t>(g.eyeHeight)};
    g.projViews[eye].subImage.imageArrayIndex = 0;
  }
  g.hasProjection = true;

  if (g.dumpRequested) {
    g.dumpRequested = false;
    vkWaitForFences(g.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    dump_eye(slot, mode_name());
  }
  g.slot = (g.slot + 1) % kStereoSlots;
}

void set_perf_levels(int cpu, int gpu);

// Self-test hooks, polled about once a second: a file /storage/emulated/0/Strikers/xr_mode holding
// "jumbotron mr", "jumbotron vr", "tabletop mr" or "tabletop vr" sets the mode (then is deleted);
// a file xr_dump requests one left-eye dump.
void poll_triggers() {
  const uint64_t t = now_ns();
  if (t - g.lastTriggerPollNs < 1000000000ull) return;
  g.lastTriggerPollNs = t;
  const char* modePath = "/storage/emulated/0/Strikers/xr_mode";
  if (FILE* f = std::fopen(modePath, "r")) {
    char buf[64] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::remove(modePath);
    buf[n] = '\0';
    const std::string s(buf);
    g.tabletop = s.find("tabletop") != std::string::npos;
    set_mr_mode(s.find("vr") == std::string::npos);
  }
  const char* dumpPath = "/storage/emulated/0/Strikers/xr_dump";
  struct stat st {};
  if (stat(dumpPath, &st) == 0) {
    std::remove(dumpPath);
    g.dumpRequested = true;
    // And what the screen / scoreboard quad shows (the newest game frame read back).
    std::lock_guard<std::mutex> lock(g_frameMutex);
    if (g_frameWidth > 0 && g_frameHeight > 0) {
      if (FILE* f = std::fopen("/storage/emulated/0/Strikers/xr_screen.ppm", "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", g_frameWidth, g_frameHeight);
        std::vector<uint8_t> row(static_cast<size_t>(g_frameWidth) * 3);
        for (uint32_t y = 0; y < g_frameHeight; ++y) {
          const uint8_t* src = g_frame.data() + static_cast<size_t>(y) * g_frameWidth * 4;
          for (uint32_t x = 0; x < g_frameWidth; ++x) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
          }
          std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
      }
    }
  }
  // Tuning: "viewmask <hex>" = which GLV views the eye sends draw (bit n = view n).
  const char* tunePath = "/storage/emulated/0/Strikers/xr_tune";
  if (FILE* f = std::fopen(tunePath, "r")) {
    char line[64] = {};
    while (std::fgets(line, sizeof(line), f) != nullptr) {
      if (std::strncmp(line, "viewmask_table", 14) == 0) {
        g.eyeViewMaskTable = static_cast<uint32_t>(std::strtoul(line + 14, nullptr, 16));
      } else if (std::strncmp(line, "viewmask_stadium", 16) == 0) {
        g.eyeViewMaskStadium = static_cast<uint32_t>(std::strtoul(line + 16, nullptr, 16));
      } else if (std::strncmp(line, "viewmask", 8) == 0) {
        g.eyeViewMask = static_cast<uint32_t>(std::strtoul(line + 8, nullptr, 16));
      } else if (std::strncmp(line, "skip", 4) == 0) {
        // "skip <crowd> <stands> <stadiumfield>", each 0/1
        int crowd = 0, stands = 0, field = 0;
        std::sscanf(line + 4, "%d %d %d", &crowd, &stands, &field);
        PortXrTuneSkip(crowd, stands, field);
      } else if (std::strncmp(line, "lod", 3) == 0) {
        g.lodMinPx = static_cast<float>(std::atof(line + 3));
      } else if (std::strncmp(line, "tallfield", 9) == 0) {
        g.skipTallField = std::atoi(line + 9) != 0;
      } else if (std::strncmp(line, "msaa", 4) == 0) {
        int table = 4, stadium = 4;
        std::sscanf(line + 4, "%d %d", &table, &stadium);
        g.msaaTable = table > 1 ? 4 : 1;
        g.msaaStadium = stadium > 1 ? 4 : 1;
      } else if (std::strncmp(line, "decode", 6) == 0) {
        // "decode <0-2>": vertex decode-once off / players / everything eligible
        aurora_xr_set_vertex_decode(std::atoi(line + 6));
      } else if (std::strncmp(line, "aniso", 5) == 0) {
        // "aniso <1-16>": the minimum anisotropic filtering (new samplers from the next draw on)
        aurora_xr_set_texture_anisotropy(static_cast<unsigned>(std::max(1, std::atoi(line + 5))));
      } else if (std::strncmp(line, "perf", 4) == 0) {
        // "perf <cpu> <gpu>": 0 power savings, 1 sustained low, 2 sustained high, 3 boost
        int cpu = 2, gpu = 2;
        std::sscanf(line + 4, "%d %d", &cpu, &gpu);
        set_perf_levels(cpu, gpu);
      } else if (std::strncmp(line, "charscale", 9) == 0) {
        g.charScale = std::max(1.0f, std::min(3.0f, static_cast<float>(std::atof(line + 9))));
      } else if (std::strncmp(line, "tablehere", 9) == 0) {
        g.bringTable = true;
      } else if (std::strncmp(line, "probe", 5) == 0) {
        PortXrProbe();
      } else if (std::strncmp(line, "stadiumyaw", 10) == 0) {
        g.stadiumYawDebug = static_cast<float>(std::atof(line + 10)) * 3.14159265f / 180.0f;
      } else if (std::strncmp(line, "reset", 5) == 0) {
        g.eyeViewMask = g.eyeViewMaskTable = g.eyeViewMaskStadium = 0xFFFFFFFFu;
        PortXrTuneSkip(0, 0, 0);
        g.lodMinPx = 0.0f;
        g.skipTallField = false;
      }
      XLOG("tune: %s", line);
    }
    std::fclose(f);
    std::remove(tunePath);
  }
  // Celebration self-test: "goal <class> [super]", "replay", "end", "skip", "dumpfx" (one per line).
  const char* fxPath = "/storage/emulated/0/Strikers/xr_fx";
  if (FILE* f = std::fopen(fxPath, "r")) {
    char line[64];
    std::vector<std::string> cmds;
    while (std::fgets(line, sizeof(line), f) != nullptr) cmds.emplace_back(line);
    std::fclose(f);
    std::remove(fxPath);
    for (const std::string& c : cmds) {
      if (c.rfind("goal", 0) == 0) {
        const int who = std::atoi(c.c_str() + 4);
        PortXrGoal(0, who, c.find("super") != std::string::npos ? 1 : 0, 0, 0, -1);
      } else if (c.rfind("replay", 0) == 0) {
        PortXrCelebrationPhase(1);
      } else if (c.rfind("end", 0) == 0) {
        PortXrCelebrationPhase(2);
      } else if (c.rfind("skip", 0) == 0) {
        PortXrCelebrationPhase(3);
      } else if (c.rfind("dumpfx", 0) == 0 && g.fxReady) {
        xrfx::request_dump("/storage/emulated/0/Strikers/xr_fx.ppm");
      } else if (c.rfind("item", 0) == 0) {
        // A green shell for your team, then X pressed a second later through the Touch path.
        g.testItem = true;
        g.testPressXNs = now_ns() + 1500000000ull;
      } else if (c.rfind("wipe", 0) == 0) {
        // The game's fade_to_white_and_back wipe, the effects layer dumped at the flash's height.
        g.testWipe = true;
        ensure_fx();
        if (g.fxReady) xrfx::dump_at_flash();
      } else if (c.rfind("sts", 0) == 0) {
        // The Super Strike meter over the player you control for 3 s, no shot taken.
        g.testSts = true;
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Session events
// ---------------------------------------------------------------------------------------------

// The tid of this process's thread whose name starts with `prefix`, 0 when there is none.
pid_t find_thread(const char* prefix) {
  DIR* dir = opendir("/proc/self/task");
  if (dir == nullptr) return 0;
  pid_t found = 0;
  while (dirent* e = readdir(dir)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
    if (FILE* f = std::fopen(path, "r")) {
      char name[32] = {};
      std::fgets(name, sizeof(name), f);
      std::fclose(f);
      if (std::strncmp(name, prefix, std::strlen(prefix)) == 0) {
        found = static_cast<pid_t>(std::atoi(e->d_name));
        break;
      }
    }
  }
  closedir(dir);
  return found;
}

// CPU / GPU levels: 0 power savings, 1 sustained low, 2 sustained high, 3 boost (xr_tune "perf").
void set_perf_levels(int cpu, int gpu) {
  if (!g.hasPerfSettingsExt) return;
  PFN_xrPerfSettingsSetPerformanceLevelEXT setLevel = nullptr;
  if (!get_proc(g.instance, "xrPerfSettingsSetPerformanceLevelEXT", &setLevel)) return;
  static const XrPerfSettingsLevelEXT kLevels[] = {
      XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT,
      XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT};
  xr_ok(setLevel(g.session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, kLevels[std::clamp(cpu, 0, 3)]),
        "xrPerfSettingsSetPerformanceLevelEXT(CPU)");
  xr_ok(setLevel(g.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, kLevels[std::clamp(gpu, 0, 3)]),
        "xrPerfSettingsSetPerformanceLevelEXT(GPU)");
}

// Without these the runtime keeps the app on its default (dynamic, low) clocks and schedules the
// game and render threads like any background thread. Five sends a frame in Tabletop · Stadium
// need the sustained-high CPU and GPU levels and the big cores.
void apply_performance_settings() {
  set_perf_levels(2, 2);
  if (g.hasThreadSettingsExt) {
    PFN_xrSetAndroidApplicationThreadKHR setThread = nullptr;
    if (get_proc(g.instance, "xrSetAndroidApplicationThreadKHR", &setThread)) {
      // This is the game thread: it runs the game, the sends and the XR frame loop.
      const pid_t game = gettid();
      const pid_t render = find_thread("Aurora render");
      const pid_t fifo = find_thread("Aurora FIFO");  // as TPVR does: RENDERER_WORKER
      xr_ok(setThread(g.session, XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, game),
            "xrSetAndroidApplicationThreadKHR(game)");
      if (render != 0) {
        xr_ok(setThread(g.session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, render),
              "xrSetAndroidApplicationThreadKHR(render)");
      }
      if (fifo != 0) {
        xr_ok(setThread(g.session, XR_ANDROID_THREAD_TYPE_RENDERER_WORKER_KHR, fifo),
              "xrSetAndroidApplicationThreadKHR(fifo)");
      }
      XLOG("thread hints: game tid %d, render worker tid %d, fifo tid %d", static_cast<int>(game),
           static_cast<int>(render), static_cast<int>(fifo));
    }
  }
}

void on_session_began() {
  init_refresh_rate();
  apply_performance_settings();
  set_mr_mode(g.mrMode);
}

void poll_events() {
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(g.instance, &event) == XR_SUCCESS) {
    switch (event.type) {
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
      const auto* e = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
      g.sessionState = e->state;
      XLOG("session state %d", static_cast<int>(e->state));
      switch (e->state) {
      case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (xr_ok(xrBeginSession(g.session, &begin), "xrBeginSession")) {
          g.running = true;
          on_session_began();
        }
        break;
      }
      case XR_SESSION_STATE_STOPPING:
        xr_ok(xrEndSession(g.session), "xrEndSession");
        g.running = false;
        g.focused = false;
        break;
      case XR_SESSION_STATE_FOCUSED:
        g.focused = true;
        break;
      case XR_SESSION_STATE_VISIBLE:
      case XR_SESSION_STATE_SYNCHRONIZED:
      case XR_SESSION_STATE_IDLE:
        g.focused = false;
        break;
      case XR_SESSION_STATE_EXITING:
      case XR_SESSION_STATE_LOSS_PENDING:
        g.running = false;
        g.focused = false;
        g.quit = true;
        break;
      default:
        break;
      }
      break;
    }
    case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
      g.quit = true;
      break;
    default:
      break;
    }
    event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
  }
}

void log_stats() {
  const uint64_t t = now_ns();
  if (g.lastStatsNs == 0) {
    g.lastStatsNs = t;
    return;
  }
  if (t - g.lastStatsNs < 5000000000ull) {
    return;
  }
  const double secs = static_cast<double>(t - g.lastStatsNs) / 1e9;
  const double uploadMs =
      g.gameFramesSinceStats ? static_cast<double>(g.uploadNsSinceStats) / 1e6 / g.gameFramesSinceStats : 0.0;
  unsigned long buffers = 0, underruns = 0;
  int everNonSilent = 0;
  PortAudioStats(&buffers, &underruns, &everNonSilent);
  const double frameEndMs =
      g.gameFramesSinceStats ? static_cast<double>(g.stereoNsSinceStats) / 1e6 / g.gameFramesSinceStats : 0.0;
  const double eyeSendMs =
      g.gameFramesSinceStats ? static_cast<double>(g.eyeSendNsSinceStats) / 1e6 / g.gameFramesSinceStats : 0.0;
  uint32_t poolPeak[4] = {}, poolSize[4] = {};
  aurora_gfx_pool_stats(poolPeak, poolSize, 4);
  const auto pct = [&](int i) { return poolSize[i] ? 100.0 * poolPeak[i] / poolSize[i] : 0.0; };
  XLOG("stats: display %.1f fps, game %.1f fps, upload %.2f ms, frame-end %.2f ms, eye-send %.2f ms, "
       "audio underruns +%lu, %s, 3D %s, screen %d, rumble %llu, pools v%.0f%% u%.0f%% i%.0f%% s%.0f%%, "
       "layers dropped %llu, culled %.0f/frame",
       g.displayFramesSinceStats / secs, g.gameFramesSinceStats / secs, uploadMs, frameEndMs, eyeSendMs,
       underruns - g.lastUnderruns, mode_name(), g.hasProjection ? "on" : "off", g.screenPreset,
       static_cast<unsigned long long>(g.rumblePulses), pct(0), pct(1), pct(2), pct(3),
       static_cast<unsigned long long>(g.layersDroppedSinceStats),
       g.gameFramesSinceStats ? static_cast<double>(PortXrLayersTakeCulled()) / g.gameFramesSinceStats : 0.0);
  uint32_t fxParts = 0, fxIndices = 0;
  if (g.fxReady && xrfx::take_stats(&fxParts, &fxIndices)) {
    XLOG("fx: up to %u pieces, %u mesh indices per eye", fxParts, fxIndices);
  }
  g.rumblePulses = 0;
  g.layersDroppedSinceStats = 0;
  if (g.gameFramesSinceStats > 0) {
    const double n = static_cast<double>(g.gameFramesSinceStats) * 1e6;
    XLOG("loop per game frame (ms): xrWaitFrame %.2f (all display frames), PortXrBeginFrame %.2f, "
         "aurora_begin_frame %.2f, tasks %.2f, aurora_end_frame %.2f, PortXrEndFrame %.2f",
         g.xrWaitNsSinceStats / n, g.loopNs[0] / n, g.loopNs[1] / n, g.loopNs[2] / n, g.loopNs[3] / n,
         g.loopNs[4] / n);
    uint64_t w[8] = {};
    aurora_xr_take_waits(w, 8);
    XLOG("waits per game frame (ms): fifo-drain %.2f, queue-full %.2f, sync %.2f, frame-slot %.2f, staging %.2f, "
         "pipeline %.2f, eye-fence %.2f, swapchain %.2f, cpu-wait %.2f | fx %.2f, xrEndFrame %.2f | busy: fifo "
         "%.2f, render %.2f",
         w[0] / n, w[1] / n, w[2] / n, w[3] / n, w[4] / n, w[5] / n, g.eyeFenceNs / n, g.swapNs / n,
         g.cpuWaitNs / n, g.fxNs / n, g.xrEndNs / n, w[6] / n, w[7] / n);
    uint32_t d[6] = {};
    aurora_xr_take_decode_stats(d);
    const double frames = static_cast<double>(g.gameFramesSinceStats);
    XLOG("decode per game frame: %.1f draws decoded, %.1f reused, %.0f vertices, %.2f ms CPU | %.1f waiting for "
         "their pipeline, %.1f not eligible",
         d[0] / frames, d[1] / frames, d[2] / frames, d[5] / 1000.0 / frames, d[3] / frames, d[4] / frames);
  }
  g.eyeFenceNs = g.swapNs = g.cpuWaitNs = g.fxNs = g.xrEndNs = 0;
  std::memset(g.loopNs, 0, sizeof(g.loopNs));
  g.xrWaitNsSinceStats = 0;
  g.lastUnderruns = underruns;
  g.stereoNsSinceStats = 0;
  g.eyeSendNsSinceStats = 0;
  g.lastStatsNs = t;
  g.displayFramesSinceStats = 0;
  g.gameFramesSinceStats = 0;
  g.uploadNsSinceStats = 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------------------------

extern "C" int PortXrWanted(void) {
  const char* off = std::getenv("STRIKERS_XR");
  return (off != nullptr && off[0] == '0') ? 0 : 1;
}

extern "C" int PortXrInit(void) {
  if (g.active) {
    return 1;
  }
  if (!PortXrWanted()) {
    XLOG("STRIKERS_XR=0: flat mode");
    return 0;
  }
  XLOG("%s starting", kBuildTag);

  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr || env->GetJavaVM(&g.vm) != JNI_OK) {
    XERR("no JNI environment or activity from SDL");
    return 0;
  }
  g.activity = env->NewGlobalRef(activity);
  env->DeleteLocalRef(activity);

  if (!init_loader() || !create_instance() || !create_vulkan_device() || !create_session() || !create_actions()) {
    XERR("OpenXR bring-up failed; running flat");
    PortXrShutdown();
    return 0;
  }
  create_passthrough();

  aurora_xr_set_surface_suppressed(true);
  aurora_xr_set_frame_callback(&on_aurora_frame, nullptr);
  // The immersive Activity's window is panel-sized (4128x2208 on Quest 3), and the port's render
  // target follows the window (launch.cpp PortFollowRenderScale) up to 3x: 2512x1344, read back
  // every frame. 2x is 896 rows, about 1600x896 on the 16:9 screen. A res_scale the player set wins.
  setenv("STRIKERS_RES_SCALE", "2.0", 0);
  // 50 ms of audio queued instead of 30: see audio_out.cpp target_buffers().
  setenv("STRIKERS_AUDIO_BUFFERS", "10", 0);
  // The Tabletop pitch is seen small and at a slant: 8x anisotropic filtering on mipmapped textures
  // keeps the grass and markings from shimmering (Lance, 2026-10-10: grainy). STRIKERS_XR_ANISO.
  const char* aniso = std::getenv("STRIKERS_XR_ANISO");
  aurora_xr_set_texture_anisotropy(aniso != nullptr && *aniso != '\0' ? static_cast<unsigned>(std::atoi(aniso)) : 8u);
  // r38: vertices are decoded to floats once per frame and reused by every send, instead of every
  // eye, binning pass and tile unpacking the GameCube bytes (Aurora vertex_decode.hpp). Measured
  // (r38c, same session): Tabletop · Living Room 48-51 -> 69-72 fps (game 60), Tabletop · Stadium
  // 26 -> 40-45, Jumbotron · Stadium 33 -> 48; identical pictures. 0 off, 1 players only,
  // 2 everything eligible (default). STRIKERS_XR_DECODE, xr_tune "decode <n>".
  const char* decode = std::getenv("STRIKERS_XR_DECODE");
  aurora_xr_set_vertex_decode(decode != nullptr && *decode != '\0' ? std::atoi(decode) : 2);
  g.active = true;
  XLOG("%s ready (session created, waiting for READY)", kBuildTag);
  return 1;
}

extern "C" int PortXrActive(void) { return g.active ? 1 : 0; }

// --- The per-eye re-send contract with glPlat.cpp (see glx_XrSendEyes) and world.cpp ---

extern "C" int PortXrStereoWorld(void) { return (g.active && stereo_mode() && !g.stereoFailed) ? 1 : 0; }

// Tabletop · Stadium shows the whole stadium life-size around you, so the gameplay camera's
// "drop the stands between me and the pitch" must not apply. Jumbotron · Stadium keeps it: there the
// broadcast view on the big screen is the main picture and must not be blocked.
extern "C" int PortXrStereoFullStands(void) {
  return (PortXrStereoWorld() && g.tabletop && !g.mrMode) ? 1 : 0;
}

namespace {

// Pool guard. The eye sends re-push vertices, indices, uniforms and storage arrays, so five sends
// a frame can outgrow Aurora's fixed pools, and an overflow aborts the game (r4, r6). The cost of
// one eye send of each layer kind is measured as it happens; a frame whose sends would not fit
// drops the stadium layer first, then its 3D altogether (the last pair is shown again).
constexpr double kPoolMargin = 1.25;

bool layers_fit(const uint32_t used[kPools], const int* kinds, int count) {
  for (int p = 0; p < kPools; ++p) {
    if (g.poolReserved[p] == 0) continue;
    double need = used[p];
    for (int l = 0; l < count; ++l) need += 2.0 * g.perSend[kinds[l]][p] * kPoolMargin;
    if (p == 0) need += g.decodedPerFrame * kPoolMargin;  // the decoded players, once per frame
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
    est = delta > est ? delta : est * 0.99f + delta * 0.01f;  // rise at once, fall slowly
  }
}

}  // namespace

extern "C" int PortXrStereoFrameBegin(void) {
  if (!g.active || !g.frameBegun || !g.viewsValid || !stereo_mode() || g.stereoFailed) {
    return 0;
  }
  // Online, a headset that cannot hold 60 game frames a second would slow the shared match down for
  // both (fixed step). Then its 3D view is redrawn every other game frame (the compositor reprojects
  // the last pair for head motion) and the match keeps its speed: presentation stays per player.
  if (PortNetOnline() && g.onlineHalfRate && (g.onlineParity++ & 1) != 0) {
    return 0;
  }
  if (g.poolReserved[0] == 0) {
    aurora_gfx_pool_stats(nullptr, g.poolReserved, kPools);
  }
  {
    // The previous game frame's decoded bytes (Aurora counts them per frame).
    const float lastFrame = static_cast<float>(aurora_xr_decoded_bytes_last_frame());
    g.decodedPerFrame = lastFrame > g.decodedPerFrame ? lastFrame : g.decodedPerFrame * 0.99f + lastFrame * 0.01f;
  }
  uint32_t used[kPools];
  aurora_gfx_pool_usage(used, kPools);
  for (int k = 0; k < 2; ++k) {
    if (g.perSend[k][0] == 0.0f) {
      // Until measured: half of the game view's own send (r5/r6 eye sends were a quarter to a half).
      for (int p = 0; p < kPools; ++p) g.perSend[k][p] = 0.5f * static_cast<float>(used[p]);
    }
  }
  int kinds[2] = {0, 0};
  int count = layer_count();
  for (int l = 0; l < count; ++l) kinds[l] = layer_kind(l);
  if (!layers_fit(used, kinds, count) && count == 2) {
    // Keep the field; the stadium estimate decays so the layer is tried (and measured) again.
    for (int p = 0; p < kPools; ++p) g.perSend[kinds[0]][p] *= 0.98f;
    kinds[0] = kinds[1];
    count = 1;
    ++g.layersDroppedSinceStats;
  }
  if (!layers_fit(used, kinds, count)) {
    g.layersDroppedSinceStats += count;
    return 0;
  }
  g.frameLayerKind[0] = kinds[0];
  g.frameLayerKind[1] = kinds[1];
  StereoSlot& slot = g.slots[g.slot];
  if (slot.pending) {
    // This slot's blit from two frames ago must be done before Dawn draws into its images again.
    const uint64_t fenceStart = now_ns();
    vkWaitForFences(g.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    g.eyeFenceNs += now_ns() - fenceStart;
    vkResetFences(g.device, 1, &slot.fence);
    slot.pending = false;
  }
  for (int eye = 0; eye < 2; ++eye) {
    EyeImage& img = slot.eye[eye];
    // Dawn's Vulkan backend requires the layout chain. UNDEFINED both ways: every pixel is
    // rewritten each frame, so the previous contents may go.
    wgpu::SharedTextureMemoryVkImageLayoutBeginState layoutBegin{};
    layoutBegin.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    layoutBegin.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    wgpu::SharedTextureMemoryBeginAccessDescriptor desc{};
    desc.nextInChain = &layoutBegin;
    desc.initialized = true;
    img.accessOpen = static_cast<bool>(img.shared.BeginAccess(img.texture, &desc));
    g.eyePassOpen[eye] = false;
  }
  g.stereoLayers = count;
  g.stereoThisFrame = true;
  g.eyeSendStartNs = now_ns();
  return g.stereoLayers;
}

extern "C" int PortXrStereoEyeLayer(int eye, int layer, float view[16], float proj[16], int* flags) {
  if (!g.stereoThisFrame || eye < 0 || eye > 1) {
    return 0;
  }
  EyeImage& img = g.slots[g.slot].eye[eye];
  if (layer == 0) {
    if (!img.accessOpen) {
      return 0;
    }
    aurora::gfx::ExternalPassTarget target;
    target.texture = img.texture;
    target.view = img.renderView;
    target.format = aurora::gfx::color_format();
    target.width = g.eyeWidth;
    target.height = g.eyeHeight;
    // 4x multisampling smooths the edges (Lance: grainy Tabletop); per mode, xr_tune "msaa".
    const bool stadiumLayer = g.frameLayerKind[0] == kLayerStadium ||
                              (g.stereoLayers > 1 && g.frameLayerKind[1] == kLayerStadium);
    target.sampleCount = static_cast<uint32_t>(stadiumLayer ? g.msaaStadium : g.msaaTable);
    // The re-send sets its viewport in the game's native units; let them span the eye image.
    aurora::gfx::set_offscreen_uses_native_logical_size(true);
    if (!aurora::gfx::create_pass_external(target)) {
      aurora::gfx::set_offscreen_uses_native_logical_size(false);
      return 0;
    }
    g.eyePassOpen[eye] = true;
    // create_pass_external drained the GX FIFO: safe to switch Aurora's coverage mode, which makes
    // solid draws write alpha 1 so the field is solid over passthrough (gx.hpp g_xrCoverage).
    aurora_gx_set_xr_coverage(g.mrMode);
  }
  if (!g.eyePassOpen[eye] || layer < 0 || layer > 1) {
    return 0;
  }
  close_pool_measurement();
  const LayerKind kind = static_cast<LayerKind>(g.frameLayerKind[layer]);
  {
    // Last line of defence when a send outgrows its estimate mid-frame: leave this layer out.
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
  const M4 v = m4_mul(model, m4_view_from_pose(g.views[eye].pose));
  std::memcpy(view, v.m, sizeof(v.m));
  // One projection for every layer of an eye, so their depths compare.
  gx_projection(g.views[eye].fov, 0.05f, 1500.0f, proj);
  *flags = kind == kLayerTable ? (kFlagSkipSky | kFlagTable) : kFlagSkipActors;
  return 1;
}

extern "C" void PortXrStereoEyeEnd(int eye) {
  close_pool_measurement();
  if (eye < 0 || eye > 1 || !g.eyePassOpen[eye]) {
    return;
  }
  // Closes the eye's pass; its output is the shared image itself, so nothing is snapshotted.
  aurora::gfx::ResolvedTargets out;
  aurora::gfx::ResolveDesc desc;
  desc.color = false;
  desc.depth = false;
  aurora::gfx::resolve_pass(desc, out);
  aurora_gx_set_xr_coverage(false);  // resolve_pass drained the FIFO
  aurora::gfx::set_offscreen_uses_native_logical_size(false);
  g.eyePassOpen[eye] = false;
}

extern "C" void PortXrStereoFrameEnd(void) {
  if (g.eyeSendStartNs != 0) {
    g.eyeSendNsSinceStats += now_ns() - g.eyeSendStartNs;
    g.eyeSendStartNs = 0;
  }
}

extern "C" int PortXrShouldQuit(void) { return g.quit ? 1 : 0; }

extern "C" void PortXrNoteLoopTimes(int ranGameFrame, unsigned long long xrBegin, unsigned long long auroraBegin,
                                    unsigned long long tasks, unsigned long long auroraEnd, unsigned long long xrEnd) {
  if (!g.active || !ranGameFrame) {
    return;
  }
  g.loopNs[0] += xrBegin;
  g.loopNs[1] += auroraBegin;
  g.loopNs[2] += tasks;
  g.loopNs[3] += auroraEnd;
  g.loopNs[4] += xrEnd;
}

// --- Goal celebrations (Game.cpp) and the Tabletop player marker, drawn by xr_fx.cpp. ---

static void ensure_fx();

extern "C" void PortXrGoal(int team, int character, int superStrike, int hyper, int ownGoal, int humanPad) {
  if (!g.active) {
    return;
  }
  static const char* kNames[] = {"Birdo", "Daisy", "Donkey Kong", "Hammer Bro", "Koopa", "Luigi",
                                 "Mario", "Peach", "Toad", "Waluigi", "Wario", "Yoshi"};
  const char* who = (character >= 0 && character < 12) ? kNames[character] : "?";
  XLOG("celebration: GOAL team %d by %s (class %d)%s%s%s, scorer pad %d", team, who, character,
       superStrike ? " SUPER STRIKE" : "", hyper ? " perfect" : "", ownGoal ? " own goal" : "", humanPad);

  ensure_fx();
  if (g.fxReady) {
    xrfx::goal(team, character, superStrike != 0, hyper != 0, ownGoal != 0);
  }
  // The scorer's hands feel it: a long, strong buzz when the goal is yours (pad 0 = Touch), then a
  // thump with every cannon wave (PortXrEndFrame).
  g.celebBuzz = humanPad == xrnet::local_pad() && !ownGoal;
  g.buzzWave = 0;
  if (g.celebBuzz) {
    g.rumble.amplitude = 1.0f;
    g.rumble.seconds = superStrike ? 0.9f : 0.5f;
    g.rumble.startPending = true;
    g.rumble.stopPending = false;
  }
}

// The effects renderer starts when first needed (a goal, or the Tabletop marker): its eye images at
// 0.6 of the runtime's size (confetti does not need more, and the compositor scales the layer).
static void ensure_fx() {
  if (!g.fxReady && g.device != VK_NULL_HANDLE) {
    xrfx::DeviceInfo dev;
    dev.instance = g.instance;
    dev.session = g.session;
    dev.local = g.local;
    dev.device = g.device;
    dev.queue = g.queue;
    dev.queueFamily = g.queueFamily;
    dev.memProps = &g.memProps;
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
                                      nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (viewCount > 0 &&
        XR_SUCCEEDED(xrEnumerateViewConfigurationViews(g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                       viewCount, &viewCount, views.data()))) {
      dev.width = (static_cast<uint32_t>(views[0].recommendedImageRectWidth * 0.6f) / 8) * 8;
      dev.height = (static_cast<uint32_t>(views[0].recommendedImageRectHeight * 0.6f) / 8) * 8;
    }
    g.fxReady = xrfx::init(dev);
  }
}

extern "C" void PortXrSetGameplayLive(int live) { g.gameplayLive = live != 0; }

// The game's full-screen flashes (the white flash in its replay wipes, the Super Strike photo
// flash) light up your whole view instead of only the game picture (Lance, 2026-10-10). Bright ones
// only: the black fades and the long dark tints stay on the game picture.
extern "C" int PortXrScreenFlash(float r, float gr, float b, float a) {
  if (!g.active || std::max(r, std::max(gr, b)) < 0.5f) {
    return 0;
  }
  ensure_fx();
  if (!g.fxReady) {
    return 0;
  }
  xrfx::screen_flash(r, gr, b, a);
  return 1;
}

// Self-test (xr_fx "wipe"): Game.cpp plays the game's white-flash wipe once.
extern "C" int PortXrTakeTestWipe(void) {
  const bool take = g.testWipe;
  g.testWipe = false;
  return take ? 1 : 0;
}

// Self-test (xr_fx "item"): Game.cpp gives your team a green shell once.
extern "C" int PortXrTakeTestItem(void) {
  const bool take = g.testItem;
  g.testItem = false;
  return take ? 1 : 0;
}

// Self-test (xr_fx "sts"): Game.cpp shows the Super Strike meter once.
extern "C" int PortXrTakeTestSts(void) {
  const bool take = g.testSts;
  g.testSts = false;
  return take ? 1 : 0;
}

extern "C" void PortXrSetStsMeter(int shown, float x, float y, float z, float halfU, float halfV) {
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

// Fielder.cpp UseTeamPowerup: the game took an item press (for the item check).
extern "C" void PortXrNoteItemUse(int current, int next, int blocked) {
  XLOG("item: use, current %d, next %d%s", current, next, blocked ? " (blocked)" : "");
}

extern "C" void PortXrLog(const char* msg) { XLOG("%s", msg); }

// The players on the Tabletop field, drawn bigger (Lance, 2026-10-10: tiny next to the pitch).
extern "C" float PortXrCharacterScale(void) {
  return (g.active && g.tabletop && !g.stereoFailed) ? g.charScale : 1.0f;
}

// The "that's you" marker over the player you control, in both Tabletop modes.
extern "C" void PortXrSetPlayerMarker(int shown, float x, float y, float z, int teamCaptain) {
  if (!g.active) {
    return;
  }
  const bool show = shown != 0 && g.tabletop && !g.stereoFailed;
  if (show) {
    ensure_fx();
  }
  if (g.fxReady) {
    xrfx::set_marker(show, x, y, z, teamCaptain);
  }
}

extern "C" int PortXrStadiumLayerActive(void) { return (PortXrStereoWorld() && !g.mrMode) ? 1 : 0; }

extern "C" int PortXrEyeViewAllowed(int view, int tableLayer) {
  const uint32_t mask = g.eyeViewMask & (tableLayer ? g.eyeViewMaskTable : g.eyeViewMaskStadium);
  return (view >= 0 && view < 32 && ((mask >> view) & 1u) != 0) ? 1 : 0;
}

extern "C" int PortXrHudOnlyNormalView(void) {
  static int s_enabled = -1;
  if (s_enabled < 0) {
    const char* e = std::getenv("STRIKERS_XR_HUD_ONLY");
    s_enabled = (e != nullptr && e[0] == '0') ? 0 : 1;
  }
  return (s_enabled && g.active && g.tabletop && stereo_mode() && !g.stereoFailed && g.gameplayLive &&
          g.hasProjection)
             ? 1
             : 0;
}

extern "C" void PortXrCelebrationPhase(int phase) {
  if (!g.active) {
    return;
  }
  static const char* kPhases[] = {"?", "replay started", "replay played out", "skipped", "play live"};
  XLOG("celebration: %s", (phase >= 1 && phase <= 4) ? kPhases[phase] : "?");
  if (g.fxReady) {
    xrfx::phase(phase);
  }
}

// --- The game's rumble (RumbleActions.cpp, platpad.cpp). Only pad 0 is the Touch controllers. ---

extern "C" void PortXrRumblePreset(int pad, int preset) {
  if (!g.active || pad != xrnet::local_pad()) {
    return;
  }
  // eRumbleActionPreset: small, medium, solid contact, shot contact, Super Strike (hyper), Super
  // Strike. The game's GameCube motor is on or off; the preset is how hard the hit was.
  constexpr float kAmplitude[] = {0.35f, 0.55f, 0.8f, 0.65f, 1.0f, 0.9f};
  if (preset >= 0 && preset < static_cast<int>(sizeof(kAmplitude) / sizeof(kAmplitude[0]))) {
    g.rumble.amplitude = kAmplitude[preset];
  }
}

extern "C" void PortXrRumbleStart(int pad, float seconds) {
  if (!g.active || pad != xrnet::local_pad()) {
    return;
  }
  g.rumble.startPending = true;
  g.rumble.stopPending = false;
  g.rumble.seconds = seconds;
}

extern "C" void PortXrRumbleStop(int pad) {
  if (!g.active || pad != xrnet::local_pad()) {
    return;
  }
  if (!g.rumble.startPending) {
    g.rumble.stopPending = true;
  }
}

extern "C" int PortXrBeginFrame(void) {
  if (!g.active) {
    return 0;
  }
  poll_events();
  if (!g.running) {
    usleep(10000);
    return 0;
  }

  // xrWaitFrame/xrBeginFrame happen in PortXrEndFrame, after the game frame has been recorded and
  // handed to the GPU. The runtime holds xrWaitFrame until the GPU is done with the previous frame;
  // waiting there BEFORE recording (r2-r8) meant the CPU recorded while the GPU sat idle and then sat
  // idle itself while the GPU drew (Tabletop · Stadium: 25 ms + 28 ms a frame). Now the next frame
  // is recorded while the GPU draws this one. The eyes are predicted one display period past the
  // last frame's display time; the compositor reprojects from the poses each image was drawn with.
  g.frameBegun = true;  // this loop iteration ends with an XR frame (PortXrEndFrame)

  xrnet::poll();  // the online room, every headset frame
  sync_input();
  poll_triggers();
  // Before the game frame draws: xr_layers.cpp marks the life-size field parts while it classifies.
  PortXrSetStadiumEconomy(g.lodMinPx, 0.5f * static_cast<float>(g.eyeWidth ? g.eyeWidth : 1256),
                          g.skipTallField ? 1 : 0, (g.tabletop && !g.mrMode && !g.stereoFailed) ? 1 : 0);

  // The 3D modes need both eyes where they will be when this frame is shown, and the floor.
  g.viewsValid = false;
  const XrTime predicted = g.frameState.predictedDisplayTime != 0
                               ? g.frameState.predictedDisplayTime + g.frameState.predictedDisplayPeriod
                               : 0;
  if (!stereo_mode()) {
    g.hasProjection = false;
  } else if (predicted != 0 && create_stereo_resources()) {
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = predicted;
    locate.space = g.local;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    g.views[0] = XrView{XR_TYPE_VIEW};
    g.views[1] = XrView{XR_TYPE_VIEW};
    if (XR_SUCCEEDED(xrLocateViews(g.session, &locate, &viewState, 2, &count, g.views)) && count == 2 &&
        (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0) {
      g.viewsValid = true;
    }
    if (g.stage != XR_NULL_HANDLE) {
      XrSpaceLocation floor{XR_TYPE_SPACE_LOCATION};
      if (XR_SUCCEEDED(xrLocateSpace(g.stage, g.local, predicted, &floor)) &&
          (floor.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
        g.floorY = floor.pose.position.y;
      }
    }
  }

  // The table comes to you: 0.9 m ahead of where you look, at about waist height, its long side
  // across your view (left stick tap, or xr_tune "tablehere").
  if (g.bringTable && g.viewsValid) {
    g.bringTable = false;
    const XrPosef& eye = g.views[0].pose;
    const XrQuaternionf& o = eye.orientation;
    float fx = -(2 * (o.x * o.z + o.w * o.y)), fz = -(1 - 2 * (o.x * o.x + o.y * o.y));
    const float len = std::max(1e-3f, std::sqrt(fx * fx + fz * fz));
    fx /= len;
    fz /= len;
    const XrVector3f head{0.5f * (g.views[0].pose.position.x + g.views[1].pose.position.x),
                          0.5f * (g.views[0].pose.position.y + g.views[1].pose.position.y),
                          0.5f * (g.views[0].pose.position.z + g.views[1].pose.position.z)};
    g.table.position = {head.x + fx * 0.9f, std::max(g.floorY + 0.75f, head.y - 0.55f), head.z + fz * 0.9f};
    g.table.yaw = std::atan2(-fx, -fz);
    XLOG("table brought to (%.2f, %.2f, %.2f) yaw %.2f", g.table.position.x, g.table.position.y,
         g.table.position.z, g.table.yaw);
  }

  // Real time since the last iteration, not the display period: when a 3D mode runs below 60 fps
  // every iteration takes longer than a period, and counting periods made one iteration in six skip
  // its game frame anyway (r2-r10). At 72 Hz with time to spare this still gives 60 game frames.
  const uint64_t tickNow = now_ns();
  if (g.lastTickNs != 0) {
    g.tickAccumNs += static_cast<int64_t>(tickNow - g.lastTickNs);
    // Online and slower than ~64 loops a second with game frames to run: halve the 3D view's rate
    // for the rest of the match (PortXrStereoFrameBegin).
    g.loopIntervalEma = 0.9 * g.loopIntervalEma + 0.1 * static_cast<double>(tickNow - g.lastTickNs);
    if (!PortNetOnline()) {
      g.onlineHalfRate = false;
    } else if (!g.onlineHalfRate && stereo_mode() && g.loopIntervalEma > 15.5e6) {
      g.onlineHalfRate = true;
      XLOG("net: this headset is too slow for 60 online; its 3D view now updates every other frame");
    }
  } else {
    g.tickAccumNs += g.frameState.predictedDisplayPeriod;
  }
  g.lastTickNs = tickNow;
  if (g.tickAccumNs > 4 * kGameTickNs) {
    g.tickAccumNs = kGameTickNs;  // after a stall, catch up by one frame rather than a burst
  }
  const bool due = g.tickAccumNs >= kGameTickNs;

  // Online (include/port/xr_net.h): a game frame runs only once both players' input for it is
  // here; both pads come from the match, home on pad 0 and away on pad 1, the same on both
  // headsets. Waiting keeps the frame due, so it runs as soon as the friend's input arrives.
  // r39b: returning 1 does not use the frame up; main.cpp's PortNetCommitFrame does, once it runs.
  if (xrnet::lockstep()) {
    if (!due) return 0;
    auto to_net = [](const PADStatus& s) {
      xrnet::NetPad p;
      p.buttons = s.button;
      p.stickX = s.stickX;
      p.stickY = s.stickY;
      p.substickX = s.substickX;
      p.substickY = s.substickY;
      p.triggerLeft = s.triggerLeft;
      p.triggerRight = s.triggerRight;
      return p;
    };
    auto from_net = [](const xrnet::NetPad& p) {
      PADStatus s{};
      s.button = p.buttons;
      s.stickX = p.stickX;
      s.stickY = p.stickY;
      s.substickX = p.substickX;
      s.substickY = p.substickY;
      s.triggerLeft = p.triggerLeft;
      s.triggerRight = p.triggerRight;
      s.err = PAD_ERR_NONE;
      return s;
    };
    xrnet::NetPad pad0, pad1;
    if (!xrnet::step(to_net(g.localPad), &pad0, &pad1)) {
      g.tickAccumNs = std::min<int64_t>(g.tickAccumNs, kGameTickNs);
      return 0;
    }
    g.tickAccumNs -= kGameTickNs;
    const PADStatus s0 = from_net(pad0), s1 = from_net(pad1);
    PADSetVirtualStatus(0, &s0);
    PADSetVirtualStatus(1, &s1);
    g.port1Active = true;
    return 1;
  }
  // Offline: this headset is pad 0, every headset frame (keeps it connected without a Bluetooth pad;
  // Aurora merges one in, pad.cpp merge_virtual_status).
  PADSetVirtualStatus(0, &g.localPad);
  if (g.port1Active) {
    PADClearVirtualStatus(1);
    g.port1Active = false;
  }
  if (due) {
    g.tickAccumNs -= kGameTickNs;
    return 1;
  }
  return 0;
}

extern "C" void PortXrEndFrame(int ranGameFrame) {
  if (!g.active || !g.frameBegun) {
    return;
  }
  g.frameBegun = false;

  XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
  g.frameState = XrFrameState{XR_TYPE_FRAME_STATE};
  const uint64_t waitStart = now_ns();
  bool frameOk = xr_ok(xrWaitFrame(g.session, &waitInfo, &g.frameState), "xrWaitFrame");
  g.xrWaitNsSinceStats += now_ns() - waitStart;
  if (frameOk) {
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    frameOk = xr_ok(xrBeginFrame(g.session, &beginInfo), "xrBeginFrame");
  }
  if (frameOk) {
    ++g.displayFramesSinceStats;
  }

  // Even without an XR frame to end, this game frame's eye images go back from Dawn (finish).
  if (ranGameFrame) {
    const uint64_t t0 = now_ns();
    aurora::gfx::synchronize();
    upload_latest_frame();
    if (g.stereoThisFrame) {
      g.stereoThisFrame = false;
      finish_stereo_frame();
    }
    g.stereoNsSinceStats += now_ns() - t0;
    ++g.gameFramesSinceStats;
  }
  if (!frameOk) {
    return;
  }

  XrCompositionLayerPassthroughFB ptLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
  XrCompositionLayerProjection projLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerProjection fxLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  const XrCompositionLayerBaseHeader* layers[4];
  uint32_t layerCount = 0;

  // The celebration effects step every display frame, smooth whatever the game's frame rate, with
  // the eyes located for this display time (not the game frame's prediction).
  const uint64_t fxNow = now_ns();
  xrfx::Scene scene;
  scene.dt = g.lastFxNs != 0 ? static_cast<float>(fxNow - g.lastFxNs) / 1e9f : 0.0f;
  g.lastFxNs = fxNow;
  bool sceneViews = false;
  if (g.fxReady) {
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = g.frameState.predictedDisplayTime;
    locate.space = g.local;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    sceneViews = XR_SUCCEEDED(xrLocateViews(g.session, &locate, &viewState, 2, &count, scene.views)) && count == 2 &&
                 (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
                 (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    if (g.stage != XR_NULL_HANDLE) {
      XrSpaceLocation floor{XR_TYPE_SPACE_LOCATION};
      if (XR_SUCCEEDED(xrLocateSpace(g.stage, g.local, g.frameState.predictedDisplayTime, &floor)) &&
          (floor.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
        g.floorY = floor.pose.position.y;
      }
    }
    scene.floorY = g.floorY;
    scene.mr = g.mrMode;
    scene.tabletop = g.tabletop && !g.stereoFailed;
    scene.tablePos = g.table.position;
    scene.tableYaw = g.table.yaw;
    scene.tableScale = g.table.scale;
    scene.charScale = PortXrCharacterScale();
    scene.stadiumScale = stadium_scale();
    scene.stadiumOrigin = XrVector3f{0.0f, g.floorY, kStandY * stadium_scale()};
  }

  if (g.frameState.shouldRender) {
    if (g.mrMode && g.pt.available && g.pt.layerRunning) {
      ptLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      ptLayer.space = XR_NULL_HANDLE;
      ptLayer.layerHandle = g.pt.layer;
      layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ptLayer);
    }
    // The 3D picture. On display frames without a new game frame the last pair is submitted again
    // with the poses it was drawn for, and the compositor reprojects it.
    if (stereo_mode() && g.hasProjection) {
      // Over passthrough only what was drawn is solid: the eye passes clear to alpha 0 and force
      // alpha 255 on everything they draw (glPlat.cpp).
      projLayer.layerFlags = g.mrMode ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
      projLayer.space = g.local;
      projLayer.viewCount = 2;
      projLayer.views = g.projViews;
      layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projLayer);
    }
    if (g.swapchainHasImage) {
      quad.layerFlags = 0;  // opaque: the GameCube framebuffer's alpha is not coverage
      quad.space = g.local;
      quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      quad.subImage.swapchain = g.swapchain;
      quad.subImage.imageRect.offset = {0, 0};
      quad.subImage.imageRect.extent = {static_cast<int32_t>(g.swapchainWidth),
                                        static_cast<int32_t>(g.swapchainHeight)};
      quad.subImage.imageArrayIndex = 0;
      const float aspect = static_cast<float>(g.swapchainHeight) / static_cast<float>(g.swapchainWidth);
      const float drop = g.fxReady ? xrfx::jumbotron_drop() : 0.0f;
      if (g.tabletop && !g.stereoFailed && drop > 0.0f) {
        // The replay jumbotron comes down from the top, beyond the field and facing you: from the
        // ceiling in the room, from the sky in the stadium (VISION.md).
        XrVector3f head = g.views[0].pose.position;
        if (sceneViews) {
          head = XrVector3f{0.5f * (scene.views[0].pose.position.x + scene.views[1].pose.position.x),
                            0.5f * (scene.views[0].pose.position.y + scene.views[1].pose.position.y),
                            0.5f * (scene.views[0].pose.position.z + scene.views[1].pose.position.z)};
        }
        float dx = g.table.position.x - head.x, dz = g.table.position.z - head.z;
        const float len = std::max(0.1f, std::sqrt(dx * dx + dz * dz));
        dx /= len;
        dz /= len;
        const float width = g.mrMode ? 1.5f : 9.0f;
        const float height = width * aspect;
        XrVector3f anchor;
        float startY, finalY;
        if (g.mrMode) {
          // Past the far side of the field, hanging from the ceiling: its top just under it, so it
          // sits above eye level rather than in front of your face (Lance: "not eye level").
          anchor = XrVector3f{g.table.position.x + dx * 0.6f, 0.0f, g.table.position.z + dz * 0.6f};
          const float ceiling = std::max(g.floorY + 2.3f, head.y + 0.45f);
          startY = ceiling + 0.5f * height;
          finalY = ceiling - 0.1f - 0.5f * height;
        } else {
          anchor = XrVector3f{head.x + dx * 14.0f, 0.0f, head.z + dz * 14.0f};
          startY = g.floorY + 30.0f;
          finalY = g.floorY + 3.0f + 0.5f * height;
        }
        anchor.y = startY + (finalY - startY) * drop;
        const float yaw = std::atan2(head.x - anchor.x, head.z - anchor.z);  // +Z of the quad toward you
        quad.pose.orientation = {0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)};
        quad.pose.position = anchor;
        quad.size.width = width;
        quad.size.height = height;
        if (drop > 0.95f && !g.dropLogged) {
          g.dropLogged = true;
          XLOG("jumbotron down: center (%.2f, %.2f, %.2f), %.1f x %.1f m, head (%.2f, %.2f, %.2f), floor %.2f",
               anchor.x, anchor.y, anchor.z, width, height, head.x, head.y, head.z, g.floorY);
        }
      } else if (g.tabletop && !g.stereoFailed) {
        g.dropLogged = false;
        // The scoreboard: the broadcast view (score, time, replays) hanging over the field.
        const float half = 0.5f * g.table.yaw;
        quad.pose.orientation = {0.0f, std::sin(half), 0.0f, std::cos(half)};
        if (PortXrHudOnlyNormalView() && g.sts.shown) {
          // The Super Strike meter (Lance, alpha 1: the score-bar crop below cut it off, so the
          // timing presses had nothing to aim at). ShootToScoreMeter.cpp draws it in the middle of
          // the HUD-only picture; that square hangs over the captain, turned to you, while it is up.
          const int32_t hw = std::max(1, static_cast<int32_t>(g.swapchainWidth * g.sts.halfU));
          const int32_t hh = std::max(1, static_cast<int32_t>(g.swapchainHeight * g.sts.halfV));
          quad.subImage.imageRect.offset = {static_cast<int32_t>(g.swapchainWidth / 2) - hw,
                                            static_cast<int32_t>(g.swapchainHeight / 2) - hh};
          quad.subImage.imageRect.extent = {2 * hw, 2 * hh};
          // Readable at any table size; 8 game units (about a fifth of the field's width) by default.
          const float width = std::min(0.5f, std::max(0.2f, 8.0f * g.table.scale));
          const float height = width * static_cast<float>(hh) / static_cast<float>(hw);
          // Its bottom edge above the "that's you" arrow over the captain's head.
          XrVector3f anchor =
              table_point(g.sts.game[0], g.sts.game[1], g.sts.game[2] + 4.5f * PortXrCharacterScale());
          anchor.y += 0.5f * height;
          XrVector3f head = g.views[0].pose.position;
          if (sceneViews) {
            head = XrVector3f{0.5f * (scene.views[0].pose.position.x + scene.views[1].pose.position.x),
                              0.5f * (scene.views[0].pose.position.y + scene.views[1].pose.position.y),
                              0.5f * (scene.views[0].pose.position.z + scene.views[1].pose.position.z)};
          }
          const float yaw = std::atan2(head.x - anchor.x, head.z - anchor.z);  // +Z of the quad toward you
          quad.pose.orientation = {0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)};
          quad.pose.position = anchor;
          quad.size.width = width;
          quad.size.height = height;
          if (!g.sts.logged) {
            g.sts.logged = true;
            XLOG("super strike meter: panel (%.2f, %.2f, %.2f), %.2f x %.2f m, picture %dx%d at (%d, %d)",
                 anchor.x, anchor.y, anchor.z, width, height, 2 * hw, 2 * hh,
                 quad.subImage.imageRect.offset.x, quad.subImage.imageRect.offset.y);
          }
        } else if (PortXrHudOnlyNormalView()) {
          // Live play draws the HUD alone (glPlat.cpp): show just its score bar (names, score,
          // clock, items), a ribbon board over the field, instead of a mostly black screen.
          const int32_t x0 = static_cast<int32_t>(g.swapchainWidth * 0.13f);
          const int32_t y0 = static_cast<int32_t>(g.swapchainHeight * 0.03f);
          const int32_t w = static_cast<int32_t>(g.swapchainWidth * 0.74f);
          const int32_t h = std::max(1, static_cast<int32_t>(g.swapchainHeight * 0.11f));
          quad.subImage.imageRect.offset = {x0, y0};
          quad.subImage.imageRect.extent = {w, h};
          const float width = 0.9f;
          const float height = width * static_cast<float>(h) / static_cast<float>(w);
          quad.pose.position = {g.table.position.x, g.table.position.y + 0.4f + 0.5f * height, g.table.position.z};
          quad.size.width = width;
          quad.size.height = height;
        } else {
          const float width = 0.75f;
          quad.pose.position = {g.table.position.x, g.table.position.y + 0.42f + 0.5f * width * aspect,
                                g.table.position.z};
          quad.size.width = width;
          quad.size.height = width * aspect;
        }
      } else {
        const ScreenPreset& preset = kScreenPresets[g.screenPreset];
        quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        quad.pose.position = {0.0f, -0.1f, -preset.distance};
        quad.size.width = preset.width;
        quad.size.height = preset.width * aspect;
      }
      layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
      scene.screenValid = true;
      scene.screenPose = quad.pose;
      scene.screenWidth = quad.size.width;
      scene.screenHeight = quad.size.height;
    }
    // The celebration over everything else.
    const uint64_t fxStart = now_ns();
    if (g.fxReady && sceneViews && xrfx::update(scene) && xrfx::render(scene, &fxLayer)) {
      layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&fxLayer);
    }
    g.fxNs += now_ns() - fxStart;
    if (g.fxReady && g.celebBuzz && xrfx::cannon_waves_fired() > g.buzzWave) {
      g.buzzWave = xrfx::cannon_waves_fired();
      if (g.buzzWave > 1) {  // the first wave rides on the goal buzz
        g.rumble.amplitude = 1.0f;
        g.rumble.seconds = 0.22f;
        g.rumble.startPending = true;
        g.rumble.stopPending = false;
      }
    }
  }
  XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
  endInfo.displayTime = g.frameState.predictedDisplayTime;
  endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  endInfo.layerCount = layerCount;
  endInfo.layers = layers;
  const uint64_t endStart = now_ns();
  xr_ok(xrEndFrame(g.session, &endInfo), "xrEndFrame");
  g.xrEndNs += now_ns() - endStart;

  log_stats();
}

extern "C" void PortXrShutdown(void) {
  aurora_xr_set_frame_callback(nullptr, nullptr);
  aurora_xr_set_surface_suppressed(false);
  xrfx::shutdown();
  if (g.device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(g.device);
    for (uint32_t s = 0; s < kStereoSlots; ++s) {
      StereoSlot& slot = g.slots[s];
      for (int eye = 0; eye < 2; ++eye) {
        EyeImage& img = slot.eye[eye];
        img.renderView = nullptr;
        img.texture = nullptr;
        img.shared = nullptr;
        if (img.dawnDone != VK_NULL_HANDLE) vkDestroySemaphore(g.device, img.dawnDone, nullptr);
        if (img.image != VK_NULL_HANDLE) vkDestroyImage(g.device, img.image, nullptr);
        if (img.memory != VK_NULL_HANDLE) vkFreeMemory(g.device, img.memory, nullptr);
      }
      if (slot.fence != VK_NULL_HANDLE) vkDestroyFence(g.device, slot.fence, nullptr);
    }
  }
  for (int eye = 0; eye < 2; ++eye) {
    if (g.eyeSwapchain[eye] != XR_NULL_HANDLE) xrDestroySwapchain(g.eyeSwapchain[eye]);
  }
  if (g.stage != XR_NULL_HANDLE) xrDestroySpace(g.stage);
  if (g.actions.gripSpaceL != XR_NULL_HANDLE) xrDestroySpace(g.actions.gripSpaceL);
  if (g.actions.gripSpaceR != XR_NULL_HANDLE) xrDestroySpace(g.actions.gripSpaceR);
  if (g.device != VK_NULL_HANDLE) {
    for (UploadSlot& u : g.uploads) {
      if (u.mapped != nullptr) vkUnmapMemory(g.device, u.memory);
      if (u.buffer != VK_NULL_HANDLE) vkDestroyBuffer(g.device, u.buffer, nullptr);
      if (u.memory != VK_NULL_HANDLE) vkFreeMemory(g.device, u.memory, nullptr);
      if (u.fence != VK_NULL_HANDLE) vkDestroyFence(g.device, u.fence, nullptr);
    }
  }
  destroy_swapchain();
  if (g.device != VK_NULL_HANDLE) {
    destroy_staging();
    if (g.fence != VK_NULL_HANDLE) vkDestroyFence(g.device, g.fence, nullptr);
    if (g.pool != VK_NULL_HANDLE) vkDestroyCommandPool(g.device, g.pool, nullptr);
  }
  if (g.local != XR_NULL_HANDLE) xrDestroySpace(g.local);
  if (g.session != XR_NULL_HANDLE) xrDestroySession(g.session);
  if (g.device != VK_NULL_HANDLE) vkDestroyDevice(g.device, nullptr);
  if (g.vkInstance != VK_NULL_HANDLE) vkDestroyInstance(g.vkInstance, nullptr);
  if (g.instance != XR_NULL_HANDLE) xrDestroyInstance(g.instance);
  g = State{};
}

#else // !__ANDROID__ || !STRIKERS_XR

extern "C" int PortXrWanted(void) { return 0; }
extern "C" int PortXrInit(void) { return 0; }
extern "C" int PortXrActive(void) { return 0; }
extern "C" int PortXrStereoWorld(void) { return 0; }
extern "C" int PortXrStereoFullStands(void) { return 0; }
extern "C" int PortXrStereoFrameBegin(void) { return 0; }
extern "C" int PortXrStereoEyeLayer(int, int, float*, float*, int*) { return 0; }
extern "C" void PortXrStereoEyeEnd(int) {}
extern "C" void PortXrStereoFrameEnd(void) {}
extern "C" int PortXrShouldQuit(void) { return 0; }
extern "C" void PortXrNoteLoopTimes(int, unsigned long long, unsigned long long, unsigned long long,
                                    unsigned long long, unsigned long long) {}
extern "C" void PortXrGoal(int, int, int, int, int, int) {}
extern "C" void PortXrCelebrationPhase(int) {}
extern "C" void PortXrSetGameplayLive(int) {}
extern "C" void PortXrSetPlayerMarker(int, float, float, float, int) {}
extern "C" int PortXrScreenFlash(float, float, float, float) { return 0; }
extern "C" int PortXrTakeTestWipe(void) { return 0; }
extern "C" int PortXrTakeTestItem(void) { return 0; }
extern "C" void PortXrSetStsMeter(int, float, float, float, float, float) {}
extern "C" int PortXrTakeTestSts(void) { return 0; }
extern "C" void PortXrNoteItemUse(int, int, int) {}
extern "C" void PortXrLog(const char*) {}
extern "C" float PortXrCharacterScale(void) { return 1.0f; }
extern "C" int PortXrHudOnlyNormalView(void) { return 0; }
extern "C" int PortXrEyeViewAllowed(int, int) { return 1; }
extern "C" int PortXrStadiumLayerActive(void) { return 0; }
extern "C" void PortXrRumblePreset(int, int) {}
extern "C" void PortXrRumbleStart(int, float) {}
extern "C" void PortXrRumbleStop(int) {}
extern "C" int PortXrBeginFrame(void) { return 0; }
extern "C" void PortXrEndFrame(int) {}
extern "C" void PortXrShutdown(void) {}

#endif
