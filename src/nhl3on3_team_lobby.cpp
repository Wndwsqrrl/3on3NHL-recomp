// Team play lobby: side choice on the Select Sides screen.
//
// The patched screen (tools/ui_mod.py) talks to this file through the
// GetControllerLogin script native, which normally takes a controller index
// below 4. Requests at 98 and up are answered here instead:
//   GetControllerLogin(99, me * 16 + count)   tick: local index, player count
//   GetControllerLogin(100 + side, me)        the local player picks a side
//   GetControllerLogin(102)                   -> the local side if it changed
//                                                since the last ask, else "-1"
//   GetControllerLogin(110 + side)            -> names on that side, ", "
//   GetControllerLogin(120 + i)               -> side of player i, "0" / "1"
//   GetControllerLogin(1000 + i, name)        name of player i
// Starting Lineups (position locks):
//   GetControllerLogin(130, lock)             tick: the local player's lock
//   GetControllerLogin(140 + j)               -> lock of teammate j (another
//                                                console's player on the local
//                                                side), "-1" for none, "-2" if
//                                                unchanged since the last ask
//   GetControllerLogin(150 + j)               -> teammate j's label, " (name)"
//   GetControllerLogin(200 + k)               -> script name k (kScriptNames)
//   GetControllerLogin(1100 + i, local)       whether player i is on this
//                                                console
// Ready status (both screens):
//   GetControllerLogin(1200 + i, ready)       Select Sides: player i's ready flag
//   GetControllerLogin(164 + side)            -> "1" if everyone on the side is
//                                                ready, else "0"
//   GetControllerLogin(160 + side)            -> "setReady" if that changed since
//                                                it was last shown, else "-"
//   GetControllerLogin(162 + side)            -> Starting Lineups: "animateReady"
//                                                / "animateNotReady" on a change,
//                                                else "-"
//   GetControllerLogin(171)                   -> title line: who is ready and
//                                                who is still waited for
//   GetControllerLogin(170)                   -> "setTitle" if that changed, else
//                                                "-"
// "-" names nothing, so the script's call does nothing. Starting Lineups has
// no room to report flags, so ready also comes off the wire: a client's
// "RDY|true" to the host and the host's "UPD|i:ready=true" to everyone.
// Indices are session order, the same on every console (m_aPlayers, and the
// order the roster step visits members in).
//
// Picks travel as session control messages, which the stock screens ignore:
//   TPS|i=side       a console's own pick, on change and every second
//   TPT|i=side;...   the host's whole table, every second
//   TPP|i=lock       a console's own position lock, on change and every
//                    second on Starting Lineups, and when the screen stores
//                    its user record at match start. Clients only reach the
//                    host, so the host's TPP carries every lock it knows
//                    (i=lock;...) and is resent as soon as a client's arrives.
//   TRF|id           this console's roster pack id (RosterOnlineId), on change
//                    and every second. Mismatched packs desync in lockstep, so
//                    the lobby refuses to start until everyone matches.
// The roster step waits for every player's lock (see PositionsReady), since
// they must agree on all of them. Two teammates with the same lock: the first
// in session order keeps it, the other gets none.
// Both are read off the wire by the datagram logger (OnControlMessage), so
// every console sees its own and everyone else's.

#include "nhl3on3_team_lobby.h"
#include "nhl3on3_roster.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <utility>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(int32_t, team_play);
REXCVAR_DECLARE(int32_t, goalie_test);

REXCVAR_DEFINE_BOOL(play_goalie, false, "NHL3on3/Online",
                    "Ask to play goalie in online team play (the first player on a side to "
                    "ask gets it; otherwise a side's fourth player is its goalie)");

bool Nhl3on3RankedSearch();

// Script native helpers: argument (index) -> value, value -> number,
// argument (index) -> C string ("" if not a string), C string -> returned value.
extern "C" REX_FUNC(sub_82C03D10);
extern "C" REX_FUNC(sub_82C00868);
extern "C" REX_FUNC(sub_829BD130);
extern "C" REX_FUNC(sub_829CC260);

