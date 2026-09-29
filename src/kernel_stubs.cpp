// Kernel exports the game imports but the ReXGlue runtime does not provide.

#include <rex/hook.h>
#include <rex/types.h>

namespace nhl3on3 {

// Xbox Live Vision camera (the SDK leaves xboxkrnl_usbcam.cpp out of its build).
// Report no camera attached; the game only probes for it.
rex::u32 XUsbcamGetState_entry() {
  return 0;  // 0 = not connected
}

}  // namespace nhl3on3

REX_EXPORT(__imp__XUsbcamGetState, nhl3on3::XUsbcamGetState_entry)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetConfig, 0)
