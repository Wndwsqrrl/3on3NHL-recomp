// Kernel exports the game imports but the ReXGlue runtime does not provide.

#include <functional>
#include <utility>

#include <rex/hook.h>
#include <rex/types.h>

#include "kernel_stubs.h"

namespace nhl3on3 {

namespace {
std::function<void()> g_open_achievements;
}  // namespace

void SetAchievementsOpener(std::function<void()> opener) {
  g_open_achievements = std::move(opener);
}

// Xbox Live Vision camera (the SDK leaves xboxkrnl_usbcam.cpp out of its build).
// Report no camera attached; the game only probes for it.
rex::u32 XUsbcamGetState_entry() {
  return 0;  // 0 = not connected
}

// The main menu's Achievements item asks the system to show its achievements UI, which the
// SDK only stubs. Open the runtime's achievements overlay instead.
rex::u32 XamShowAchievementsUI_entry(rex::u32 user_index, rex::u32 flags) {
  (void)user_index;
  (void)flags;
  if (g_open_achievements) {
    g_open_achievements();
  }
  return 0;  // X_ERROR_SUCCESS
}

}  // namespace nhl3on3

REX_EXPORT(__imp__XUsbcamGetState, nhl3on3::XUsbcamGetState_entry)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetConfig, 0)
REX_EXPORT(__imp__XamShowAchievementsUI, nhl3on3::XamShowAchievementsUI_entry)
