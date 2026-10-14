// StrikersVision: the contract between the three visionOS pieces.
//
//   visionos/app  (Swift, SwiftUI)  — launcher, settings, ImmersiveSpace + CompositorLayer, SharePlay.
//   visionos/xr   (Obj-C++ / Metal) — implements include/port/xr_quest.h (PortXr*) and the
//                                      PortXrStereo* hooks with Compositor Services + ARKit.
//   visionos/net  (Swift / Obj-C)    — the network transports behind xr_net.cpp (WebSocket relay and
//                                      SharePlay), replacing android/.../NetWs.java.
//   smstrikers-port                  — the game, built as a static library; its main() is renamed
//                                      strikers_main() on visionOS (see SV_GAME_MAIN below).
//
// Rules:
//  - Plain C, so Swift imports it through the bridging header and every side links against it.
//  - Additive only: new functions/fields go at the end of their section; never change an existing
//    signature (the other pieces are written against it in parallel).
//  - Settings reach the game as STRIKERS_* environment variables set by the app before
//    sv_game_start() (the port already reads its options that way: PortEnvU32/PortEnvFloat).
//  - Threads: sv_game_start() runs the whole game (aurora + game loop + XR frame loop) on ONE thread
//    it creates, like the Quest build's SDL main thread. Compositor Services frame pacing happens on
//    that thread inside PortXrBeginFrame/PortXrEndFrame. Everything else is called from the main
//    thread unless stated.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================
 * App -> game (implemented in visionos/xr/sv_game.mm)
 * ============================================================================================ */

/* Starts the game thread once per process. `home` = the folder that plays STRIKERS_HOME's role
 * (strikers.ini, strikers-log.txt, saves, and the first place the disc image is looked for):
 * <Documents>/Strikers, visible in the Files app. `discFolder` = an extra folder the user picked
 * with the system picker (security-scoped access already started by the app), or NULL.
 * Returns 1 if started (or already running). */
int  sv_game_start(const char* home, const char* discFolder);

/* 1 while the game thread is alive. */
int  sv_game_running(void);

/* The CompositorLayer's renderer (a cp_layer_renderer_t, passed as void* so this header does not
 * need CompositorServices). Called from the CompositorLayer closure every time the immersive space
 * opens; the game thread picks it up at its next PortXrBeginFrame. Until one is set, and after it
 * is invalidated, the game thread waits (paused, no game frames). The app keeps the renderer alive;
 * the XR side only retains it while using it. */
void sv_set_layer_renderer(void* cpLayerRenderer);

/* The immersive space closed or the renderer reached cp_layer_renderer_state_invalidated. */
void sv_layer_renderer_invalidated(void);

/* The app moved to the background / came back (scenePhase). Paused = no game frames, audio paused. */
void sv_set_paused(int paused);

/* Asks the game to quit cleanly (the loop ends, saves are flushed). */
void sv_game_request_quit(void);

/* Live settings the app may change while playing (Settings sheet inside the immersive space).
 * key/value pairs, same names as the STRIKERS_* variables; unknown keys are ignored. */
void sv_setting_changed(const char* key, const char* value);

/* ============================================================================================
 * Game/XR -> app (implemented in visionos/app, Swift @_cdecl; registered before sv_game_start)
 * ============================================================================================ */

typedef struct SVAppCallbacks {
    /* The XR layer wants the system hands/arms hidden (1) or shown (0) — e.g. hidden in the stadium.
     * Called on the game thread; the app hops to the main thread. */
    void (*set_upper_limbs_visible)(int visible);
    /* The game thread ended (normal quit or fatal error). `reason` is a short English string. */
    void (*game_ended)(const char* reason);
    /* One log line for the in-app log viewer (already written to strikers-log.txt). Any thread. */
    void (*log_line)(const char* line);
    /* The current view mode changed (SV_VIEW_*), for the app's HUD/settings sheet. Any thread. */
    void (*view_mode_changed)(int viewMode);
    /* The game booted far enough to draw into the immersive space (the headset loop is running):
     * the app opens the space only then, so it never shows an empty view during the boot. Any
     * thread, once. */
    void (*game_ready)(void);
} SVAppCallbacks;

void sv_register_app_callbacks(const SVAppCallbacks* callbacks);

/* View modes, as the Quest build names them (xr_quest.cpp). */
enum {
    SV_VIEW_JUMBOTRON_ROOM    = 0,  /* big screen in your room (passthrough) */
    SV_VIEW_JUMBOTRON_STADIUM = 1,  /* big screen, standing in the stadium */
    SV_VIEW_TABLETOP_ROOM     = 2,  /* miniature pitch on your table */
    SV_VIEW_TABLETOP_STADIUM  = 3,  /* the table inside the stadium */
};

/* Asks the XR layer to switch view mode (from the app's UI as well as the stick double-clicks). */
void sv_set_view_mode(int viewMode);

/* ============================================================================================
 * Network transports (implemented in visionos/net; used by xr_net.cpp on __APPLE__)
 * Same shape as NetWs.java: handle-based, non-blocking, message-framed.
 * ============================================================================================ */

/* Opens a connection to the room `room` on the relay `host` (wss://host/room/<room>), or, when a
 * SharePlay session is active and `host` is SV_NET_SHAREPLAY, to the SharePlay group.
 * Returns a handle >= 0, or -1. Never blocks: messages queue until connected. */
int  sv_net_open(const char* host, int port, const char* room, int flags);
/* Queues one message. 1 = queued, 0 = the connection is gone (caller reopens). */
int  sv_net_send(int handle, const void* data, int size);
/* Copies the next received message into buf. Returns its size, 0 = nothing pending,
 * -1 = connection gone, -2 = buf too small (message kept). */
int  sv_net_recv(int handle, void* buf, int capacity);
void sv_net_close(int handle);
/* 1 while the handle's connection is up (relay socket open / SharePlay session joined). */
int  sv_net_connected(int handle);

#define SV_NET_SHAREPLAY "shareplay"

/* App -> game: the room picked in the launcher ("" = offline), as nativeSetRoom on Android.
 * Implemented in xr_net.cpp. `viaSharePlay` = 1 routes it through SV_NET_SHAREPLAY. */
void sv_net_set_room(const char* room, int viaSharePlay);

/* ============================================================================================
 * SharePlay (spatial Personas) <-> XR layer
 * ============================================================================================ */

/* App -> XR: number of participants in the SharePlay session (0 = none), and whether the system
 * placed us in a shared (spatial) immersive space, so the room origin is common to everyone. */
void sv_shareplay_state(int participants, int spatial);

/* XR -> app: the table's pose changed locally (column-major 4x4, metres, immersive-space origin),
 * scale = the table's size factor. The app broadcasts it to the group. Game thread. */
typedef void (*SVTablePoseCallback)(const float m[16], float scale);
void sv_register_table_pose_callback(SVTablePoseCallback callback);

/* App -> XR: a participant moved the shared table; apply it (same spatial frame for everyone). */
void sv_apply_remote_table_pose(const float m[16], float scale);

/* ============================================================================================
 * The game's entry point on visionOS (smstrikers-port/src/Game/main.cpp is compiled with
 * -Dmain=strikers_main through SV_GAME_MAIN, and called by sv_game_start on its thread).
 * ============================================================================================ */
int strikers_main(int argc, char** argv);

#ifdef __cplusplus
}
#endif