namespace nhl3on3::team_lobby {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kOnlineGlobal = 0x83211FCC;  // +84: the game setup adaptor
constexpr uint32_t kSendToPeersSlot = 28;       // adaptor virtual (adaptor, text)
constexpr uint32_t kMaxPlayers = 12;
constexpr uint32_t kMaxPerSide = 4;  // three skaters and a goalie
// Lock value a player sends to ask for goalie; the position lock screen only
// has none (0), C, LW and RW.
constexpr int kGoalieLock = 15;
constexpr auto kResend = std::chrono::seconds(1);
constexpr auto kLockResend = std::chrono::milliseconds(250);

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

struct Player {
  std::string name;
  int side = -1;  // -1: no pick
  int lock = -1;  // position lock index from the user record; -1: unknown
  bool here = false;  // on this console (the local player or a local guest)
  bool ready = false;  // ready on the current screen
  std::string roster_id;  // TRF from that console; empty until heard
};

// Screens whose timer ticks the lobby; ready flags are per screen.
enum class Screen { kNone, kSelectSides, kStartingLineups };

// Remote teammate icons on Starting Lineups (position lock slots 4..6).
constexpr uint32_t kTeammateSlots = 3;
constexpr auto kTeammateRefresh = std::chrono::seconds(2);

// Names the Starting Lineups timer needs that are not in its constant pool.
constexpr const char* kScriptNames[] = {
    "lineupPanel",  "m_mySide",           "mcPositionLock", "getPosition",
    "setPosition",  "setConnectedStatus", "setText",        "removePadding",
    "gControllerIDCurrentScreen", "mPositions", "NAME",
    // Must match SCRIPT_NAMES in tools/ui_mod.py.
    "mainFrame", "SelectSides", "StartingLineups",
};

struct Lobby {
  std::mutex mutex;
  std::vector<Player> players;
  uint32_t count = 0;
  int local = -1;
  int shown_side = -1;  // last local side answered to request 102
  bool dirty = false;   // local pick not yet sent
  Clock::time_point last_send{};
  Clock::time_point last_lock_send{};
  bool locks_dirty = false;  // host: a lock received from a client, not yet relayed
  // Teammate icons as last shown: player index and lock (-1 / -1: hidden).
  std::array<std::pair<int, int>, kTeammateSlots> shown_teammates{};
  Clock::time_point teammates_refreshed{};
  std::string logged;
  Screen screen = Screen::kNone;
  // Per side "everyone ready", as last answered to 164 and as last shown.
  std::array<int, 2> side_ready{-1, -1};
  std::array<int, 2> shown_side_ready{-1, -1};
  std::string status;        // as last answered to 171
  std::string shown_status;  // as last shown
  bool roster_mismatch = false;
  bool roster_mismatch_logged = false;
  std::string sent_roster_id;
};
Lobby g_lobby;

uint32_t EffectiveSide(const Lobby& lobby, uint32_t index) {
  if (index < lobby.players.size() && lobby.players[index].side >= 0) {
    return uint32_t(lobby.players[index].side);
  }
  return index % 2;
}

void LogTable(Lobby& lobby) {
  std::string text;
  for (uint32_t i = 0; i < lobby.count && i < lobby.players.size(); ++i) {
    const Player& p = lobby.players[i];
    text += fmt::format("{}{}={}{}", text.empty() ? "" : ", ", p.name.empty() ? "?" : p.name,
                        EffectiveSide(lobby, i), p.side < 0 ? " (default)" : "");
  }
  if (text != lobby.logged) {
    lobby.logged = text;
    REXLOG_INFO("[team] sides: {}", text);
  }
}

Player* At(Lobby& lobby, uint32_t index) {
  if (index >= kMaxPlayers) {
    return nullptr;
  }
  if (lobby.players.size() <= index) {
    lobby.players.resize(index + 1);
  }
  return &lobby.players[index];
}

void SetSide(Lobby& lobby, uint32_t index, int side) {
  if (side >= -1 && side <= 1) {
    if (Player* p = At(lobby, index)) {
      p->side = side;
    }
  }
}

void SetLock(Lobby& lobby, uint32_t index, int lock) {
  if (lock >= 0 && lock < 16) {
    if (Player* p = At(lobby, index)) {
      if (p->lock != lock) {
        REXLOG_INFO("[team] player {} position lock {}", index, lock);
      }
      p->lock = lock;
    }
  }
}

int ParseInt(std::string_view text) {
  int value = 0;
  bool negative = false, any = false;
  for (const char c : text) {
    if (c == '-' && !any) {
      negative = true;
    } else if (c >= '0' && c <= '9') {
      value = value * 10 + (c - '0');
      any = true;
    } else {
      break;
    }
  }
  return any ? (negative ? -value : value) : -1;
}

// "i=value" entries separated by ';'; the local player's own entry is skipped.
void ApplyEntries(Lobby& lobby, std::string_view data, void (*set)(Lobby&, uint32_t, int)) {
  while (!data.empty()) {
    const size_t end = data.find(';');
    const std::string_view entry = data.substr(0, end);
    if (const size_t eq = entry.find('='); eq != std::string_view::npos) {
      const int index = ParseInt(entry.substr(0, eq));
      if (index >= 0 && index != lobby.local) {
        set(lobby, uint32_t(index), ParseInt(entry.substr(eq + 1)));
      }
    }
    data = end == std::string_view::npos ? std::string_view() : data.substr(end + 1);
  }
}

// Ready flags belong to one screen; a new screen starts with nobody ready.
void EnterScreen(Lobby& lobby, Screen screen) {
  if (lobby.screen == screen) {
    return;
  }
  lobby.screen = screen;
  for (Player& p : lobby.players) {
    p.ready = false;
  }
  lobby.side_ready = lobby.shown_side_ready = {-1, -1};
  lobby.status.clear();
  lobby.shown_status.clear();
}

void SetReady(Lobby& lobby, uint32_t index, bool ready) {
  if (Player* p = At(lobby, index); p && p->ready != ready) {
    p->ready = ready;
    REXLOG_INFO("[team] player {} {}", index, ready ? "ready" : "not ready");
  }
}

// True when every known peer roster id matches ours. Unknown (empty) ids are
// ignored until that console's TRF arrives.
bool RosterMismatch(Lobby& lobby) {
  const std::string local = nhl3on3::RosterOnlineId();
  bool mismatch = false;
  for (uint32_t i = 0; i < lobby.count && i < lobby.players.size(); ++i) {
    const std::string& id = lobby.players[i].roster_id;
    if (!id.empty() && id != local) {
      mismatch = true;
      break;
    }
  }
  if (mismatch && !lobby.roster_mismatch_logged) {
    lobby.roster_mismatch_logged = true;
    REXLOG_ERROR("[team] roster pack mismatch (local {}); everyone needs the same "
                 "roster zip or none",
                 local);
  }
  if (!mismatch) {
    lobby.roster_mismatch_logged = false;
  }
  lobby.roster_mismatch = mismatch;
  return mismatch;
}

// Whether `side` has players and all of them are ready.
bool SideReady(Lobby& lobby, uint32_t side) {
  if (RosterMismatch(lobby)) {
    return false;
  }
  bool any = false;
  for (uint32_t i = 0; i < lobby.count; ++i) {
    if (EffectiveSide(lobby, i) == side) {
      if (i >= lobby.players.size() || !lobby.players[i].ready) {
        return false;
      }
      any = true;
    }
  }
  return any;
}

// "READY: a, b   WAITING: c" for team play (more than two players). Once shown
// it stays up to date even if players leave.
std::string StatusText(Lobby& lobby) {
  if (RosterMismatch(lobby)) {
    return "ROSTER MISMATCH: same pack required";
  }
  if (lobby.count <= 2 && lobby.shown_status.empty()) {
    return {};
  }
  std::string ready, waiting;
  for (uint32_t i = 0; i < lobby.count; ++i) {
    const bool is_ready = i < lobby.players.size() && lobby.players[i].ready;
    const std::string name =
        i < lobby.players.size() && !lobby.players[i].name.empty() ? lobby.players[i].name : "?";
    std::string& list = is_ready ? ready : waiting;
    list += (list.empty() ? "" : ", ") + name;
  }
  if (waiting.empty()) {
    return "ALL READY";
  }
  return ready.empty() ? "WAITING: " + waiting : "READY: " + ready + "   WAITING: " + waiting;
}

// The TPP to send: the host's has every known lock, a client's only its own.
std::string LockMessage(Lobby& lobby) {
  std::string message;
  for (uint32_t i = 0; i < lobby.players.size(); ++i) {
    if (lobby.players[i].lock >= 0 && (lobby.local == 0 || int(i) == lobby.local)) {
      message += fmt::format("{}{}={}", message.empty() ? "TPP|" : ";", i, lobby.players[i].lock);
    }
  }
  if (lobby.local == 0) {
    lobby.locks_dirty = false;
  }
  return message;
}

// Copies `text` into one of two guest buffers (0: script replies, 1: sends).
uint32_t GuestText(const std::string& text, uint32_t slot) {
  constexpr uint32_t kSize = 512;
  static uint32_t buffers[2] = {};
  uint32_t& buffer = buffers[slot];
  auto* memory = REX_KERNEL_MEMORY();
  if (!buffer) {
    buffer = memory->SystemHeapAlloc(kSize);
    if (!buffer) {
      return 0;
    }
  }
  const size_t n = std::min<size_t>(text.size(), kSize - 1);
  auto* p = reinterpret_cast<char*>(memory->TranslateVirtual(buffer));
  std::memcpy(p, text.data(), n);
  p[n] = 0;
  return buffer;
}

void SendToPeers(const PPCContext& caller, uint8_t* base, const std::string& text) {
  const uint32_t online = Load32(base, kOnlineGlobal);
  const uint32_t adaptor = online ? Load32(base, online + 84) : 0;
  if (!adaptor) {
    return;
  }
  PPCFunc* send =
      rex::runtime::ResolveIndirectFunction(Load32(base, Load32(base, adaptor) + kSendToPeersSlot));
  const uint32_t guest = GuestText(text, 1);
  if (!send || !guest) {
    return;
  }
  PPCContext ctx = caller;
  ctx.r3.u64 = adaptor;
  ctx.r4.u64 = guest;
  send(ctx, base);
}

// Sends the local pick (or, on the host, the whole table) when it changed or
// a second has passed. Also keeps the roster pack id on the wire. Called on
// the game thread; the lock is not held.
void MaybeSend(const PPCContext& ctx, uint8_t* base) {
  std::string message, roster_message;
  {
    std::lock_guard lock(g_lobby.mutex);
    Lobby& lobby = g_lobby;
    const auto now = Clock::now();
    const std::string roster_id = nhl3on3::RosterOnlineId();
    const bool roster_due =
        lobby.local >= 0 && (lobby.sent_roster_id != roster_id || now - lobby.last_send >= kResend);
    if (lobby.local < 0 || (!lobby.dirty && now - lobby.last_send < kResend && !roster_due)) {
      return;
    }
    lobby.dirty = false;
    lobby.last_send = now;
    if (lobby.local == 0) {
      for (uint32_t i = 0; i < lobby.count && i < lobby.players.size(); ++i) {
        if (lobby.players[i].side >= 0) {
          message += fmt::format("{}{}={}", message.empty() ? "TPT|" : ";", i,
                                 lobby.players[i].side);
        }
      }
    } else if (uint32_t(lobby.local) < lobby.players.size() &&
               lobby.players[lobby.local].side >= 0) {
      message = fmt::format("TPS|{}={}", lobby.local, lobby.players[lobby.local].side);
    }
    if (roster_due) {
      lobby.sent_roster_id = roster_id;
      if (uint32_t(lobby.local) < lobby.players.size()) {
        lobby.players[lobby.local].roster_id = roster_id;
      }
      roster_message = "TRF|" + roster_id;
    }
  }
  if (!message.empty()) {
    SendToPeers(ctx, base, message);
  }
  if (!roster_message.empty()) {
    SendToPeers(ctx, base, roster_message);
  }
}

int32_t ArgNumber(const PPCContext& caller, uint8_t* base, uint32_t index) {
  PPCContext ctx = caller;
  ctx.r3.u64 = index;
  sub_82C03D10(ctx, base);
  sub_82C00868(ctx, base);
  return int32_t(ctx.r3.u32);
}

std::string ArgString(const PPCContext& caller, uint8_t* base, uint32_t index) {
  PPCContext ctx = caller;
  ctx.r3.u64 = index;
  sub_829BD130(ctx, base);
  std::string text;
  for (uint32_t p = ctx.r3.u32, i = 0; p && i < 256 && base[p + i]; ++i) {
    text += char(base[p + i]);
  }
  return text;
}

// Index of teammate j: the j-th player on the local side who is not on this
// console; -1 if there is none.
int Teammate(const Lobby& lobby, uint32_t j) {
  if (lobby.local < 0) {
    return -1;
  }
  const uint32_t side = EffectiveSide(lobby, lobby.local);
  for (uint32_t i = 0; i < lobby.count && i < lobby.players.size(); ++i) {
    const Player& p = lobby.players[i];
    if (int(i) != lobby.local && !p.here && EffectiveSide(lobby, i) == side && j-- == 0) {
      return int(i);
    }
  }
  return -1;
}

// The lock player `index` plays with: none if an earlier teammate has it.
int EffectiveLock(const Lobby& lobby, uint32_t index) {
  if (index >= lobby.players.size()) {
    return -1;
  }
  const int lock = lobby.players[index].lock;
  if (lock == kGoalieLock) {
    return 0;
  }
  for (uint32_t i = 0; lock > 0 && i < index; ++i) {
    if (lobby.players[i].lock == lock && EffectiveSide(lobby, i) == EffectiveSide(lobby, index)) {
      return 0;
    }
  }
  return lock;
}

// The goalie of `side`, in session order so every console agrees: the first
// player there who asked for it, else the fourth player (a side's skaters
// are full at three); goalie_test makes it the side's first player. -1: none.
int GoalieOf(const Lobby& lobby, uint32_t side) {
  int first = -1, last = -1;
  uint32_t count = 0;
  for (uint32_t i = 0; i < lobby.count; ++i) {
    if (EffectiveSide(lobby, i) != side) {
      continue;
    }
    if (i < lobby.players.size() && lobby.players[i].lock == kGoalieLock) {
      return int(i);
    }
    first = first < 0 ? int(i) : first;
    last = int(i);
    ++count;
  }
  if (REXCVAR_GET(goalie_test) == int32_t(side)) {
    return first;
  }
  return count >= kMaxPerSide ? last : -1;
}

// The lock the local player reports: the screen's, or goalie if asked for.
int LocalLock(int screen_lock) {
  return REXCVAR_GET(play_goalie) ? kGoalieLock : screen_lock;
}

// Answers one request; returns the text the script gets back.
std::string Handle(const PPCContext& ctx, uint8_t* base, int32_t code) {
  if (code == 130) {
    const int32_t lock = LocalLock(ArgNumber(ctx, base, 1));
    std::string message;
    {
      std::lock_guard lock_guard(g_lobby.mutex);
      Lobby& lobby = g_lobby;
      EnterScreen(lobby, Screen::kStartingLineups);
      if (lobby.local < 0 || uint32_t(lobby.local) >= kMaxPlayers || lock < 0 || lock >= 16) {
        return {};
      }
      Player* me = At(lobby, lobby.local);
      const auto now = Clock::now();
      if (me->lock != lock || lobby.locks_dirty || now - lobby.last_lock_send >= kResend) {
        SetLock(lobby, lobby.local, lock);
        lobby.last_lock_send = now;
        message = LockMessage(lobby);
      }
    }
    if (!message.empty()) {
      SendToPeers(ctx, base, message);
    }
    return {};
  }
  if (code >= 1100 && code < 1100 + int32_t(kMaxPlayers)) {
    const bool here = ArgNumber(ctx, base, 1) != 0;
    std::lock_guard lock(g_lobby.mutex);
    if (Player* p = At(g_lobby, code - 1100)) {
      p->here = here;
    }
    return {};
  }
  if (code >= 1200 && code < 1200 + int32_t(kMaxPlayers)) {
    const bool ready = ArgNumber(ctx, base, 1) == 1;
    std::lock_guard lock(g_lobby.mutex);
    SetReady(g_lobby, code - 1200, ready);
    return {};
  }
  if (code == 99) {
    const int32_t arg = ArgNumber(ctx, base, 1);
    {
      std::lock_guard lock(g_lobby.mutex);
      EnterScreen(g_lobby, Screen::kSelectSides);
      g_lobby.local = arg >= 0 ? arg / 16 : -1;
      g_lobby.count = arg >= 0 ? std::min<uint32_t>(arg % 16, kMaxPlayers) : 0;
      LogTable(g_lobby);
    }
    MaybeSend(ctx, base);
    return {};
  }
  if (code == 100 || code == 101) {
    const int32_t me = ArgNumber(ctx, base, 1);
    const int side = code - 100;
    {
      std::lock_guard lock(g_lobby.mutex);
      Lobby& lobby = g_lobby;
      if (REXCVAR_GET(team_play) < 3 || Nhl3on3RankedSearch() || me < 0 ||
          uint32_t(me) >= kMaxPlayers) {
        return {};
      }
      lobby.local = me;
      uint32_t others = 0;
      for (uint32_t i = 0; i < lobby.count; ++i) {
        others += i != uint32_t(me) && EffectiveSide(lobby, i) == uint32_t(side);
      }
      if (EffectiveSide(lobby, me) == uint32_t(side) || others >= kMaxPerSide) {
        return {};
      }
      SetSide(lobby, me, side);
      lobby.dirty = true;
      REXLOG_INFO("[team] picked side {}", side);
      LogTable(lobby);
    }
    MaybeSend(ctx, base);
    return {};
  }
  std::lock_guard lock(g_lobby.mutex);
  Lobby& lobby = g_lobby;
  if (code == 102) {
    if (lobby.local < 0) {
      return "-1";
    }
    const int side = int(EffectiveSide(lobby, lobby.local));
    if (side == lobby.shown_side) {
      return "-1";
    }
    lobby.shown_side = side;
    return side ? "1" : "0";
  }
  if (code == 110 || code == 111) {
    std::string names;
    for (uint32_t i = 0; i < lobby.count && i < lobby.players.size(); ++i) {
      if (EffectiveSide(lobby, i) == uint32_t(code - 110) && !lobby.players[i].name.empty()) {
        names += (names.empty() ? "" : ", ") + lobby.players[i].name;
      }
    }
    return names;
  }
  if (code >= 120 && code < 120 + int32_t(kMaxPlayers)) {
    return EffectiveSide(lobby, code - 120) ? "1" : "0";
  }
  if (code >= 140 && code < 140 + int32_t(kTeammateSlots)) {
    const uint32_t j = code - 140;
    const auto now = Clock::now();
    if (j == 0 && now - lobby.teammates_refreshed >= kTeammateRefresh) {
      lobby.teammates_refreshed = now;
      lobby.shown_teammates.fill({-2, -2});
    }
    const int index = Teammate(lobby, j);
    const std::pair<int, int> shown{index, index < 0 ? -1 : std::max(EffectiveLock(lobby, index), 0)};
    if (shown == lobby.shown_teammates[j]) {
      return "-2";
    }
    lobby.shown_teammates[j] = shown;
    return std::to_string(index < 0 ? -1 : shown.second);
  }
  if (code >= 150 && code < 150 + int32_t(kTeammateSlots)) {
    const int index = Teammate(lobby, code - 150);
    return index < 0 ? std::string() : " (" + lobby.players[index].name + ")";
  }
  if (code == 164 || code == 165) {
    const uint32_t side = code - 164;
    lobby.side_ready[side] = SideReady(lobby, side);
    return lobby.side_ready[side] ? "1" : "0";
  }
  if (code == 160 || code == 161) {
    const uint32_t side = code - 160;
    if (lobby.side_ready[side] < 0 || lobby.side_ready[side] == lobby.shown_side_ready[side]) {
      return "-";
    }
    lobby.shown_side_ready[side] = lobby.side_ready[side];
    return "setReady";
  }
  if (code == 162 || code == 163) {
    const uint32_t side = code - 162;
    const int ready = SideReady(lobby, side);
    if (ready == std::max(lobby.shown_side_ready[side], 0)) {
      return "-";
    }
    lobby.shown_side_ready[side] = ready;
    return ready ? "animateReady" : "animateNotReady";
  }
  if (code == 171) {
    lobby.status = StatusText(lobby);
    return lobby.status;
  }
  if (code == 170) {
    if (lobby.status.empty() || lobby.status == lobby.shown_status) {
      return "-";
    }
    lobby.shown_status = lobby.status;
    REXLOG_INFO("[team] status: {}", lobby.status);
    return "setTitle";
  }
  if (code >= 200 && code < 200 + int32_t(std::size(kScriptNames))) {
    return kScriptNames[code - 200];
  }
  if (code >= 1000 && code < 1000 + int32_t(kMaxPlayers)) {
    const uint32_t index = code - 1000;
    std::string name = ArgString(ctx, base, 1);
    if (name.empty() || name == "undefined") {
      return {};
    }
    if (lobby.players.size() <= index) {
      lobby.players.resize(index + 1);
    }
    Player& p = lobby.players[index];
    if (p.name != name) {
      REXLOG_INFO("[team] player {} is {}", index, name);
      p = {name, -1};
    }
    lobby.count = std::max(lobby.count, index + 1);
  }
  return {};
}

}  // namespace

