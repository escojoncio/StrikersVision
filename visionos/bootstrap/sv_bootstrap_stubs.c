// StrikersVision bootstrap: every symbol the game library expects from the app side, as inert stubs,
// so libstrikers.a can be linked into a visionOS app before visionos/xr and visionos/net exist.
//
//   - include/port/xr_quest.h (PortXr*) and the PortXrStereo* hooks glPlat.cpp declares: the same
//     "not active" answers src/platform/xr_quest.cpp gives on a flat build.
//   - visionos/include/sv_bridge.h: sv_game_start/sv_game_running (run strikers_main on its own
//     thread) and the sv_net_* transports (always "gone").
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/sv_bridge.h"

// ---- include/port/xr_quest.h + glPlat.cpp's stereo hooks ----------------------------------------
int PortXrWanted(void) { return 0; }
int PortXrInit(void) { return 0; }
int PortXrActive(void) { return 0; }
int PortXrStereoWorld(void) { return 0; }
int PortXrStereoFullStands(void) { return 0; }
int PortXrStereoFrameBegin(void) { return 0; }
int PortXrStereoEyeLayer(int eye, int layer, float view[16], float proj[16], int* flags)
{
    (void)eye; (void)layer; (void)view; (void)proj; (void)flags;
    return 0;
}
void PortXrStereoEyeEnd(int eye) { (void)eye; }
void PortXrStereoFrameEnd(void) {}
int PortXrShouldQuit(void) { return 0; }
void PortXrNoteLoopTimes(int ranGameFrame, unsigned long long xrBegin, unsigned long long auroraBegin,
                         unsigned long long tasks, unsigned long long auroraEnd, unsigned long long xrEnd)
{
    (void)ranGameFrame; (void)xrBegin; (void)auroraBegin; (void)tasks; (void)auroraEnd; (void)xrEnd;
}
void PortXrGoal(int team, int character, int superStrike, int hyper, int ownGoal, int humanPad)
{
    (void)team; (void)character; (void)superStrike; (void)hyper; (void)ownGoal; (void)humanPad;
}
void PortXrCelebrationPhase(int phase) { (void)phase; }
void PortXrSetGameplayLive(int live) { (void)live; }
void PortXrSetPlayerMarker(int shown, float x, float y, float z, int teamCaptain)
{
    (void)shown; (void)x; (void)y; (void)z; (void)teamCaptain;
}
int PortXrScreenFlash(float r, float g, float b, float a)
{
    (void)r; (void)g; (void)b; (void)a;
    return 0;
}
int PortXrTakeTestWipe(void) { return 0; }
int PortXrTakeTestItem(void) { return 0; }
void PortXrSetStsMeter(int shown, float x, float y, float z, float halfU, float halfV)
{
    (void)shown; (void)x; (void)y; (void)z; (void)halfU; (void)halfV;
}
int PortXrTakeTestSts(void) { return 0; }
void PortXrNoteItemUse(int current, int next, int blocked) { (void)current; (void)next; (void)blocked; }
void PortXrLog(const char* msg) { fprintf(stderr, "[xr] %s\n", msg != NULL ? msg : ""); }
float PortXrCharacterScale(void) { return 1.0f; }
int PortXrHudOnlyNormalView(void) { return 0; }
int PortXrEyeViewAllowed(int view, int tableLayer) { (void)view; (void)tableLayer; return 1; }
int PortXrStadiumLayerActive(void) { return 0; }
void PortXrRumblePreset(int pad, int preset) { (void)pad; (void)preset; }
void PortXrRumbleStart(int pad, float seconds) { (void)pad; (void)seconds; }
void PortXrRumbleStop(int pad) { (void)pad; }
int PortXrBeginFrame(void) { return 0; }
void PortXrEndFrame(int ranGameFrame) { (void)ranGameFrame; }
void PortXrShutdown(void) {}

// ---- visionos/include/sv_bridge.h: the game thread ----------------------------------------------
static atomic_int s_started;
static atomic_int s_running;
static char s_home[1024];

static void* game_thread(void* arg)
{
    (void)arg;
    pthread_setname_np("strikers-game");
    char* argv[] = { (char*)"strikers", NULL };
    atomic_store(&s_running, 1);
    const int rc = strikers_main(1, argv);
    fprintf(stderr, "[bootstrap] strikers_main returned %d\n", rc);
    atomic_store(&s_running, 0);
    return NULL;
}

int sv_game_start(const char* home, const char* discFolder)
{
    (void)discFolder;
    if (atomic_exchange(&s_started, 1))
        return 1;
    snprintf(s_home, sizeof s_home, "%s", home != NULL ? home : "");
    setenv("STRIKERS_HOME", s_home, 1);
    setenv("STRIKERS_LOG", "1", 0);
    setenv("STRIKERS_NO_MESSAGEBOX", "1", 1);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // The game was written for a console main thread; a secondary thread's 512 KiB is not enough.
    pthread_attr_setstacksize(&attr, 16u * 1024u * 1024u);
    pthread_t tid;
    const int rc = pthread_create(&tid, &attr, game_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
    {
        atomic_store(&s_started, 0);
        return 0;
    }
    pthread_detach(tid);
    return 1;
}

int sv_game_running(void) { return atomic_load(&s_running); }

// ---- visionos/include/sv_bridge.h: transports (xr_net.cpp's, on visionOS) ------------------------
int sv_net_open(const char* host, int port, const char* room, int flags)
{
    (void)host; (void)port; (void)room; (void)flags;
    return -1;
}
int sv_net_send(int handle, const void* data, int size) { (void)handle; (void)data; (void)size; return 0; }
int sv_net_recv(int handle, void* buf, int capacity) { (void)handle; (void)buf; (void)capacity; return -1; }
void sv_net_close(int handle) { (void)handle; }
int sv_net_connected(int handle) { (void)handle; return 0; }
