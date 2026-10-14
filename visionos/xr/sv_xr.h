// StrikersVision headset layer: what the pieces of visionos/xr share among themselves.
//
//   sv_game.mm      the App -> game half of sv_bridge.h, the game thread, settings, the log.
//   sv_xr.mm        include/port/xr_quest.h on visionOS: frame loop, input -> PAD, the 3D eye passes
//                   (IOSurfaces shared with Dawn), the flat game picture, the Metal composite.
//   sv_fx.mm        the goal celebrations (port of src/platform/xr_fx.cpp) drawn with Metal.
//   SVXRHost.swift  Compositor Services frames, ARKit (head and Sense controller poses),
//                   GameController buttons and haptics; reached through the sv_xrhost_* C functions
//                   below (@_cdecl), all called on the game thread.
//   sv_xr_pad.cpp   the PADStatus hand-over to Aurora's PAD (plain C++: dolphin/types.h's BOOL
//                   cannot meet Objective-C's).
//   SVLayerConfiguration.swift  the CompositorLayer setup (formats, layout, foveation).
//   sv_shaders.h    every shader of the composite and the effects, as one MSL source compiled at run
//                   time (newLibraryWithSource), so a shader error lands in the game log.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Log: one line to stderr (the game log file once the port has redirected it) and to the
 * app's log viewer (SVAppCallbacks.log_line). Any thread. */
