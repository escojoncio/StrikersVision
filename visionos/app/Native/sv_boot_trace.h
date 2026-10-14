// StrikersVision: a launch breadcrumb file, Documents/arranque.txt, written from the very first code
// that runs (a load-time constructor) through the first launcher frame, so a launch that dies before
// the game log exists still says how far it got. stderr goes to the same file until the game opens
// its own log (sv_game_start), so a crash handler's backtrace in that window lands there too.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void sv_boot_trace(const char* step);

#ifdef __cplusplus
}
#endif
