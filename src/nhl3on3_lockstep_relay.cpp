// Lockstep input relay for 3+ console matches; see nhl3on3_lockstep_relay.h.
//
// Packets (UDP port kPort, little-endian), header:
//   u32 magic, u8 kind, u8 slot, u8 slot count, u8 reserved
// kInputs (client -> host): u32 frames received, u32 first input number,
//   u16 n, n x input
// kFrames (host -> client): u32 inputs received from that client, u32 first
//   frame number, u16 n, n x (slot count x input)
// kMessages (either way): u32 messages received, u32 first message number,
//   u16 n, n x input; control messages, reliable and in order per link
// input: u8 type, u16 length, data
// Everything not yet acknowledged is resent until it is.

#include "nhl3on3_lockstep_relay.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DECLARE(int32_t, team_play);

// The match module's stream receive callback (stream, ?, ?, data, length):
// takes "gamedata" + u8 type + payload and posts it to the game.
extern "C" REX_FUNC(sub_82515070);

REXCVAR_DEFINE_INT32(lockstep_lead, 0, "NHL3on3/Online",
                     "Client inputs sent ahead of the frames played in a relayed match "
                     "(each is one input period of delay); 0 = from the measured turnaround");

REXCVAR_DEFINE_BOOL(relay_timing, false, "NHL3on3/Online",
                    "Log relayed match timing every 600 frames: how long the local "
                    "input takes to be built into a frame and then played");

REXCVAR_DEFINE_BOOL(lockstep_relay, true, "NHL3on3/Online",
                    "Carry a 3+ console match's lockstep input through the host "
                    "(needs --team_play)");

uint32_t Nhl3on3LocalIp();
std::vector<uint32_t> Nhl3on3GamePeers();
bool Nhl3on3TunnelActive();
void Nhl3on3TunnelSendRelay(const uint8_t* data, size_t size);
bool Nhl3on3TunnelReceiveRelay(std::vector<uint8_t>& packet,
                               std::chrono::steady_clock::time_point& arrived);

namespace nhl3on3::lockstep_relay {
namespace {

using Clock = std::chrono::steady_clock;

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kNoSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kNoSocket = -1;
#endif

constexpr uint16_t kPort = 3660;
constexpr uint32_t kMagic = 0x3152484E;  // "NHR1"
constexpr uint8_t kInputs = 1;
constexpr uint8_t kFrames = 2;
constexpr uint8_t kMessages = 3;
constexpr uint32_t kMaxMessage = 1200;
constexpr size_t kHeaderSize = 8;
constexpr size_t kMaxPacket = 1400;
constexpr uint32_t kMaxInput = 252;  // the game's per-slot receive buffer
constexpr uint32_t kMaxSlots = 12;
constexpr uint32_t kWindow = 8;  // most local inputs sent ahead of the frames played
constexpr uint32_t kMinLead = 1;
constexpr double kMinMarginMs = 4.0;
// After the first kSettleFrames, the lead drops only once it could have been
// lower for this long; each input it drops stalls every console one period.
constexpr uint32_t kSettleFrames = 120;
constexpr uint32_t kPlayedSlots = 64;  // > kWindow
constexpr auto kLowerAfter = std::chrono::seconds(3);
constexpr auto kResend = std::chrono::milliseconds(20);
constexpr uint32_t kDefaultRateMs = 16;  // if the dist has no input rate set

struct Input {
  uint8_t type = 1;
  std::vector<uint8_t> data;
};
using Frame = std::vector<Input>;  // one input per slot

// Reliable, ordered control messages over one link.
struct Channel {
  std::deque<Input> out;  // [out_base, out_base + out.size()) not yet acknowledged
  uint32_t out_base = 0;
  uint32_t in_next = 0;   // next message number expected
  uint32_t ack_sent = ~0u;
  Clock::time_point last_send{};
};

struct Client {
  bool known = false;
  sockaddr_in addr{};
  Channel messages;
  uint32_t inputs_received = 0;  // inputs taken from this client
  uint32_t frames_acked = 0;     // frames it has received
  uint32_t frames_sent = 0;      // frames sent so far (end of the last packet)
  Clock::time_point last_send{};
  std::deque<Input> pending;     // inputs not yet in a frame
};

struct Relay {
  uint32_t dist = 0, slot = 0, count = 0;
  bool host = false;
  // Packets go through the dedicated server's tunnel, and the server is the
  // host: every console, slot 0 included, is a client.
  bool tunnel = false;
  NativeSocket socket = kNoSocket;

