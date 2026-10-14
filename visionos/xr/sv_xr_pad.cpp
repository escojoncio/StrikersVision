// StrikersVision: the one place the headset layer touches Aurora's PAD (dolphin/pad.h).
//
// Its own translation unit, plain C++, on purpose: dolphin/types.h typedefs BOOL as int, which
// clashes with Objective-C's BOOL, so sv_xr.mm (Objective-C++) cannot include dolphin/pad.h. The
// PADStatus handed to PADSetVirtualStatus must be the one Aurora's PAD was built with, which is
// exactly what this file sees (same TARGET_PC / AURORA defines as the game side, project.yml).

#include "sv_xr.h"

#include <dolphin/pad.h>

#include <cstring>

static_assert(SV_PAD_LEFT == PAD_BUTTON_LEFT && SV_PAD_RIGHT == PAD_BUTTON_RIGHT && SV_PAD_DOWN == PAD_BUTTON_DOWN &&
                  SV_PAD_UP == PAD_BUTTON_UP && SV_PAD_Z == PAD_TRIGGER_Z && SV_PAD_R == PAD_TRIGGER_R &&
                  SV_PAD_L == PAD_TRIGGER_L && SV_PAD_A == PAD_BUTTON_A && SV_PAD_B == PAD_BUTTON_B &&
                  SV_PAD_X == PAD_BUTTON_X && SV_PAD_Y == PAD_BUTTON_Y && SV_PAD_START == PAD_BUTTON_START,
              "sv_xr.h SV_PAD_* must match dolphin/pad.h");

extern "C" void sv_pad_set_virtual(int port, const SVPad* pad) {
  if (pad == nullptr || port < 0 || port >= PAD_CHANMAX) return;
  PADStatus s;
  std::memset(&s, 0, sizeof(s));
  s.button = pad->button;
  s.stickX = pad->stickX;
  s.stickY = pad->stickY;
  s.substickX = pad->substickX;
  s.substickY = pad->substickY;
  s.triggerLeft = pad->triggerLeft;
  s.triggerRight = pad->triggerRight;
  s.err = PAD_ERR_NONE;
  PADSetVirtualStatus(static_cast<u32>(port), &s);
}

extern "C" void sv_pad_clear_virtual(int port) {
  if (port < 0 || port >= PAD_CHANMAX) return;
  PADClearVirtualStatus(static_cast<u32>(port));
}
