#pragma once

#include <filesystem>
#include <string>

namespace nhl3on3 {

// If roster/roster.json is found beside the executable (or in a parent) and
// roster/headshots/ has images for it, edits the extracted game so those
// players are on the arcade switch list. No-op once that exact roster and
// the current front-end patch are already applied. Must run after
// ApplyUiPatches and before the runtime opens the game files.
void ApplyRoster(const std::filesystem::path& game_dir);

// Short id shared in the team-play lobby. "stock" when no custom roster is
// applied; otherwise a content hash of version.txt + roster.json + headshots.
// Peers that disagree will desync if they start a match.
std::string RosterOnlineId();

}  // namespace nhl3on3