  uint32_t inputs_sent = 0;   // local inputs produced
  uint32_t frames_played = 0; // frames handed to the game

  // Input pacing: the dist's input rate and when the next input is due.
  uint32_t rate_ms = 0;
  Clock::time_point next_input{};

  // Frames [frames_base, frames_base + frames.size()): built (host) or
  // received (client) and not yet played or acknowledged by every client.
  std::deque<Frame> frames;
  uint32_t frames_base = 0;

  // Client: local inputs [inputs_acked, inputs_sent) not yet taken by the host,
  // and when each could first be sent (frame N - lead played).
  std::deque<Input> unacked;
  std::deque<Clock::time_point> unacked_times;
  // Client: when each recent frame was played, by frame number mod kPlayedSlots.
  Clock::time_point played_at[kPlayedSlots]{};
  // Client: the input for frame N may be sent once frame N - lead is played,
  // and frame N must be back by the time it is due, lead input periods later.
  // turnaround_ms is the smoothed time from that send window opening to the
  // frame arriving (the trip through the server plus waiting for the other
  // consoles), turnaround_dev_ms its mean deviation.
  double turnaround_ms = 0;
  double turnaround_dev_ms = 0;
  uint32_t lead = kWindow;
  Clock::time_point lower_since{};
  uint32_t inputs_acked = 0;
  uint32_t acked_sent = ~0u;
  Clock::time_point last_send{};

  // Host: one entry per slot (its own slot's pending inputs included).
  std::vector<Client> clients;

  // Client: control messages to and from the host.
  Channel host_messages;

