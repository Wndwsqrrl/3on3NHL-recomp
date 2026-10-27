// Team play lobby: the side each player picked on the Select Sides screen,
// shared between consoles; see nhl3on3_team_lobby.cpp.
#pragma once

#include <cstdint>
#include <string_view>

#include <rex/ppc/context.h>

namespace nhl3on3::team_lobby {

// Side (0 or 1) of the player at session index `index`: their pick, or
// session order parity if they made none.
uint32_t SideOf(uint32_t index);

// Lineup slot for the player at `index`: side + 2 * rank among the players
// before them on that side.
uint32_t LineupOf(uint32_t index);

// Position lock index (0: none) of the player at `index` from their user
// record, shared between consoles; -1 if not known yet.
int LockOf(uint32_t index);

// Whether the player at `index` is their side's goalie: the first there to
// ask for it (play_goalie), else a side's fourth player.
bool IsGoalie(uint32_t index);

// True once every player's position lock is known; until then, resends the
// local one now and then (needs the game thread's context).
bool PositionsReady(const PPCContext& ctx, uint8_t* base);

// Forgets the position locks once a match's roster is set up.
void ClearPositions();

// Forgets everything (players, sides, locks) when a new search starts.
void Reset();

// An OSDK session control message ("name|CMD|data") sent or received.
void OnControlMessage(std::string_view text);

}  // namespace nhl3on3::team_lobby