uint32_t SideOf(uint32_t index) {
  std::lock_guard lock(g_lobby.mutex);
  return EffectiveSide(g_lobby, index);
}

uint32_t LineupOf(uint32_t index) {
  std::lock_guard lock(g_lobby.mutex);
  const uint32_t side = EffectiveSide(g_lobby, index);
  uint32_t rank = 0;
  for (uint32_t i = 0; i < index; ++i) {
    rank += EffectiveSide(g_lobby, i) == side;
  }
  return side + 2 * rank;
}

int LockOf(uint32_t index) {
  std::lock_guard lock(g_lobby.mutex);
  return EffectiveLock(g_lobby, index);
}

bool IsGoalie(uint32_t index) {
  std::lock_guard lock(g_lobby.mutex);
  return GoalieOf(g_lobby, EffectiveSide(g_lobby, index)) == int(index);
}

bool PositionsReady(const PPCContext& ctx, uint8_t* base) {
  std::string message;
  bool ready = true;
  {
    std::lock_guard lock(g_lobby.mutex);
    Lobby& lobby = g_lobby;
    if (RosterMismatch(lobby)) {
      return false;
    }
    for (uint32_t i = 0; i < lobby.count; ++i) {
      ready = ready && i < lobby.players.size() && lobby.players[i].lock >= 0;
    }
    const auto now = Clock::now();
    if (lobby.locks_dirty ||
        (!ready && lobby.local >= 0 && uint32_t(lobby.local) < lobby.players.size() &&
         lobby.players[lobby.local].lock >= 0 && now - lobby.last_lock_send > kLockResend)) {
      lobby.last_lock_send = now;
      message = LockMessage(lobby);
    }
  }
  if (!message.empty()) {
    SendToPeers(ctx, base, message);
  }
  return ready;
}