void sv_log(const char* level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#define SVLOG(...) sv_log("", __VA_ARGS__)
#define SVWARN(...) sv_log(" warn", __VA_ARGS__)
#define SVERR(...) sv_log(" error", __VA_ARGS__)
/* The same for Swift (which cannot call a variadic C function): one finished line. */
void sv_log_line(const char* line);

/* ---- sv_game.mm -> sv_xr.mm */
/* The renderer the app handed over (cp_layer_renderer_t), retained for the caller (+1: CFRelease
 * it), or NULL; and a generation number that changes every time a new one is set. */
void* sv_game_copy_renderer(uint32_t* generation);
int sv_game_paused(void);
int sv_game_quit_requested(void);
/* Pending live settings (sv_setting_changed), applied by sv_xr.mm on the game thread. Returns 1 and
 * fills key/value while there are any. */
int sv_game_take_setting(char* key, int keySize, char* value, int valueSize);
/* Pending view mode (sv_set_view_mode), -1 when none. */
int sv_game_take_view_mode(void);
/* SharePlay: participants, spatial; the registered table pose callback; a pending remote pose. */
void sv_game_shareplay(int* participants, int* spatial);
void sv_game_send_table_pose(const float m[16], float scale);
int sv_game_take_remote_table_pose(float m[16], float* scale);
/* The app's callbacks (any may be NULL). */
void sv_game_set_upper_limbs_visible(int visible);
void sv_game_view_mode_changed(int viewMode);

/* ---- sv_xr_pad.cpp: one GameCube pad state for Aurora's virtual pad on `port` (PADSetVirtualStatus),
 * or the port given back to real controllers (PADClearVirtualStatus). Game thread. */
typedef struct SVPad {
  uint16_t button; /* SV_PAD_* (dolphin/pad.h's PAD_BUTTON_* / PAD_TRIGGER_* values) */
  int8_t stickX, stickY, substickX, substickY;
  uint8_t triggerLeft, triggerRight;
} SVPad;
enum {
  SV_PAD_LEFT = 0x0001,
  SV_PAD_RIGHT = 0x0002,
  SV_PAD_DOWN = 0x0004,
  SV_PAD_UP = 0x0008,
  SV_PAD_Z = 0x0010,
  SV_PAD_R = 0x0020,
  SV_PAD_L = 0x0040,
  SV_PAD_A = 0x0100,
  SV_PAD_B = 0x0200,
  SV_PAD_X = 0x0400,
  SV_PAD_Y = 0x0800,
  SV_PAD_START = 0x1000,
};
void sv_pad_set_virtual(int port, const SVPad* pad);
void sv_pad_clear_virtual(int port);

/* ---- sv_game.mm: the game's log reaches the app's viewer. stderr stays the log file itself (the port
 * dup2s STRIKERS_LOG onto fd 2, unbuffered), so every byte, a crash handler's last words included, is
 * on disk before anything else happens; a thread follows the file and hands each new line to
 * SVAppCallbacks.log_line. Started by sv_game_start when STRIKERS_LOG is an absolute path. */

/* ---- Helpers on environment variables (STRIKERS_*). */
float sv_env_float(const char* name, float fallback, float lo, float hi);
int sv_env_int(const char* name, int fallback, int lo, int hi);

/* ---- SVXRHost.swift (@_cdecl). Game thread only. ---- */
/* Starts the ARKit session (world tracking, plus Sense controller tracking when supported), and
 * hands Swift the log function (Swift cannot call the variadic sv_log). Idempotent. */
void sv_xrhost_start(void (*logLine)(const char* line));
/* State of a cp_layer_renderer_t: 0 paused, 1 running, 2 invalidated. */
int32_t sv_xrhost_renderer_state(void* renderer);
/* One Compositor Services frame, up to startSubmission: query the next frame, update phase, wait for
 * the optimal input time, take the drawables (visionOS 26 can hand out more than one while the
 * render quality changes; every one must be presented). Returns the drawable count: > 0 = the
 * caller MUST present each (sv_xrhost_frame_present), commit, then call sv_xrhost_frame_finish;
 * 0 = no frame this time (nothing to end). metalDevice = the layer renderer's MTLDevice (unretained). */
int32_t sv_xrhost_frame_begin(void* renderer, void** metalDevice);
/* Drawable d: presentation time in ARKit's clock (CACurrentMediaTime domain), depth range
 * (x = far, y = near, metres, as the drawable reports it) and its view count. */
int32_t sv_xrhost_drawable_info(int32_t d, double* presentationTime, float* depthRange, int32_t* viewCount);
/* View v of drawable d: device-from-view transform and the drawable's projection (both column-major,
 * the projection reverse-Z for Metal clip space), its colour and depth textures (id<MTLTexture>,
 * unretained, valid until present), the texture slice and the viewport (x, y, width, height in
 * pixels). 0 if out of range. */
int32_t sv_xrhost_frame_view(int32_t d, int32_t v, float* deviceFromView, float* projection, void** colorTexture,
                             void** depthTexture, int32_t* slice, float* viewport);
/* The head (ARKit device anchor) predicted for `time`: origin-from-device (column-major) and an id the
 * drawable can be told later (sv_xrhost_frame_present); -1 when ARKit has no pose. */
int32_t sv_xrhost_head(double time, float* originFromDevice);
/* Drawable d: sets its device anchor (the pose its pixels were drawn for; -1 = none) and encodes its
 * present into the command buffer (id<MTLCommandBuffer>). The caller then commits. */
void sv_xrhost_frame_present(int32_t d, void* commandBuffer, int32_t anchorId);
/* frame.endSubmission(), after the commit. */
void sv_xrhost_frame_finish(void);
/* Controllers, predicted to `time`. Per hand (0 left, 1 right): connected, buttons (SV_XRB_*),
 * analog[4] = trigger, grip, stick x, stick y; poseValid and originFromGrip (column-major). The
 * gamepad (any other controller with an extended gamepad profile): buttons (SV_GPB_*) and
 * analog[6] = left x, left y, right x, right y, left trigger, right trigger. */
void sv_xrhost_input(double time, int32_t* handConnected, uint32_t* handButtons, float* handAnalog,
                     int32_t* poseValid, float* originFromGrip, int32_t* gamepadConnected, uint32_t* gamepadButtons,
                     float* gamepadAnalog);
/* Rumble: amplitude 0..1 for `seconds` on every connected controller (0 seconds = stop). */
void sv_xrhost_rumble(float amplitude, float seconds);

/* Sense controller buttons, per hand (the hand is the device). */
enum {
  SV_XRB_LOWER = 1u << 0,       /* right: cross (Touch A), left: square (Touch X) */
  SV_XRB_UPPER = 1u << 1,       /* right: circle (Touch B), left: triangle (Touch Y) */
  SV_XRB_MENU = 1u << 2,        /* left Create / right Options */
  SV_XRB_STICK_CLICK = 1u << 3,
  SV_XRB_PS = 1u << 4,          /* the PlayStation button, when the system lets it through */
};
/* Gamepad buttons (Aurora's standard layout: south = A, east = B, west = X, north = Y). */
enum {
  SV_GPB_SOUTH = 1u << 0,
  SV_GPB_EAST = 1u << 1,
  SV_GPB_WEST = 1u << 2,
  SV_GPB_NORTH = 1u << 3,
  SV_GPB_START = 1u << 4,
  SV_GPB_RSHOULDER = 1u << 5,
  SV_GPB_LSHOULDER = 1u << 6,
  SV_GPB_LSTICK = 1u << 7,
  SV_GPB_RSTICK = 1u << 8,
  SV_GPB_UP = 1u << 9,
  SV_GPB_DOWN = 1u << 10,
  SV_GPB_LEFT = 1u << 11,
  SV_GPB_RIGHT = 1u << 12,
  SV_GPB_HOME = 1u << 13,
};

#ifdef __cplusplus
}  // extern "C"

