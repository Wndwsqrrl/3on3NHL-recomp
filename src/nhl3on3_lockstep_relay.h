#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

// Carries a 3+ console match's lockstep input through the host instead of the
// game's NetGameDist links. Clients only link to the host, and the 12-slot
// dist mode expects a server to combine every slot's input, so each console
// sends its numbered inputs to the host over UDP, and the host broadcasts
// frame N once it holds input N from every slot. Every console, the host
// included, plays the same frames. Through the dedicated server's tunnel the
// server is the host (server/tunnel.py) and every console is a client.
namespace nhl3on3::lockstep_relay {

// The match's dist object was set up as `slot` of `count` slots.
void OnMultiSetup(uint32_t dist, uint32_t slot, uint32_t count);

// The dist object `dist` is being destroyed: stops relaying it, so a later
// match never pumps the old relay or receives its leftover messages.
void OnDistDestroyed(uint32_t dist);

// Whether the relay carries `dist`'s input.
bool Carries(uint32_t dist);

// Queues one local input blob (guest address and length). Returns 1.
int32_t SendInput(const uint8_t* base, uint32_t dist, uint8_t type, uint32_t data,
                  uint32_t length);

// Fills the guest per-slot arrays (u8 types, u32 data buffer pointers, u32
// lengths) with the next frame. Returns its 1-based number, or 0 if none is
// ready yet.
int32_t QueryFrame(uint8_t* base, uint32_t dist, uint32_t types, uint32_t datas,
                   uint32_t lengths);

// Milliseconds until another local input may be sent (0 = now), pacing inputs
// at the dist's input rate (`rate_ms`; 0 = not set, paced at 60 Hz) like the game's
// NetGameDistInputCheck. Sets `frames_ready` to the frames received and not
// yet played.
int32_t SendWait(uint32_t dist, uint32_t rate_ms, uint32_t& frames_ready);

// Queues one control message (the match module's "gamedata" stream: ready
// handshakes and other non-input messages). The host forwards each client's
// messages to the other clients. Returns 2 (sent).
int32_t SendControl(const uint8_t* base, uint32_t dist, uint8_t type, uint32_t data,
                    uint32_t length);

// Hands received control messages to the game the way its stream receive
// callback would. Call from a guest call on the match thread.
void DeliverMessages(const PPCContext& ctx, uint8_t* base);

}  // namespace nhl3on3::lockstep_relay