void ClearPositions() {
  std::lock_guard lock(g_lobby.mutex);
  for (Player& p : g_lobby.players) {
    p.lock = -1;
  }
  g_lobby.locks_dirty = false;
}

void Reset() {
  std::lock_guard lock(g_lobby.mutex);
  Lobby& lobby = g_lobby;
  lobby.players.clear();
  lobby.count = 0;
  lobby.local = -1;
  lobby.shown_side = -1;
  lobby.dirty = false;
  lobby.locks_dirty = false;
  lobby.teammates_refreshed = {};
  lobby.logged.clear();
  lobby.screen = Screen::kNone;
  lobby.side_ready = lobby.shown_side_ready = {-1, -1};
  lobby.status.clear();
  lobby.shown_status.clear();
  lobby.roster_mismatch = false;
  lobby.roster_mismatch_logged = false;
  lobby.sent_roster_id.clear();
}

void OnControlMessage(std::string_view text) {
  const size_t a = text.find('|');
  const size_t b = a == std::string_view::npos ? a : text.find('|', a + 1);
  if (b == std::string_view::npos) {
    return;
  }
  const std::string_view sender = text.substr(0, a), command = text.substr(a + 1, b - a - 1);
  const std::string_view data = text.substr(b + 1);
  if (command != "TPS" && command != "TPT" && command != "TPP" && command != "UPD" &&
      command != "RDY" && command != "TRF") {
    return;
  }
  std::lock_guard lock(g_lobby.mutex);
  Lobby& lobby = g_lobby;
  if (command == "TRF") {
    for (uint32_t i = 0; i < lobby.players.size(); ++i) {
      if (lobby.players[i].name == sender) {
        if (lobby.players[i].roster_id != data) {
          lobby.players[i].roster_id = std::string(data);
          REXLOG_INFO("[team] player {} roster id {}", i, lobby.players[i].roster_id);
        }
      }
    }
    RosterMismatch(lobby);
    return;
  }
  if (command == "RDY") {
    for (uint32_t i = 0; i < lobby.players.size(); ++i) {
      if (lobby.players[i].name == sender) {
        SetReady(lobby, i, data == "true");
      }
    }
    return;
  }
  if (command == "UPD") {
    // "i:key=value;key=value"
    const size_t colon = data.find(':');
    const int index = colon == std::string_view::npos ? -1 : ParseInt(data.substr(0, colon));
    const size_t at = data.find("ready=");
    if (index >= 0 && at != std::string_view::npos && (at == colon + 1 || data[at - 1] == ';')) {
      SetReady(lobby, uint32_t(index), data.substr(at + 6, 4) == "true");
    }
    return;
  }
  if (command == "TPP") {
    std::vector<int> before;
    for (const Player& p : lobby.players) {
      before.push_back(p.lock);
    }
    ApplyEntries(lobby, data, SetLock);
    for (uint32_t i = 0; lobby.local == 0 && i < before.size(); ++i) {
      lobby.locks_dirty = lobby.locks_dirty || lobby.players[i].lock != before[i];
    }
    return;
  }
  if (command == "TPT" && !(lobby.players.size() > 0 && lobby.players[0].name == sender)) {
    return;
  }
  ApplyEntries(lobby, data, SetSide);
  LogTable(lobby);
}

}  // namespace nhl3on3::team_lobby

