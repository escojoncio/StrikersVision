// StrikersVision launch probe (SVProbeGame): references the game's entry point so the linker keeps
// the force-loaded game libraries and their load-time constructors, exactly as in the real app,
// without starting anything. Inert stand-ins for the app-side symbols the game expects come from
// visionos/bootstrap/sv_bootstrap_stubs.c.
#include "../include/sv_bridge.h"

__attribute__((used)) static void* const sv_probe_keep[] = { (void*)&strikers_main };
