// StrikersVision: TEMPORARY. The include/port/xr_quest.h side of the headset layer (PortXr*), as the
// inert "not active" answers a flat build gives, until visionos/xr/sv_xr.mm provides them. Linked
// only into the StrikersVision target; each definition goes as soon as the real one exists.
#include <stdio.h>

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