// GetControllerLogin(controller) script native; see the top of this file.
extern "C" REX_FUNC(__imp__sub_82AC9248);
extern "C" REX_FUNC(sub_82AC9248) {
  using namespace nhl3on3::team_lobby;
  const int32_t code = ArgNumber(ctx, base, 0);
  if (code < 98) {
    __imp__sub_82AC9248(ctx, base);
    return;
  }
  const uint32_t text = GuestText(Handle(ctx, base, code), 0);
  ctx.r3.u64 = text;
  sub_829CC260(ctx, base);
}

// StoreUserRecord(record) script native. Starting Lineups stores
// "core_Username=name|S=..|O=..|L=..|P=lock" for each local user when the
// match starts; the local player's lock is shared with the other consoles.
extern "C" REX_FUNC(__imp__sub_82AC92A0);
extern "C" REX_FUNC(sub_82AC92A0) {
  using namespace nhl3on3::team_lobby;
  const std::string record = ArgString(ctx, base, 0);
  std::string message;
  {
    std::lock_guard lock(g_lobby.mutex);
    Lobby& lobby = g_lobby;
    const size_t user = record.find("core_Username="), at = record.find("|P=");
    if (lobby.local >= 0 && uint32_t(lobby.local) < lobby.players.size() &&
        user != std::string::npos && at != std::string::npos) {
      const std::string name = record.substr(user + 14, record.find('|', user) - user - 14);
      if (name == lobby.players[lobby.local].name) {
        SetLock(lobby, lobby.local, LocalLock(ParseInt(std::string_view(record).substr(at + 3))));
        lobby.last_lock_send = Clock::now();
        message = LockMessage(lobby);
      }
    }
  }
  if (!message.empty()) {
    SendToPeers(ctx, base, message);
  }
  __imp__sub_82AC92A0(ctx, base);
}