  // Received control messages not yet handed to the game.
  std::deque<Input> inbox;
};

std::mutex g_mutex;
Relay g_relay;

// relay_timing: per-frame times, indexed by frame number mod kTimingSlots,
// and sums over the current reporting period.
constexpr uint32_t kTimingSlots = 64;
constexpr uint32_t kTimingPeriod = 600;

struct TimingStats {
  double build_sum = 0, build_max = 0;  // input produced -> frame ready
  double wait_sum = 0, wait_max = 0;    // frame ready -> played
  double poll_sum = 0, poll_max = 0;    // between empty frame queries
  uint32_t frames = 0, polls = 0, backlog_sum = 0;
};

struct Timing {
  Clock::time_point sent[kTimingSlots]{};   // local input for frame N produced
  Clock::time_point built[kTimingSlots]{};  // frame N built (host) or received (client)
  Clock::time_point last_poll{};            // previous frame query that found none
  TimingStats stats;
};

Timing g_timing;

double Ms(Clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

void TimeFrameReady(uint32_t frame) {
  if (REXCVAR_GET(relay_timing)) {
    g_timing.built[frame % kTimingSlots] = Clock::now();
  }
}

// frame: 1-based number of the frame just played; backlog: frames still ready.
void TimeFramePlayed(uint32_t frame, uint32_t backlog) {
  if (!REXCVAR_GET(relay_timing)) {
    return;
  }
  Timing& t = g_timing;
  TimingStats& s = t.stats;
  const auto now = Clock::now();
  const auto sent = t.sent[frame % kTimingSlots], built = t.built[frame % kTimingSlots];
  t.sent[frame % kTimingSlots] = t.built[frame % kTimingSlots] = {};
  if (sent == Clock::time_point{} || built == Clock::time_point{}) {
    return;
  }
  const double build = Ms(built - sent), wait = Ms(now - built);
  s.build_sum += build;
  s.build_max = std::max(s.build_max, build);
  s.wait_sum += wait;
  s.wait_max = std::max(s.wait_max, wait);
  s.backlog_sum += backlog;
  if (++s.frames < kTimingPeriod) {
    return;
  }
  const Relay& r = g_relay;
  REXLOG_INFO(
      "[relay] timing to frame {}: input to frame ready avg {:.1f} max {:.1f} ms, ready to "
      "played avg {:.1f} max {:.1f} ms, total avg {:.1f} ms; frames left after each play "
      "avg {:.2f}; empty frame queries every {:.1f} ms (max {:.1f}); sent {} played {}",
      frame, s.build_sum / s.frames, s.build_max, s.wait_sum / s.frames, s.wait_max,
      (s.build_sum + s.wait_sum) / s.frames, double(s.backlog_sum) / s.frames,
      s.polls ? s.poll_sum / s.polls : 0.0, s.poll_max, r.inputs_sent, r.frames_played);
  s = TimingStats{};
}

void TimeEmptyQuery() {
  if (!REXCVAR_GET(relay_timing)) {
    return;
  }
  Timing& t = g_timing;
  const auto now = Clock::now();
  if (t.last_poll != Clock::time_point{}) {
    const double gap = Ms(now - t.last_poll);
    t.stats.poll_sum += gap;
    t.stats.poll_max = std::max(t.stats.poll_max, gap);
    ++t.stats.polls;
  }
  t.last_poll = now;
}

std::string FormatIp(uint32_t ip) {
  return fmt::format("{}.{}.{}.{}", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

uint32_t Load32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void Store32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

void CloseSocket(NativeSocket s) {
#if defined(_WIN32)
  closesocket(s);
#else
  close(s);
#endif
}

NativeSocket OpenSocket(uint32_t ip) {
#if defined(_WIN32)
  static const bool started = [] {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  }();
  (void)started;
#endif
  NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kNoSocket) {
    return s;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  addr.sin_addr.s_addr = htonl(ip);
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseSocket(s);
    return kNoSocket;
  }
#if defined(_WIN32)
  u_long nonblocking = 1;
  ioctlsocket(s, FIONBIO, &nonblocking);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
  return s;
}

class Writer {
 public:
  explicit Writer(std::vector<uint8_t>& out) : out_(out) {}
  void U8(uint8_t v) { out_.push_back(v); }
  void U16(uint16_t v) {
    out_.push_back(uint8_t(v));
    out_.push_back(uint8_t(v >> 8));
  }
  void U32(uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      out_.push_back(uint8_t(v >> (i * 8)));
    }
  }
  void Put(const Input& in) {
    U8(in.type);
    U16(uint16_t(in.data.size()));
    out_.insert(out_.end(), in.data.begin(), in.data.end());
  }

 private:
  std::vector<uint8_t>& out_;
};

class Reader {
 public:
  Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
  bool ok() const { return ok_; }
  uint8_t U8() { return Need(1) ? *p_++ : 0; }
  uint16_t U16() {
    if (!Need(2)) return 0;
    const uint16_t v = uint16_t(p_[0] | (p_[1] << 8));
    p_ += 2;
    return v;
  }
  uint32_t U32() {
    if (!Need(4)) return 0;
    const uint32_t v = uint32_t(p_[0]) | (uint32_t(p_[1]) << 8) | (uint32_t(p_[2]) << 16) |
                       (uint32_t(p_[3]) << 24);
    p_ += 4;
    return v;
  }
  Input Get() {
    Input in;
    in.type = U8();
    const uint16_t length = U16();
    if (Need(length)) {
      in.data.assign(p_, p_ + length);
      p_ += length;
    }
    return in;
  }

 private:
  bool Need(size_t n) {
    ok_ = ok_ && size_t(end_ - p_) >= n;
    return ok_;
  }
  const uint8_t* p_;
  const uint8_t* end_;
  bool ok_ = true;
};

size_t InputSize(const Input& in) {
  return 3 + in.data.size();
}

void Header(Writer& w, uint8_t kind) {
  w.U32(kMagic);
  w.U8(kind);
  w.U8(uint8_t(g_relay.slot));
  w.U8(uint8_t(g_relay.count));
  w.U8(0);
}

void SendTo(const sockaddr_in& addr, const std::vector<uint8_t>& packet) {
  if (g_relay.tunnel) {
    Nhl3on3TunnelSendRelay(packet.data(), packet.size());
    return;
  }
  ::sendto(g_relay.socket, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
           reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

uint32_t FramesEnd() {
  return g_relay.frames_base + uint32_t(g_relay.frames.size());
}

// Host: sends a client every frame it has not acknowledged, as many as fit.
void SendFrames(Client& client, Clock::time_point now) {
  Relay& r = g_relay;
  const uint32_t first = std::max(client.frames_acked, r.frames_base);
  std::vector<uint8_t> packet;
  Writer w(packet);
  Header(w, kFrames);
  w.U32(client.inputs_received);
  w.U32(first);
  const size_t count_at = packet.size();
  w.U16(0);
  uint16_t n = 0;
  for (uint32_t f = first; f < FramesEnd(); ++f) {
    const Frame& frame = r.frames[f - r.frames_base];
    size_t size = 0;
    for (const Input& in : frame) {
      size += InputSize(in);
    }
    if (n && packet.size() + size > kMaxPacket) {
      break;
    }
    for (const Input& in : frame) {
      w.Put(in);
    }
    ++n;
  }
  packet[count_at] = uint8_t(n);
  packet[count_at + 1] = uint8_t(n >> 8);
  SendTo(client.addr, packet);
  client.frames_sent = first + n;
  client.last_send = now;
}

// Host: turns inputs held from every slot into frames.
void BuildFrames() {
  Relay& r = g_relay;
  for (;;) {
    for (const Client& c : r.clients) {
      if (c.pending.empty()) {
        return;
      }
    }
    Frame frame;
    frame.reserve(r.clients.size());
    for (Client& c : r.clients) {
      frame.push_back(std::move(c.pending.front()));
      c.pending.pop_front();
    }
    r.frames.push_back(std::move(frame));
    const uint32_t built = FramesEnd();
    TimeFrameReady(built);
    if (built == 1 || built % 600 == 0) {
      REXLOG_INFO("[relay] host built frame {} (played {})", built, r.frames_played);
    }
  }
}

// Host: drops frames every client has acknowledged and the host has played.
void TrimFrames() {
  Relay& r = g_relay;
  uint32_t keep = r.frames_played;
  for (uint32_t s = 0; s < r.count; ++s) {
    if (s != r.slot) {
      keep = std::min(keep, r.clients[s].frames_acked);
    }
  }
  while (r.frames_base < keep && !r.frames.empty()) {
    r.frames.pop_front();
    ++r.frames_base;
  }
}

void HandleInputs(Reader& rd, uint8_t slot, const sockaddr_in& from) {
  Relay& r = g_relay;
  if (slot >= r.count || slot == r.slot) {
    return;
  }
  Client& c = r.clients[slot];
  if (!c.known) {
    c.known = true;
    REXLOG_INFO("[relay] host: slot {} is {}:{}", slot, FormatIp(ntohl(from.sin_addr.s_addr)),
                ntohs(from.sin_port));
  }
  c.addr = from;
  const uint32_t acked = rd.U32();
  const uint32_t first = rd.U32();
  const uint16_t n = rd.U16();
  c.frames_acked = std::max(c.frames_acked, std::min(acked, FramesEnd()));
  for (uint32_t i = 0; i < n; ++i) {
    Input in = rd.Get();
    if (!rd.ok()) {
      return;
    }
    if (first + i == c.inputs_received) {
      c.pending.push_back(std::move(in));
      ++c.inputs_received;
    }
  }
}

// Client: the lead from lockstep_lead, or enough input periods to cover the
// turnaround plus twice its deviation.
void UpdateLead(Clock::time_point now) {
  Relay& r = g_relay;
  uint32_t lead = uint32_t(std::max(REXCVAR_GET(lockstep_lead), 0));
  const bool fixed = lead != 0;
  if (!fixed && r.rate_ms && r.turnaround_ms > 0) {
    const double needed =
        r.turnaround_ms + std::max(2.0 * r.turnaround_dev_ms, kMinMarginMs);
    lead = uint32_t(std::ceil(needed / r.rate_ms));
  }
  lead = lead ? std::clamp(lead, kMinLead, kWindow) : kWindow;
  // Settle freely over the first frames (the match is still starting up).
  if (lead < r.lead && !fixed && r.frames_played >= kSettleFrames) {
    if (r.lower_since == Clock::time_point{}) {
      r.lower_since = now;
    }
    if (now - r.lower_since < kLowerAfter) {
      return;
    }
  }
  r.lower_since = {};
  if (lead != r.lead) {
    REXLOG_INFO("[relay] client lead {} inputs (turnaround {:.0f} +/- {:.0f} ms, input rate {} ms)",
                lead, r.turnaround_ms, r.turnaround_dev_ms, r.rate_ms);
    r.lead = lead;
  }
}

void HandleFrames(Reader& rd, uint8_t count, Clock::time_point arrived) {
  Relay& r = g_relay;
  if (count != r.count) {
    return;
  }
  const uint32_t inputs_received = rd.U32();
  const uint32_t first = rd.U32();
  const uint16_t n = rd.U16();
  while (r.inputs_acked < inputs_received && !r.unacked.empty()) {
    const double sample = Ms(arrived - r.unacked_times.front());
    if (r.turnaround_ms > 0) {
      r.turnaround_dev_ms += (std::fabs(sample - r.turnaround_ms) - r.turnaround_dev_ms) * 0.25;
      r.turnaround_ms += (sample - r.turnaround_ms) * 0.125;
    } else {
      r.turnaround_ms = sample;
      r.turnaround_dev_ms = sample / 2;
    }
    r.unacked.pop_front();
    r.unacked_times.pop_front();
    ++r.inputs_acked;
  }
  UpdateLead(Clock::now());
  for (uint32_t i = 0; i < n; ++i) {
    Frame frame(r.count);
    for (Input& in : frame) {
      in = rd.Get();
    }
    if (!rd.ok()) {
      return;
    }
    if (first + i == FramesEnd()) {
      r.frames.push_back(std::move(frame));
      const uint32_t received = FramesEnd();
      TimeFrameReady(received);
      if (received == 1 || received % 600 == 0) {
        REXLOG_INFO("[relay] client received frame {} (played {})", received, r.frames_played);
      }
    }
  }
}

// Client: where the host may be; every address its game link talks to.
std::vector<sockaddr_in> HostAddrs() {
  if (g_relay.tunnel) {
    return {sockaddr_in{}};  // the server; SendTo ignores the address
  }
  std::vector<sockaddr_in> out;
  for (uint32_t ip : Nhl3on3GamePeers()) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(ip);
    out.push_back(addr);
  }
  return out;
}

// Sends a link's unacknowledged control messages and its receive ack.
void SendChannel(Channel& ch, const std::vector<sockaddr_in>& to, Clock::time_point now) {
  std::vector<uint8_t> packet;
  Writer w(packet);
  Header(w, kMessages);
  w.U32(ch.in_next);
  w.U32(ch.out_base);
  const size_t count_at = packet.size();
  w.U16(0);
  uint16_t n = 0;
  for (const Input& in : ch.out) {
    if (n && packet.size() + InputSize(in) > kMaxPacket) {
      break;
    }
    w.Put(in);
    ++n;
  }
  packet[count_at] = uint8_t(n);
  packet[count_at + 1] = uint8_t(n >> 8);
  for (const sockaddr_in& addr : to) {
    SendTo(addr, packet);
  }
  ch.ack_sent = ch.in_next;
  ch.last_send = now;
}

void PumpChannel(Channel& ch, const std::vector<sockaddr_in>& to, Clock::time_point now) {
  const bool resend = !ch.out.empty() && now - ch.last_send >= kResend;
  if (resend || ch.ack_sent != ch.in_next) {
    SendChannel(ch, to, now);
  }
}

void HandleMessages(Reader& rd, uint8_t slot, const sockaddr_in& from) {
  Relay& r = g_relay;
  Channel* ch = &r.host_messages;
  if (r.host) {
    if (slot >= r.count || slot == r.slot) {
      return;
    }
    Client& c = r.clients[slot];
    c.known = true;
    c.addr = from;
    ch = &c.messages;
  }
  const uint32_t acked = rd.U32();
  const uint32_t first = rd.U32();
  const uint16_t n = rd.U16();
  while (ch->out_base < acked && !ch->out.empty()) {
    ch->out.pop_front();
    ++ch->out_base;
  }
  for (uint32_t i = 0; i < n; ++i) {
    Input in = rd.Get();
    if (!rd.ok()) {
      return;
    }
    if (first + i != ch->in_next) {
      continue;
    }
    ++ch->in_next;
    if (r.host) {
      for (uint32_t s = 0; s < r.count; ++s) {
        if (s != slot && s != r.slot) {
          r.clients[s].messages.out.push_back(in);
        }
      }
    }
    REXLOG_INFO("[relay] message type {} len {} from slot {}", in.type, in.data.size(), slot);
    r.inbox.push_back(std::move(in));
  }
}

// Client: sends every input the host has not taken, and the frame ack.
void SendInputs(Clock::time_point now) {
  Relay& r = g_relay;
  std::vector<uint8_t> packet;
  Writer w(packet);
  Header(w, kInputs);
  w.U32(FramesEnd());
  w.U32(r.inputs_acked);
  const size_t count_at = packet.size();
  w.U16(0);
  uint16_t n = 0;
  for (const Input& in : r.unacked) {
    if (n && packet.size() + InputSize(in) > kMaxPacket) {
      break;
    }
    w.Put(in);
    ++n;
  }
  packet[count_at] = uint8_t(n);
  packet[count_at + 1] = uint8_t(n >> 8);
  for (const sockaddr_in& addr : HostAddrs()) {
    SendTo(addr, packet);
  }
  r.acked_sent = FramesEnd();
  r.last_send = now;
  if (r.tunnel) {
    return;
  }
  const std::vector<uint32_t> peers = Nhl3on3GamePeers();
  static bool logged = false, logged_none = false;
  if (peers.empty() && !logged_none) {
    logged_none = true;
    REXLOG_WARN("[relay] client: no game link peers known yet; cannot reach the host");
  }
  if (!logged && !peers.empty()) {
    logged = true;
    std::string list;
    for (uint32_t ip : peers) {
      list += (list.empty() ? "" : ", ") + FormatIp(ip);
    }
    REXLOG_INFO("[relay] client: sending inputs to {}", list);
  }
}

// Reads the next packet into `buffer`; its length, or -1 if none is waiting.
int ReceiveOne(uint8_t* buffer, size_t capacity, sockaddr_in& from, Clock::time_point& arrived) {
  Relay& r = g_relay;
  arrived = Clock::now();
  if (r.tunnel) {
    std::vector<uint8_t> packet;
    if (!Nhl3on3TunnelReceiveRelay(packet, arrived)) {
      return -1;
    }
    const size_t n = std::min(packet.size(), capacity);
    std::memcpy(buffer, packet.data(), n);
    return int(n);
  }
  for (;;) {
#if defined(_WIN32)
    int from_len = sizeof(from);
#else
    socklen_t from_len = sizeof(from);
#endif
    const int n = ::recvfrom(r.socket, reinterpret_cast<char*>(buffer), int(capacity), 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
#if defined(_WIN32)
    if (n < 0 && WSAGetLastError() == WSAECONNRESET) {
      continue;
    }
#endif
    return n;
  }
}

void Receive() {
  Relay& r = g_relay;
  uint8_t buffer[2048];
  for (;;) {
    sockaddr_in from{};
    Clock::time_point arrived;
    const int n = ReceiveOne(buffer, sizeof(buffer), from, arrived);
    if (n < 0) {
      return;
    }
    if (size_t(n) < kHeaderSize) {
      continue;
    }
    Reader rd(buffer, size_t(n));
    if (rd.U32() != kMagic) {
      continue;
    }
    const uint8_t kind = rd.U8(), slot = rd.U8(), count = rd.U8();
    rd.U8();
    if (count != r.count) {
      continue;
    }
    if (r.host && kind == kInputs) {
      HandleInputs(rd, slot, from);
    } else if (!r.host && kind == kFrames) {
      HandleFrames(rd, count, arrived);
    } else if (kind == kMessages && (r.host || slot == 0)) {
      HandleMessages(rd, slot, from);
    }
  }
}

// Exchanges packets; called from every hooked dist call.
void Pump() {
  Relay& r = g_relay;
  if (r.socket == kNoSocket && !r.tunnel) {
    return;
  }
  Receive();
  const auto now = Clock::now();
  if (r.host) {
    BuildFrames();
    for (uint32_t s = 0; s < r.count; ++s) {
      Client& c = r.clients[s];
      if (s == r.slot || !c.known) {
        continue;
      }
      const bool fresh = c.frames_sent < FramesEnd();
      const bool stale = c.frames_acked < FramesEnd() && now - c.last_send >= kResend;
      if (fresh || stale || now - c.last_send >= kResend * 10) {
        SendFrames(c, now);
      }
      PumpChannel(c.messages, {c.addr}, now);
    }
    TrimFrames();
  } else {
    const bool fresh = !r.unacked.empty() && now - r.last_send >= kResend;
    if (fresh || r.acked_sent != FramesEnd() || now - r.last_send >= kResend * 10) {
      SendInputs(now);
    }
    PumpChannel(r.host_messages, HostAddrs(), now);
  }
}

bool Enabled() {
  return REXCVAR_GET(lockstep_relay) && REXCVAR_GET(team_play) >= 3;
}

}  // namespace

void OnMultiSetup(uint32_t dist, uint32_t slot, uint32_t count) {
  if (!Enabled() || count < 3 || count > kMaxSlots || slot >= count) {
    return;
  }
  std::lock_guard lock(g_mutex);
  Relay& r = g_relay;
  if (r.dist == dist && r.slot == slot && r.count == count) {
    return;
  }
  if (r.socket != kNoSocket) {
    CloseSocket(r.socket);
  }
  r = Relay{};
  g_timing = Timing{};
  r.dist = dist;
  r.slot = slot;
  r.count = count;
  r.clients.resize(count);
  if (Nhl3on3TunnelActive()) {
    r.tunnel = true;
    REXLOG_INFO("[relay] dist {:08X}: slot {} of {}, hosted by the server", dist, slot, count);
    return;
  }
  r.host = slot == 0;
  const uint32_t ip = Nhl3on3LocalIp();
  r.socket = OpenSocket(ip);
  if (r.socket == kNoSocket) {
    REXLOG_ERROR("[relay] could not bind {}:{}; lockstep stays on the game's links",
                 FormatIp(ip), kPort);
    r.dist = 0;
    return;
  }
  REXLOG_INFO("[relay] dist {:08X}: slot {} of {} ({}), listening on {}:{}", dist, slot, count,
              r.host ? "host" : "client", FormatIp(ip), kPort);
}

void OnDistDestroyed(uint32_t dist) {
  std::lock_guard lock(g_mutex);
  Relay& r = g_relay;
  if (!dist || dist != r.dist) {
    return;
  }
  REXLOG_INFO("[relay] dist {:08X} destroyed: slot {} of {}, inputs sent {} (taken {}), frames "
              "played {}, messages received {}, sent {} ({} unacknowledged)",
              dist, r.slot, r.count, r.inputs_sent, r.inputs_acked, r.frames_played,
              r.host_messages.in_next, r.host_messages.out_base + r.host_messages.out.size(),
              r.host_messages.out.size());
  if (r.socket != kNoSocket) {
    CloseSocket(r.socket);
  }
  r = Relay{};
  g_timing = Timing{};
}

bool Carries(uint32_t dist) {
  std::lock_guard lock(g_mutex);
  return dist && dist == g_relay.dist;
}

int32_t SendInput(const uint8_t* base, uint32_t dist, uint8_t type, uint32_t data,
                  uint32_t length) {
  std::lock_guard lock(g_mutex);
  Relay& r = g_relay;
  if (dist != r.dist) {
    return 0;
  }
  Input in;
  in.type = type;
  if (data && length) {
    length = std::min(length, kMaxInput);
    in.data.assign(base + data, base + data + length);
  }
  ++r.inputs_sent;
  const auto now = Clock::now();
  if (REXCVAR_GET(relay_timing)) {
    g_timing.sent[r.inputs_sent % kTimingSlots] = now;
  }
  const auto rate = std::chrono::milliseconds(r.rate_ms);
  // Keep the cadence across small scheduling delays, but don't burst to catch
  // up after a long stall.
  r.next_input = (now - r.next_input < rate ? r.next_input : now) + rate;
  if (r.inputs_sent == 1 || r.inputs_sent % 600 == 0) {
    REXLOG_INFO("[relay] input {} type {} len {} (played {})", r.inputs_sent, type,
                in.data.size(), r.frames_played);
  }
  if (r.host) {
    r.clients[r.slot].pending.push_back(std::move(in));
  } else {
    // The send window for this input opened when frame (input - lead) was
    // played; any delay before the game sent it counts against the lead too.
    auto opened = now;
    if (r.inputs_sent > r.lead) {
      const auto played = r.played_at[(r.inputs_sent - r.lead) % kPlayedSlots];
      if (played != Clock::time_point{} && played <= now) {
        opened = played;
      }
    }
    r.unacked.push_back(std::move(in));
    r.unacked_times.push_back(opened);
    r.last_send = {};
  }
  Pump();
  return 1;
}

int32_t QueryFrame(uint8_t* base, uint32_t dist, uint32_t types, uint32_t datas,
                   uint32_t lengths) {
  std::lock_guard lock(g_mutex);
  Relay& r = g_relay;
  if (dist != r.dist) {
    return 0;
  }
  Pump();
  if (r.frames_played >= FramesEnd()) {
    TimeEmptyQuery();
    return 0;
  }
  const Frame& frame = r.frames[r.frames_played - r.frames_base];
  for (uint32_t s = 0; s < r.count; ++s) {
    const Input& in = frame[s];
    const uint32_t length = in.type == 1 ? 0 : uint32_t(in.data.size());
    if (types) {
      base[types + s] = in.type;
    }
    if (lengths) {
      Store32(base + lengths + s * 4, length);
    }
    const uint32_t dest = datas ? Load32(base + datas + s * 4) : 0;
    if (dest && length) {
      std::memcpy(base + dest, in.data.data(), length);
    }
  }
  ++r.frames_played;
  r.played_at[r.frames_played % kPlayedSlots] = Clock::now();
  TimeFramePlayed(r.frames_played, FramesEnd() - r.frames_played);
  if (!r.host) {
    while (r.frames_base < r.frames_played && !r.frames.empty()) {
      r.frames.pop_front();
      ++r.frames_base;
    }
  }
  return int32_t(r.frames_played);
}

int32_t SendWait(uint32_t dist, uint32_t rate_ms, uint32_t& frames_ready) {
  std::lock_guard lock(g_mutex);
  Relay& r = g_relay;
  frames_ready = 0;
  if (dist != r.dist) {
    return 0;
  }
  Pump();
  frames_ready = FramesEnd() - std::min(r.frames_played, FramesEnd());
  if (!rate_ms) {
    rate_ms = kDefaultRateMs;
  }
  if (rate_ms != r.rate_ms) {
    REXLOG_INFO("[relay] input rate {} ms", rate_ms);
    r.rate_ms = rate_ms;
  }
  if (r.inputs_sent - r.frames_played >= (r.host ? kWindow : r.lead)) {
    return r.host ? 10 : int32_t(std::max<uint32_t>(r.rate_ms / 4, 1));
  }
  const auto now = Clock::now();
  if (now >= r.next_input) {
    return 0;
  }
  return int32_t(std::chrono::ceil<std::chrono::milliseconds>(r.next_input - now).count());
}

int32_t SendControl(const uint8_t* base, uint32_t dist, uint8_t type, uint32_t data,
                    uint32_t length) {
  std::lock_guard lock(g_mutex);
  Relay& r = g_relay;
  if (dist != r.dist) {
    return 0;
  }
  Input in;
  in.type = type;
  if (data && length) {
    length = std::min(length, kMaxMessage);
    in.data.assign(base + data, base + data + length);
  }
  REXLOG_INFO("[relay] send message type {} len {}", type, in.data.size());
  if (r.host) {
    for (uint32_t s = 0; s < r.count; ++s) {
      if (s != r.slot) {
        r.clients[s].messages.out.push_back(in);
        r.clients[s].messages.last_send = {};
      }
    }
  } else {
    r.host_messages.out.push_back(std::move(in));
    r.host_messages.last_send = {};
  }
  Pump();
  return 2;
}

void DeliverMessages(const PPCContext& ctx, uint8_t* base) {
  std::deque<Input> inbox;
  {
    std::lock_guard lock(g_mutex);
    if (!g_relay.dist) {
      return;
    }
    Pump();
    inbox.swap(g_relay.inbox);
  }
  for (const Input& in : inbox) {
    // "gamedata" + type + payload, on the guest stack below the caller's frame.
    const uint32_t length = 9 + uint32_t(in.data.size());
    const uint32_t buffer = (ctx.r1.u32 - 0x400 - length) & ~0xFu;
    std::memcpy(base + buffer, "gamedata", 8);
    base[buffer + 8] = in.type;
    if (!in.data.empty()) {
      std::memcpy(base + buffer + 9, in.data.data(), in.data.size());
    }
    PPCContext call = ctx;
    call.r1.u64 = buffer - 0x100;
    call.r3.u64 = 0;
    call.r4.u64 = 0;
    call.r5.u64 = 0;
    call.r6.u64 = buffer;
    call.r7.u64 = length;
    sub_82515070(call, base);
  }
}

}  // namespace nhl3on3::lockstep_relay
