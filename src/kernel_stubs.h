// Kernel exports implemented by this project (see kernel_stubs.cpp).

#pragma once

#include <functional>

namespace nhl3on3 {

// Called when the game asks for the system achievements UI.
void SetAchievementsOpener(std::function<void()> opener);

}  // namespace nhl3on3
