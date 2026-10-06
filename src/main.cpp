// nhl3on3 - ReXGlue Recompiled Project

#include "generated/default/nhl3on3_init.h"

#include "nhl3on3_app.h"

REX_DEFINE_APP(nhl3on3, Nhl3on3App::Create)

#ifdef _WIN32
// Ask hybrid-graphics laptops to run the game on the dedicated GPU. The drivers only read
// these from the executable, so the copies inside rexruntime.dll have no effect.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