// ---- C++ only: the effects renderer (sv_fx.mm), driven by sv_xr.mm. ----
namespace svfx {

struct V3 {
  float x, y, z;
};
struct Quat {
  float x, y, z, w;  // unit quaternion
};

// One eye of the composite: where it is (LOCAL space, metres, Y up), its axes, and what it sees.
struct EyeView {
  V3 position;
  V3 right, up, back;  // the eye's +X, +Y, +Z in LOCAL (it looks down -Z)
  float viewProj[16];  // LOCAL -> Metal clip space, column-major, reverse-Z (near 1, far 0)
};

// Where things are this display frame, in LOCAL space (meters, Y up). Same fields as xr_fx.h's Scene.
struct Scene {
  float dt = 0.0f;
  EyeView views[2] = {};
  float floorY = -1.6f;
  bool mr = true;
  bool tabletop = false;
  V3 tablePos{0.0f, 0.0f, 0.0f};
  float tableYaw = 0.0f;
  float tableScale = 0.03f;
  float charScale = 1.0f;
  float stadiumScale = 1.0f;
  V3 stadiumOrigin{0.0f, 0.0f, 0.0f};
  bool screenValid = false;
  V3 screenPos{0.0f, 0.0f, 0.0f};
  Quat screenOrientation{0.0f, 0.0f, 0.0f, 1.0f};
  float screenWidth = 0.0f, screenHeight = 0.0f;
  bool depthFromEyes = false;  // the drawable's depth already holds the real 3D scene
};

// Metal objects are passed as unretained pointers (id) so this header stays plain C++.
// The effects draw into a pass whose colour attachment has `colorPixelFormat` and whose depth
// attachment (the effects' own, cleared to 0) is Depth32Float.
bool init(void* mtlDevice, uint32_t colorPixelFormat);
void shutdown();
void goal(int team, int character, bool superStrike, bool hyper, bool ownGoal);
void phase(int phase);
// Steps the effects (once per display frame). True while anything is alive or still to come.
bool update(const Scene& scene);
// Fills this display frame's instance buffers (once per command buffer, before encode). False when
// there is nothing to draw (encode must then not be called).
bool prepare(const Scene& scene);
// Encodes one eye's effects into an open render encoder (id<MTLRenderCommandEncoder>).
void encode(void* renderEncoder, const Scene& scene, int view);
// The command buffer (id<MTLCommandBuffer>) that reads this display frame's buffers; they are
// reused once it completes. Call after the last encode, before commit.
void frame_committed(void* commandBuffer);
float jumbotron_drop();
int cannon_waves_fired();
void set_marker(bool shown, float x, float y, float z, int teamCaptain);
void screen_flash(float r, float g, float b, float a);
bool take_stats(uint32_t* peakParts, uint32_t* peakIndices);

}  // namespace svfx
#endif
