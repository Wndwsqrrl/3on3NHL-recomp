// Xbox LIVE emulation for online play.
//
// The SDK's XNet layer is a stub (no link, loopback address, empty service
// lookups), so the game never gets past its network checks. With --online the
// overrides below present a signed-in LIVE profile and a working XNet stack:
//   - XNADDRs carry the machine's real IPv4 address, so peers can resolve each
//     other with XNetXnAddrToInAddr.
//   - DNS and LSP (XNetServerToInAddr) lookups resolve to --online_server,
//     where a Blaze-compatible server is expected.
// Every lookup and outgoing connection is logged with the "[net]" prefix.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <random>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xsocket.h>
#include <rex/system/xmemory.h>

#include "nhl3on3_roster.h"
#include "nhl3on3_team_lobby.h"
#include "runtime_export.h"

REXCVAR_DEFINE_BOOL(online, false, "NHL3on3/Online",
                    "Emulate a signed-in Xbox LIVE profile and network stack for online play")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(online_server, "nhl3on3-turtle.fly.dev", "NHL3on3/Online",
                      "Host or IPv4 address of the Blaze server; all game DNS/LSP lookups "
                      "resolve here")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(online_ip, "", "NHL3on3/Online",
                      "IPv4 address advertised to other players (empty = detect the LAN "
                      "address; set to a VPN address such as Radmin/ZeroTier to play over it)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(online_xuid, "", "NHL3on3/Online",
                      "Online XUID as 16 hex digits (empty = derived from the advertised IP)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(online_gamertag, "", "NHL3on3/Online",
                      "Gamertag shown online, up to 15 characters (empty = profile name); "
                      "each local instance needs its own")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(online_local_users, 1, "NHL3on3/Online",
                     "Local players signed in to LIVE (1-4), one per controller; extra "
                     "players get their own XUID and a guest-style gamertag such as 'Name(1)'")
    .range(1, 4)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(team_play, 8, "NHL3on3/Online",
                     "Most consoles in an online team play match (3-8). Team play is the "
                     "online menu's ONLINE TEAM PLAY entry (the renamed unranked quick "
                     "match): the server opens a game once two players search, others "
                     "join it on Select Sides, and it starts when everyone is ready. "
                     "0 = off (unranked is the stock two-console game)")
    .range(0, 8)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(online_tunnel, true, "NHL3on3/Online",
                    "Send game traffic between players through the online server, so no "
                    "ports need forwarding (needs the dedicated server, server/; falls back "
                    "to direct play if it does not answer); off = connect to players directly")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(online_service_port, 42127, "NHL3on3/Online",
                     "Port reported for the game's Xbox LIVE service (XOnlineGetServiceInfo)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

using nhl3on3::GuestFunc;
using nhl3on3::ResolveRuntimeExport;

constexpr uint32_t kSigninStateSignedInToLive = 2;
constexpr uint32_t kEthernetLinkActive100Full = 0x01 | 0x02 | 0x08;
// XNET_GET_XNADDR_DHCP | _GATEWAY | _DNS | _ONLINE
constexpr uint32_t kXnAddrStatusOnline = 0x08 | 0x20 | 0x40 | 0x80;
constexpr uint32_t kConnectStatusConnected = 2;
// XNET_XNQOSINFO_COMPLETE | _TARGET_CONTACTED
constexpr uint8_t kQosInfoContacted = 0x01 | 0x02;
constexpr uint16_t kVdpPort = 1000;

uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void StoreBE16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
void StoreBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
void StoreBE64(uint8_t* p, uint64_t v) {
  StoreBE32(p, uint32_t(v >> 32));
  StoreBE32(p + 4, uint32_t(v));
}

std::string FormatIp(uint32_t ip) {
  return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xFF) + "." +
         std::to_string((ip >> 8) & 0xFF) + "." + std::to_string(ip & 0xFF);
}

// Guest XNet structures (big-endian, IN_ADDRs stored in network order).
struct XnAddr {
  uint8_t ina[4];
  uint8_t ina_online[4];
  uint8_t port_online[2];
  uint8_t enet[6];
  uint8_t online[20];
};
static_assert(sizeof(XnAddr) == 36);

struct XnDns {
  uint8_t status[4];
  uint8_t count[4];
  uint8_t addrs[8][4];
};

struct XnQosInfo {
  uint8_t flags;
  uint8_t reserved;
  uint8_t probes_xmit[2];
  uint8_t probes_recv[2];
  uint8_t data_size[2];
  uint8_t data_ptr[4];
  uint8_t rtt_min[2];
  uint8_t rtt_med[2];
  uint8_t up_bps[4];
  uint8_t down_bps[4];
};
static_assert(sizeof(XnQosInfo) == 24);

struct XnQos {
  uint8_t count[4];
  uint8_t count_pending[4];
  XnQosInfo info[1];
};

struct NetState {
  uint32_t local_ip = 0x7F000001;
  uint32_t server_ip = 0x7F000001;
  uint64_t xuid = 0;
};

uint32_t ResolveIpv4(const std::string& host) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) {
    return 0;
  }
  uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr);
  freeaddrinfo(result);
  return ip;
}

// The source address the OS would use to reach the internet; no packet is sent.
uint32_t DetectLocalIp() {
  auto sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(53);
  inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
  uint32_t ip = 0;
  if (connect(sock, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
      ip = ntohl(local.sin_addr.s_addr);
    }
  }
#if defined(_WIN32)
  closesocket(sock);
#else
  close(sock);
#endif
  return ip;
}

bool Online() {
  return REXCVAR_GET(online);
}

//----------------------------------------------------------------------------
// Tunnel through the dedicated server (server/tunnel.py). The server assigns
// this instance a virtual address (10.66.x.y) that it advertises instead of
// its own; datagrams to another virtual address are wrapped and sent to the
// server, which forwards them to that player. Every player only talks to the
// server, so nobody needs open ports.
//
// Packets (little-endian), header: u32 magic, u8 kind, 3 reserved bytes
//   kHello     u32 nonce, u32 wanted address, u32 roster
//                                               kWelcome  u32 nonce, u32 address
//   kKeepalive u32 address, u32 roster
//              roster: the roster pack id (0 = stock); quick match only pairs
//              games with the same one
//   kData      out: u32 dest ip, u16 dest port, u16 source port, payload
//              in:  u32 source ip, u16 source port, u16 dest port, payload
//   kRelay     one lockstep relay packet (nhl3on3_lockstep_relay.cpp)
//----------------------------------------------------------------------------

#ifdef _WIN32
using TunnelSocket = SOCKET;
constexpr TunnelSocket kNoTunnelSocket = INVALID_SOCKET;
#else
using TunnelSocket = int;
constexpr TunnelSocket kNoTunnelSocket = -1;
#endif

constexpr uint16_t kTunnelPort = 3670;
constexpr uint32_t kTunnelMagic = 0x3154484E;  // "NHT1"
constexpr uint8_t kTunnelHello = 1, kTunnelWelcome = 2, kTunnelKeepalive = 3, kTunnelData = 4,
                  kTunnelRelay = 5;
constexpr uint32_t kTunnelNet = 0x0A420000, kTunnelMask = 0xFFFF0000;
constexpr size_t kTunnelHeader = 8;
constexpr size_t kTunnelQueueLimit = 256;
constexpr auto kTunnelKeepaliveInterval = std::chrono::seconds(5);

struct TunnelDatagram {
  uint32_t ip = 0;
  uint16_t port = 0;
  std::vector<uint8_t> data;
};

struct Tunnel {
  TunnelSocket socket = kNoTunnelSocket;
  sockaddr_in server{};
  std::atomic<uint32_t> vip{0};
  std::mutex mutex;
  std::unordered_map<uint16_t, std::deque<TunnelDatagram>> inbox;  // by virtual port
  std::deque<std::pair<std::chrono::steady_clock::time_point, std::vector<uint8_t>>> relay_inbox;
  // Datagram sockets bind an ephemeral native port; peers address them by the
  // port the game asked for (by guest socket), so instances sharing one PC
  // can all use the game's fixed ports.
  std::unordered_map<uint32_t, uint16_t> ports;
};

Tunnel g_tunnel;

// Signals overlapped receives waiting on a socket bound to `port`.
void WakeTunnelReceivers(uint16_t port);
bool IsDatagram(uint32_t socket);
bool NativeHandle(uint32_t socket, TunnelSocket& out);

uint16_t NativeLocalPort(TunnelSocket native) {
  sockaddr_in local{};
  socklen_t len = sizeof(local);
  if (::getsockname(native, reinterpret_cast<sockaddr*>(&local), &len) != 0) {
    return 0;
  }
  return ntohs(local.sin_port);
}

void PutLE16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
}
void PutLE32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(v >> (i * 8)));
  }
}
uint16_t GetLE16(const uint8_t* p) {
  return uint16_t(p[0] | (p[1] << 8));
}
uint32_t GetLE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

std::vector<uint8_t> TunnelPacket(uint8_t kind, size_t reserve = 0) {
  std::vector<uint8_t> packet;
  packet.reserve(kTunnelHeader + reserve);
  PutLE32(packet, kTunnelMagic);
  packet.push_back(kind);
  packet.insert(packet.end(), 3, 0);
  return packet;
}

uint32_t TunnelRosterId() {
  const std::string id = nhl3on3::RosterOnlineId();
  if (id == "stock") {
    return 0;
  }
  const uint32_t value = uint32_t(std::strtoul(id.c_str(), nullptr, 16));
  return value ? value : 1;
}

int TunnelSend(const std::vector<uint8_t>& packet) {
  return ::sendto(g_tunnel.socket, reinterpret_cast<const char*>(packet.data()),
                  int(packet.size()), 0, reinterpret_cast<const sockaddr*>(&g_tunnel.server),
                  sizeof(g_tunnel.server));
}

bool TunnelReadable(int timeout_ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(g_tunnel.socket, &set);
  timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  return ::select(int(g_tunnel.socket) + 1, &set, nullptr, nullptr, &timeout) > 0;
}

void TunnelReceiveOne() {
  uint8_t buffer[2048];
  const int n = ::recv(g_tunnel.socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
  if (n < int(kTunnelHeader) || GetLE32(buffer) != kTunnelMagic) {
    return;
  }
  const uint8_t kind = buffer[4];
  const uint8_t* body = buffer + kTunnelHeader;
  const size_t size = size_t(n) - kTunnelHeader;
  if (kind == kTunnelData && size >= 8) {
    TunnelDatagram d;
    d.ip = GetLE32(body);
    d.port = GetLE16(body + 4);
    const uint16_t dest_port = GetLE16(body + 6);
    d.data.assign(body + 8, body + size);
    {
      std::lock_guard lock(g_tunnel.mutex);
      auto& queue = g_tunnel.inbox[dest_port];
      if (queue.size() >= kTunnelQueueLimit) {
        queue.pop_front();
      }
      queue.push_back(std::move(d));
    }
    WakeTunnelReceivers(dest_port);
  } else if (kind == kTunnelRelay) {
    std::lock_guard lock(g_tunnel.mutex);
    if (g_tunnel.relay_inbox.size() >= kTunnelQueueLimit * 2) {
      g_tunnel.relay_inbox.pop_front();
    }
    g_tunnel.relay_inbox.emplace_back(std::chrono::steady_clock::now(),
                                      std::vector<uint8_t>(body, body + size));
  }
}

void TunnelLoop() {
  auto next_keepalive = std::chrono::steady_clock::now();
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_keepalive) {
      auto packet = TunnelPacket(kTunnelKeepalive, 8);
      PutLE32(packet, g_tunnel.vip.load());
      PutLE32(packet, TunnelRosterId());
      TunnelSend(packet);
      next_keepalive = now + kTunnelKeepaliveInterval;
    }
    if (TunnelReadable(250)) {
      TunnelReceiveOne();
    }
  }
}

// Asks the server for a virtual address; 0 if it does not answer.
uint32_t TunnelRegister(uint32_t server_ip) {
  Tunnel& t = g_tunnel;
  t.socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (t.socket == kNoTunnelSocket) {
    return 0;
  }
  sockaddr_in local{};
  local.sin_family = AF_INET;
  ::bind(t.socket, reinterpret_cast<sockaddr*>(&local), sizeof(local));
#ifdef _WIN32
  constexpr DWORD kSioUdpConnReset = _WSAIOW(IOC_VENDOR, 12);
  BOOL report = FALSE;
  DWORD returned = 0;
  WSAIoctl(t.socket, kSioUdpConnReset, &report, sizeof(report), nullptr, 0, &returned, nullptr,
           nullptr);
#endif
  t.server.sin_family = AF_INET;
  t.server.sin_port = htons(kTunnelPort);
  t.server.sin_addr.s_addr = htonl(server_ip);
  const uint32_t nonce = uint32_t(std::random_device{}());
  for (int attempt = 0; attempt < 10; ++attempt) {
    auto hello = TunnelPacket(kTunnelHello, 12);
    PutLE32(hello, nonce);
    PutLE32(hello, 0);
    PutLE32(hello, TunnelRosterId());
    TunnelSend(hello);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < until && TunnelReadable(50)) {
      uint8_t buffer[64];
      const int n = ::recv(t.socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
      if (n >= int(kTunnelHeader + 8) && GetLE32(buffer) == kTunnelMagic &&
          buffer[4] == kTunnelWelcome && GetLE32(buffer + kTunnelHeader) == nonce) {
        const uint32_t vip = GetLE32(buffer + kTunnelHeader + 4);
        t.vip.store(vip);
        std::thread(TunnelLoop).detach();
        return vip;
      }
    }
  }
#ifdef _WIN32
  closesocket(t.socket);
#else
  close(t.socket);
#endif
  t.socket = kNoTunnelSocket;
  return 0;
}

bool TunnelActive() {
  return g_tunnel.vip.load() != 0;
}

bool IsTunnelAddress(uint32_t ip) {
  return TunnelActive() && (ip & kTunnelMask) == kTunnelNet;
}

int TunnelSendData(uint32_t ip, uint16_t port, uint16_t source_port, const uint8_t* data,
                   uint32_t length) {
  auto packet = TunnelPacket(kTunnelData, 8 + length);
  PutLE32(packet, ip);
  PutLE16(packet, port);
  PutLE16(packet, source_port);
  packet.insert(packet.end(), data, data + length);
  return TunnelSend(packet) < 0 ? -1 : int(length);
}

void SetVirtualPort(uint32_t socket, uint16_t port) {
  std::lock_guard lock(g_tunnel.mutex);
  if (port) {
    g_tunnel.ports[socket] = port;
  } else {
    g_tunnel.ports.erase(socket);
  }
}

// The port a tunneled socket is known by; 0 if it has none.
uint16_t VirtualPort(uint32_t socket) {
  std::lock_guard lock(g_tunnel.mutex);
  auto it = g_tunnel.ports.find(socket);
  return it == g_tunnel.ports.end() ? 0 : it->second;
}

bool TunnelPending(uint16_t port) {
  std::lock_guard lock(g_tunnel.mutex);
  auto it = g_tunnel.inbox.find(port);
  return it != g_tunnel.inbox.end() && !it->second.empty();
}

bool TunnelPop(uint16_t port, TunnelDatagram& out) {
  std::lock_guard lock(g_tunnel.mutex);
  auto it = g_tunnel.inbox.find(port);
  if (it == g_tunnel.inbox.end() || it->second.empty()) {
    return false;
  }
  out = std::move(it->second.front());
  it->second.pop_front();
  return true;
}

const NetState& State() {
  static const NetState state = [] {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    NetState s;
    if (uint32_t server = ResolveIpv4(REXCVAR_GET(online_server))) {
      s.server_ip = server;
    } else {
      REXLOG_ERROR("[net] could not resolve online_server '{}'", REXCVAR_GET(online_server));
    }
    uint32_t ip = 0;
    if (Online() && REXCVAR_GET(online_tunnel)) {
      ip = TunnelRegister(s.server_ip);
      if (ip) {
        REXLOG_INFO("[net] tunnel: server {}:{} assigned {}", FormatIp(s.server_ip), kTunnelPort,
                    FormatIp(ip));
      } else {
        REXLOG_WARN("[net] tunnel: no answer from {}:{}; playing direct (other players must "
                    "be able to reach this address)",
                    FormatIp(s.server_ip), kTunnelPort);
      }
    }
    if (!ip) {
      const std::string ip_setting = REXCVAR_GET(online_ip);
      ip = ip_setting.empty() ? DetectLocalIp() : ResolveIpv4(ip_setting);
    }
    if (ip) {
      s.local_ip = ip;
    }
    const std::string xuid_setting = REXCVAR_GET(online_xuid);
    if (!xuid_setting.empty()) {
      s.xuid = std::stoull(xuid_setting, nullptr, 16);
    } else {
      s.xuid = 0x0009000000000000ull | (uint64_t(0xEA09) << 32) | s.local_ip;
    }
    REXLOG_INFO("[net] online: local {} server {} xuid {:016X}", FormatIp(s.local_ip),
                FormatIp(s.server_ip), s.xuid);
    return s;
  }();
  return state;
}

// The most recent remote address handed out by XNetXnAddrToInAddr.
std::atomic<uint32_t> g_last_peer_ip{0};

// The match ConnApi's game port, and every address that exchanged datagrams on
// it. Clients only link to the host, so a client's set is just the host.
std::atomic<uint32_t> g_game_port{3659};
std::mutex g_game_peers_mutex;
std::vector<uint32_t> g_game_peers;

void NoteGamePeer(uint32_t ip, uint16_t port) {
  if (port != g_game_port.load()) {
    return;
  }
  std::lock_guard lock(g_game_peers_mutex);
  if (std::find(g_game_peers.begin(), g_game_peers.end(), ip) == g_game_peers.end()) {
    g_game_peers.push_back(ip);
    REXLOG_INFO("[net] game link peer {}:{}", FormatIp(ip), port);
  }
}

void FillXnAddr(XnAddr& xna, uint32_t ip) {
  std::memset(&xna, 0, sizeof(xna));
  StoreBE32(xna.ina, ip);
  StoreBE32(xna.ina_online, ip);
  StoreBE16(xna.port_online, kVdpPort);
  // Deterministic per-address MAC; DirtySDK and others seed IDs from abEnet.
  xna.enet[0] = 0x00;
  xna.enet[1] = 0x22;
  StoreBE32(xna.enet + 2, ip);
  StoreBE32(xna.online, ip);
}

uint32_t LocalUsers() {
  return uint32_t(std::clamp(REXCVAR_GET(online_local_users), 1, 4));
}

bool IsOnlineUser(uint32_t user_index) {
  return Online() && user_index < LocalUsers();
}

uint64_t UserXuid(uint32_t user_index) {
  return State().xuid + (uint64_t(user_index) << 32);
}

// Extra local players are named like Xbox guests: "Gamertag(1)", "Gamertag(2)".
std::string UserGamertag(uint32_t user_index) {
  std::string gamertag = REXCVAR_GET(online_gamertag);
  if (gamertag.empty()) {
    gamertag = REX_KERNEL_STATE()->user_profile()->name();
  }
  if (user_index == 0) {
    return gamertag.substr(0, 15);
  }
  const std::string suffix = "(" + std::to_string(user_index) + ")";
  return gamertag.substr(0, 15 - suffix.size()) + suffix;
}

// Overwrites a NUL-terminated guest name buffer with the user's gamertag. User
// 0 keeps the profile name unless online_gamertag is set.
void StoreGamertag(uint32_t user_index, uint8_t* buffer, uint32_t size) {
  if (size == 0 || (user_index == 0 && REXCVAR_GET(online_gamertag).empty())) {
    return;
  }
  const std::string gamertag = UserGamertag(user_index);
  const size_t n = std::min<size_t>(gamertag.size(), size - 1);
  std::memset(buffer, 0, size);
  std::memcpy(buffer, gamertag.data(), n);
}

bool IsLoopback(uint32_t ip) {
  return (ip >> 24) == 127;
}

uint32_t XnAddrToIp(const XnAddr& xna) {
  uint32_t online = LoadBE32(xna.ina_online);
  return online ? online : LoadBE32(xna.ina);
}

void SignalEvent(uint32_t handle) {
  if (handle) {
    rex::kernel::xboxkrnl::xeNtSetEvent(handle, nullptr);
  }
}

template <typename T>
T* GuestAlloc(uint8_t* base, uint32_t& guest_address) {
  guest_address = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(T));
  auto* host = reinterpret_cast<T*>(base + guest_address);
  std::memset(host, 0, sizeof(T));
  return host;
}

void LogSockaddr(const char* what, uint32_t socket, const uint8_t* sa) {
  uint16_t port = uint16_t((sa[2] << 8) | sa[3]);
  REXLOG_INFO("[net] {}(socket {:X}) -> {}:{}", what, socket, FormatIp(LoadBE32(sa + 4)), port);
}

}  // namespace

#define NHL3ON3_ORIGINAL(name) \
  static const GuestFunc original = ResolveRuntimeExport(#name)

// Logs a call with its arguments and result whenever its result differs from
// the last call with the same first argument, plus every 256th call.
#define NHL3ON3_TRACE(name)                                                              \
  static std::array<std::atomic<uint64_t>, 16> trace_last_{};                            \
  static std::atomic<uint32_t> trace_count_{0};                                          \
  const uint32_t trace_args_[4] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};      \
  struct TraceOnExit {                                                                   \
    PPCContext& ctx;                                                                     \
    const uint32_t* args;                                                                \
    ~TraceOnExit() {                                                                     \
      if (!Online()) {                                                                   \
        return;                                                                          \
      }                                                                                  \
      uint64_t key = (uint64_t(args[0]) << 32) | ctx.r3.u32 | (1ull << 63);              \
      uint32_t n = trace_count_++;                                                       \
      auto& slot = trace_last_[(args[0] ^ (args[0] >> 4)) & 15];                         \
      if (slot.exchange(key) != key || n % 256 == 0) {                                   \
        REXLOG_INFO("[net] " name "({:X}, {:X}, {:X}, {:X}) = {:X}  [call {}]", args[0], \
                    args[1], args[2], args[3], ctx.r3.u32, n + 1);                       \
      }                                                                                  \
    }                                                                                    \
  } trace_on_exit_{ctx, trace_args_}

// Forwards to the SDK implementation, tracing the call.
#define NHL3ON3_TRACE_PASSTHROUGH(name)          \
  extern "C" REX_FUNC(__imp__##name) {           \
    NHL3ON3_ORIGINAL(__imp__##name);             \
    NHL3ON3_TRACE(#name);                        \
    original(ctx, base);                         \
  }

//----------------------------------------------------------------------------
// Profile: signed in to LIVE with an online XUID.
//----------------------------------------------------------------------------

extern "C" REX_FUNC(__imp__XamUserGetSigninState) {
  NHL3ON3_TRACE("XamUserGetSigninState");
  NHL3ON3_ORIGINAL(__imp__XamUserGetSigninState);
  if (!Online()) {
    return original(ctx, base);
  }
  ctx.r3.u64 = IsOnlineUser(ctx.r3.u32) ? kSigninStateSignedInToLive : 0;
}

extern "C" REX_FUNC(__imp__XamUserGetSigninInfo) {
  NHL3ON3_TRACE("XamUserGetSigninInfo");
  NHL3ON3_ORIGINAL(__imp__XamUserGetSigninInfo);
  const uint32_t user_index = ctx.r3.u32;
  const uint32_t info = ctx.r5.u32;
  original(ctx, base);
  if (IsOnlineUser(user_index) && info) {
    StoreBE64(base + info, UserXuid(user_index));
    StoreBE32(base + info + 12, kSigninStateSignedInToLive);
    // X_USER_SIGNIN_INFO::szUserName[16]
    StoreGamertag(user_index, base + info + 0x18, 16);
    ctx.r3.u64 = 0;
  }
}

// (user_index, char* buffer, buffer_len)
extern "C" REX_FUNC(__imp__XamUserGetName) {
  NHL3ON3_TRACE("XamUserGetName");
  NHL3ON3_ORIGINAL(__imp__XamUserGetName);
  const uint32_t user_index = ctx.r3.u32, buffer = ctx.r4.u32, length = ctx.r5.u32;
  original(ctx, base);
  if (IsOnlineUser(user_index) && buffer && length) {
    StoreGamertag(user_index, base + buffer, std::min<uint32_t>(length, 16));
    ctx.r3.u64 = 0;
  }
}

extern "C" REX_FUNC(__imp__XamUserGetXUID) {
  NHL3ON3_TRACE("XamUserGetXUID");
  NHL3ON3_ORIGINAL(__imp__XamUserGetXUID);
  if (!IsOnlineUser(ctx.r3.u32) || !ctx.r5.u32) {
    return original(ctx, base);
  }
  StoreBE64(base + ctx.r5.u32, UserXuid(ctx.r3.u32));
  ctx.r3.u64 = 0;
}

extern "C" REX_FUNC(__imp__XamUserCheckPrivilege) {
  NHL3ON3_TRACE("XamUserCheckPrivilege");
  NHL3ON3_ORIGINAL(__imp__XamUserCheckPrivilege);
  if (!Online()) {
    return original(ctx, base);
  }
  if (ctx.r5.u32) {
    StoreBE32(base + ctx.r5.u32, 1);
  }
  ctx.r3.u64 = 0;
}

//----------------------------------------------------------------------------
// XNet addressing.
//----------------------------------------------------------------------------

extern "C" REX_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus) {
  NHL3ON3_TRACE("XNetGetEthernetLinkStatus");
  NHL3ON3_ORIGINAL(__imp__NetDll_XNetGetEthernetLinkStatus);
  if (!Online()) {
    return original(ctx, base);
  }
  ctx.r3.u64 = kEthernetLinkActive100Full;
}

extern "C" REX_FUNC(__imp__NetDll_XNetGetTitleXnAddr) {
  NHL3ON3_TRACE("XNetGetTitleXnAddr");
  NHL3ON3_ORIGINAL(__imp__NetDll_XNetGetTitleXnAddr);
  if (!Online()) {
    return original(ctx, base);
  }
  if (ctx.r4.u32) {
    FillXnAddr(*reinterpret_cast<XnAddr*>(base + ctx.r4.u32), State().local_ip);
  }
  ctx.r3.u64 = kXnAddrStatusOnline;
}

// (caller, const XNADDR*, const XNKID*, IN_ADDR* out)
extern "C" REX_FUNC(__imp__NetDll_XNetXnAddrToInAddr) {
  NHL3ON3_TRACE("XNetXnAddrToInAddr");
  NHL3ON3_ORIGINAL(__imp__NetDll_XNetXnAddrToInAddr);
  if (!Online()) {
    return original(ctx, base);
  }
  if (!ctx.r4.u32 || !ctx.r6.u32) {
    ctx.r3.u64 = 0x2726;  // WSAEINVAL
    return;
  }
  uint32_t ip = XnAddrToIp(*reinterpret_cast<const XnAddr*>(base + ctx.r4.u32));
  StoreBE32(base + ctx.r6.u32, ip);
  if (ip != State().local_ip) {
    g_last_peer_ip.store(ip);
  }
  REXLOG_INFO("[net] XNetXnAddrToInAddr -> {}", FormatIp(ip));
  ctx.r3.u64 = 0;
}

// (caller, IN_ADDR, XNADDR* out, XNKID* out)
extern "C" REX_FUNC(__imp__NetDll_XNetInAddrToXnAddr) {
  NHL3ON3_TRACE("XNetInAddrToXnAddr");
  NHL3ON3_ORIGINAL(__imp__NetDll_XNetInAddrToXnAddr);
  if (!Online()) {
    return original(ctx, base);
  }
  if (ctx.r5.u32) {
    FillXnAddr(*reinterpret_cast<XnAddr*>(base + ctx.r5.u32), ctx.r4.u32);
  }
  if (ctx.r6.u32) {
    std::memset(base + ctx.r6.u32, 0, 8);
  }
  ctx.r3.u64 = 0;
}

// (caller, IN_ADDR server, DWORD service_id, IN_ADDR* out)
extern "C" REX_FUNC(__imp__NetDll_XNetServerToInAddr) {
  NHL3ON3_TRACE("XNetServerToInAddr");
  if (!Online()) {
    ctx.r3.u64 = 0x2726;
    return;
  }
  REXLOG_INFO("[net] XNetServerToInAddr({}, service {:08X})", FormatIp(ctx.r4.u32),
              ctx.r5.u32);
  if (ctx.r6.u32) {
    StoreBE32(base + ctx.r6.u32, ctx.r4.u32);
  }
  ctx.r3.u64 = 0;
}

extern "C" REX_FUNC(__imp__NetDll_XNetConnect) {
  NHL3ON3_TRACE("XNetConnect");
  if (Online()) {
    REXLOG_INFO("[net] XNetConnect({})", FormatIp(ctx.r4.u32));
  }
  ctx.r3.u64 = 0;
}

extern "C" REX_FUNC(__imp__NetDll_XNetGetConnectStatus) {
  NHL3ON3_TRACE("XNetGetConnectStatus");
  ctx.r3.u64 = Online() ? kConnectStatusConnected : 0;
}

//----------------------------------------------------------------------------
// Session keys. Secure-association keys are meaningless without the real XNet
// crypto, but the game must get valid-looking ones back.
//----------------------------------------------------------------------------

// (caller, XNKID* out, XNKEY* out)
extern "C" REX_FUNC(__imp__NetDll_XNetCreateKey) {
  NHL3ON3_TRACE("XNetCreateKey");
  static std::mutex mutex;
  static std::mt19937_64 rng{std::random_device{}()};
  std::lock_guard lock(mutex);
  if (ctx.r4.u32) {
    uint8_t* kid = base + ctx.r4.u32;
    StoreBE64(kid, rng());
    kid[0] = uint8_t((kid[0] & 0x0F) | 0x80);  // XNET_XNKID_ONLINE_PEER
  }
  if (ctx.r5.u32) {
    StoreBE64(base + ctx.r5.u32, rng());
    StoreBE64(base + ctx.r5.u32 + 8, rng());
  }
  ctx.r3.u64 = 0;
}

extern "C" REX_FUNC(__imp__NetDll_XNetRegisterKey) {
  NHL3ON3_TRACE("XNetRegisterKey");
  ctx.r3.u64 = 0;
}

extern "C" REX_FUNC(__imp__NetDll_XNetUnregisterKey) {
  NHL3ON3_TRACE("XNetUnregisterKey");
  ctx.r3.u64 = 0;
}

// (app, message, arg1, arg2). DirtySDK requires XOnlineGetServiceInfo (app FC,
// message 58007) to report an address for the title's LIVE service, otherwise
// NetConn fails with '-svc' and the game shows "sign-in required".
extern "C" REX_FUNC(__imp__XMsgInProcessCall) {
  NHL3ON3_ORIGINAL(__imp__XMsgInProcessCall);
  constexpr uint32_t kXLiveBaseApp = 0xFC;
  constexpr uint32_t kGetServiceInfo = 0x00058007;
  if (!Online() || ctx.r3.u32 != kXLiveBaseApp || ctx.r4.u32 != kGetServiceInfo) {
    static std::mutex seen_mutex;
    static std::set<uint64_t> seen;
    const uint64_t key = (uint64_t(ctx.r3.u32) << 32) | ctx.r4.u32;
    const uint32_t app = ctx.r3.u32, message = ctx.r4.u32;
    original(ctx, base);
    std::lock_guard lock(seen_mutex);
    if (seen.insert(key).second) {
      REXLOG_INFO("[save] XMsgInProcessCall(app {:X}, message {:X}) = {:X} (first call)", app,
                  message, ctx.r3.u32);
    }
    return;
  }
  const uint32_t service_id = ctx.r5.u32, info = ctx.r6.u32;
  const uint16_t port = uint16_t(REXCVAR_GET(online_service_port));
  REXLOG_INFO("[net] XOnlineGetServiceInfo(service {:08X}) -> {}:{}", service_id,
              FormatIp(State().server_ip), port);
  if (info) {
    // XONLINE_SERVICE_INFO { DWORD dwServiceID; IN_ADDR serviceIP; WORD wServicePort; WORD wReserved; }
    StoreBE32(base + info, service_id);
    StoreBE32(base + info + 4, State().server_ip);
    StoreBE16(base + info + 8, port);
    StoreBE16(base + info + 10, 0);
  }
  ctx.r3.u64 = 0;
}

//----------------------------------------------------------------------------
// Service discovery: everything points at the configured server.
//----------------------------------------------------------------------------

// (caller, const char* host, WSAEVENT, XNDNS** out)
extern "C" REX_FUNC(__imp__NetDll_XNetDnsLookup) {
  NHL3ON3_TRACE("XNetDnsLookup");
  NHL3ON3_ORIGINAL(__imp__NetDll_XNetDnsLookup);
  if (!Online()) {
    return original(ctx, base);
  }
  const char* host = ctx.r4.u32 ? reinterpret_cast<const char*>(base + ctx.r4.u32) : "";
  const uint32_t ip = State().server_ip;
  REXLOG_INFO("[net] XNetDnsLookup('{}') -> {}", host, FormatIp(ip));
  if (ctx.r6.u32) {
    uint32_t guest = 0;
    XnDns* dns = GuestAlloc<XnDns>(base, guest);
    StoreBE32(dns->count, 1);
    StoreBE32(dns->addrs[0], ip);
    StoreBE32(base + ctx.r6.u32, guest);
  }
  SignalEvent(ctx.r5.u32);
  ctx.r3.u64 = 0;
}

// (caller, DWORD flags, WSAEVENT, XNQOS** out)
extern "C" REX_FUNC(__imp__NetDll_XNetQosServiceLookup) {
  NHL3ON3_TRACE("XNetQosServiceLookup");
  NHL3ON3_ORIGINAL(__imp__NetDll_XNetQosServiceLookup);
  if (!Online()) {
    return original(ctx, base);
  }
  REXLOG_INFO("[net] XNetQosServiceLookup(flags {:X})", ctx.r4.u32);
  if (ctx.r6.u32) {
    uint32_t guest = 0;
    XnQos* qos = GuestAlloc<XnQos>(base, guest);
    StoreBE32(qos->count, 1);
    XnQosInfo& info = qos->info[0];
    info.flags = kQosInfoContacted;
    StoreBE16(info.probes_xmit, 4);
    StoreBE16(info.probes_recv, 4);
    StoreBE16(info.rtt_min, 20);
    StoreBE16(info.rtt_med, 25);
    StoreBE32(info.up_bps, 10'000'000);
    StoreBE32(info.down_bps, 50'000'000);
    StoreBE32(base + ctx.r6.u32, guest);
  }
  SignalEvent(ctx.r5.u32);
  ctx.r3.u64 = 0;
}

//----------------------------------------------------------------------------
// Socket tracing, to discover which servers and ports the game expects.
//----------------------------------------------------------------------------

extern "C" REX_FUNC(__imp__NetDll_connect) {
  NHL3ON3_TRACE("connect");
  NHL3ON3_ORIGINAL(__imp__NetDll_connect);
  if (Online() && ctx.r5.u32) {
    LogSockaddr("connect", ctx.r4.u32, base + ctx.r5.u32);
  }
  original(ctx, base);
}

// (caller, socket, const sockaddr*, namelen). Instances sharing one PC advertise
// distinct loopback addresses (127.0.0.2, 127.0.0.3, ...); binding each one's
// wildcard sockets to its own address lets both use the game's fixed ports.
// Through the tunnel, datagram sockets bind an ephemeral port instead and are
// known to peers by the requested one (see Tunnel::ports).
extern "C" REX_FUNC(__imp__NetDll_bind) {
  NHL3ON3_TRACE("bind");
  NHL3ON3_ORIGINAL(__imp__NetDll_bind);
  const uint32_t socket = ctx.r4.u32;
  uint8_t* sa = ctx.r5.u32 ? base + ctx.r5.u32 : nullptr;
  const bool tunneled = Online() && sa && (State(), TunnelActive()) && IsDatagram(socket);
  const bool pin = !tunneled && Online() && sa && LoadBE32(sa + 4) == 0 &&
                   IsLoopback(State().local_ip);
  if (Online() && sa) {
    LogSockaddr("bind", socket, sa);
  }
  uint8_t saved[6];
  if (tunneled) {
    std::memcpy(saved, sa + 2, 6);
    std::memset(sa + 2, 0, 6);
  }
  if (pin) {
    StoreBE32(sa + 4, State().local_ip);
  }
  original(ctx, base);
  if (pin) {
    StoreBE32(sa + 4, 0);
  }
  if (tunneled) {
    std::memcpy(sa + 2, saved, 6);
    TunnelSocket native;
    if (ctx.r3.u32 == 0 && NativeHandle(socket, native)) {
      const uint16_t requested = uint16_t((saved[0] << 8) | saved[1]);
      SetVirtualPort(socket, requested ? requested : NativeLocalPort(native));
    }
  }
}

namespace {

// Logs a socket call unless it repeats the previous call to the same function
// with the same arguments and result.
void LogSocketCall(const char* name, std::atomic<uint64_t>& last, const uint32_t args[3],
                   uint32_t result, uint32_t last_error) {
  if (!Online()) {
    return;
  }
  const uint64_t key = (uint64_t(args[0] ^ args[1] * 31 ^ args[2] * 131) << 32) | result;
  if (last.exchange(key) == key) {
    return;
  }
  REXLOG_INFO("[net] {}({:X}, {:X}, {:X}) = {:X}  (WSAGetLastError {})", name, args[0], args[1],
              args[2], result, last_error);
}

}  // namespace

extern "C" REX_FUNC(__imp__NetDll_WSAGetLastError);

namespace {

// The runtime reads the host's last error lazily, so any host call made on the
// guest thread between a failed socket call and WSAGetLastError (logging, XAM)
// can reset it to 0, which DirtySDK treats as a fatal socket error. Socket
// wrappers capture the error at the point of failure instead.
thread_local uint32_t t_socket_error = 0;

// Error the guest's next WSAGetLastError on this thread reports, ahead of the
// runtime's own.
thread_local uint32_t t_forced_error = 0;

uint32_t ResolveStaleSocketError(uint32_t socket, uint32_t error);

uint32_t CaptureSocketError(PPCContext& ctx, uint8_t* base) {
  PPCContext probe = ctx;
  probe.r3.u64 = 1;
  __imp__NetDll_WSAGetLastError(probe, base);
  if (probe.r3.u32) {
    t_socket_error = probe.r3.u32;
  }
  return probe.r3.u32;
}

}  // namespace

// Traces (socket, arg, arg) and the error code the game will see on failure,
// after running `before`; `after` sees `args` and `result`.
#define NHL3ON3_SOCKET_TRACE_WITH(name, before, after)                         \
  extern "C" REX_FUNC(__imp__NetDll_##name) {                                  \
    NHL3ON3_ORIGINAL(__imp__NetDll_##name);                                    \
    before;                                                                    \
    static std::atomic<uint64_t> last{0};                                      \
    const uint32_t args[3] = {ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};             \
    original(ctx, base);                                                       \
    const uint32_t result = ctx.r3.u32;                                        \
    after;                                                                     \
    uint32_t error = 0;                                                        \
    if (result == 0xFFFFFFFF) {                                                \
      error = CaptureSocketError(ctx, base);                                   \
      const uint32_t resolved = ResolveStaleSocketError(args[0], error);       \
      if (resolved != error) {                                                 \
        error = t_socket_error = t_forced_error = resolved;                    \
      }                                                                        \
      if (Online() && error != 10035) {                                        \
        last.store(0);                                                         \
      }                                                                        \
    }                                                                          \
    LogSocketCall(#name, last, args, result, error);                           \
    ctx.r3.u64 = result;                                                       \
  }
#define NHL3ON3_SOCKET_TRACE(name) NHL3ON3_SOCKET_TRACE_WITH(name, (void)0, (void)0)

namespace {

// Whether the current lobby game is ranked: from the SYSFLAGS of the gqwk or
// gcre request, then of the game the server reports. Team play side picks
// only apply to unranked games.
constexpr uint32_t kRankedSysflag = 1u << 18;
std::atomic<bool> g_ranked_search{false};
// Consoles in the current lobby game, from the COUNT of the server's +mgm
// (membership) and +ses (game started) updates.
std::atomic<int> g_game_consoles{0};

// The value of `key` in a lobby body ("\nKEY=value" lines), if present.
std::optional<std::string_view> LobbyField(std::string_view body, std::string_view key) {
  for (size_t at = 0; at < body.size();) {
    size_t end = body.find('\n', at);
    if (end == std::string_view::npos) end = body.size();
    const std::string_view line = body.substr(at, end - at);
    if (line.size() > key.size() && line.substr(0, key.size()) == key && line[key.size()] == '=') {
      return line.substr(key.size() + 1);
    }
    at = end + 1;
  }
  return std::nullopt;
}

// A lobby body on one line for the log, without addresses and passwords.
std::string LobbySummary(std::string_view body) {
  std::string out;
  for (size_t at = 0; at < body.size() && out.size() < 240;) {
    size_t end = body.find_first_of("\n\t", at);
    if (end == std::string_view::npos) end = body.size();
    std::string_view line = body.substr(at, end - at);
    if (const size_t nul = line.find('\0'); nul != std::string_view::npos) {
      line = line.substr(0, nul);
      end = body.size();
    }
    const bool hidden = line.starts_with("MADDR") || line.starts_with("MA=") ||
                        line.starts_with("PASS=") || line.starts_with("LKEY=");
    if (!line.empty() && !hidden) {
      out.append(out.empty() ? "" : " ").append(line);
    }
    at = end + 1;
  }
  return out.size() > 240 ? out.substr(0, 240) + "..." : out;
}

bool RankedFlags(std::string_view value) {
  return (uint32_t(std::strtoul(std::string(value.substr(0, 12)).c_str(), nullptr, 10)) &
          kRankedSysflag) != 0;
}

// (caller, socket, buffer, length): each request that puts the game in a new
// lobby game (gqwk quick match search, gcre create session, gjoi join from
// the Custom Match list) starts a fresh team play lobby.
void NoteLobbySend(const uint8_t* base, uint32_t buffer, uint32_t length) {
  if (!Online() || !buffer || length < 12) {
    return;
  }
  const std::string_view cmd(reinterpret_cast<const char*>(base + buffer), 4);
  const std::string_view body(reinterpret_cast<const char*>(base + buffer + 12), length - 12);
  if (cmd != "~png") {
    REXLOG_INFO("[lobby] -> {} {}", cmd, LobbySummary(body));
  }
  if (cmd != "gqwk" && cmd != "gcre" && cmd != "gjoi") {
    return;
  }
  const auto flags = LobbyField(body, "SYSFLAGS");
  if (cmd == "gqwk" && !flags) {
    return;  // MODE=3 cancels a search
  }
  const bool ranked = flags && RankedFlags(*flags);
  g_ranked_search.store(ranked);
  g_game_consoles.store(0);
  nhl3on3::team_lobby::Reset();
  const char* kind = ranked ? "ranked" : "unranked (team play)";
  if (cmd == "gqwk") {
    REXLOG_INFO("[mm] quick match search: {}", kind);
  } else if (cmd == "gcre") {
    REXLOG_INFO("[mm] create session: {}", kind);
  } else {
    REXLOG_INFO("[mm] join session {}", LobbyField(body, "IDENT").value_or("?"));
  }
}

// (buffer, received bytes): the game reads each lobby packet as a 12-byte
// header (cmd, status, BE size including the header) and then its body; a
// +mgm (membership), +ses (game started), gcre or gjoi body updates the
// console count and whether the game is ranked.
void NoteLobbyRecv(const uint8_t* base, uint32_t buffer, uint32_t received) {
  static thread_local uint32_t pending_body = 0;
  static thread_local char pending_cmd[9] = {};
  static thread_local bool pending_game = false;
  if (!Online() || !buffer || received == 0xFFFFFFFF || received == 0) {
    return;
  }
  const uint8_t* data = base + buffer;
  if (pending_body && received == pending_body) {
    pending_body = 0;
    const std::string body = "\n" + std::string(reinterpret_cast<const char*>(data), received);
    REXLOG_INFO("[lobby] <- {} {}", pending_cmd, LobbySummary(std::string_view(body).substr(1)));
    if (!pending_game) {
      return;
    }
    if (const size_t at = body.find("\nCOUNT="); at != std::string::npos) {
      const int consoles = std::atoi(body.c_str() + at + 7);
      if (g_game_consoles.exchange(consoles) != consoles) {
        REXLOG_INFO("[mm] lobby game has {} consoles", consoles);
      }
    }
    if (const size_t at = body.find("\nSYSFLAGS="); at != std::string::npos) {
      const bool ranked = RankedFlags(std::string_view(body).substr(at + 10));
      if (g_ranked_search.exchange(ranked) != ranked) {
        REXLOG_INFO("[mm] lobby game is {}", ranked ? "ranked" : "unranked (team play)");
      }
    }
    return;
  }
  pending_body = 0;
  if (received != 12 || std::memcmp(data, "~png", 4) == 0) {
    return;
  }
  // The command and its 4-character status (zero on success) for the log.
  std::memcpy(pending_cmd, data, 4);
  pending_cmd[4] = 0;
  if (LoadBE32(data + 4) != 0) {
    std::memcpy(pending_cmd + 4, data + 4, 4);
    pending_cmd[8] = 0;
    for (int i = 4; i < 8; ++i) {
      if (pending_cmd[i] < 0x20 || pending_cmd[i] > 0x7E) pending_cmd[i] = '?';
    }
  }
  // Error replies (a nonzero status) carry no game.
  pending_game = LoadBE32(data + 4) == 0 &&
                 (std::memcmp(data, "+mgm", 4) == 0 || std::memcmp(data, "+ses", 4) == 0 ||
                  std::memcmp(data, "gcre", 4) == 0 || std::memcmp(data, "gjoi", 4) == 0);
  const uint32_t size = LoadBE32(data + 8);
  pending_body = size > 12 ? size - 12 : 0;
  if (!pending_body) {
    REXLOG_INFO("[lobby] <- {}", pending_cmd);
  }
}

}  // namespace

NHL3ON3_SOCKET_TRACE_WITH(recv, (void)0, NoteLobbyRecv(base, args[1], result))
NHL3ON3_SOCKET_TRACE_WITH(send, NoteLobbySend(base, ctx.r5.u32, ctx.r6.u32), (void)0)
std::string Nhl3on3GuestBacktrace(const PPCContext& ctx, const uint8_t* base);

//----------------------------------------------------------------------------
// Datagram sockets. The runtime's UDP path cannot carry peer traffic: sendto
// byte-swaps the destination twice and WSARecvFrom is a stub that fails
// without an error code. Online, datagram sockets bypass it and use their
// native handles directly.
//----------------------------------------------------------------------------

namespace {

void LogOnChange(const char* what, uint64_t key, const std::string& text);

constexpr uint32_t kWsaIoIncomplete = 996;
constexpr uint32_t kWsaIoPending = 997;
constexpr uint32_t kWsaEWouldBlock = 10035;
constexpr uint32_t kWsaEConnReset = 10054;
constexpr uint32_t kLoopbackIp = 0x7F000001;

#ifdef _WIN32
using NativeSocket = SOCKET;
using NativeSockLen = int;
#else
using NativeSocket = int;
using NativeSockLen = socklen_t;
#endif

// An overlapped WSARecvFrom waiting for data; the watcher thread signals its
// event once the socket is readable.
struct PendingRecv {
  uint32_t socket;
  NativeSocket native;
  uint32_t buffer, length, from, fromlen, event;
  bool signaled = false;
  uint16_t port = 0;  // local port, for tunneled datagrams
};

std::mutex g_dgram_mutex;
std::condition_variable g_dgram_cv;
std::unordered_set<uint32_t> g_dgram_sockets;
std::unordered_map<uint32_t, PendingRecv> g_pending_recvs;  // by guest OVERLAPPED

// Logs the first datagram and then every power-of-two count per socket and
// direction, so both ends' traffic is visible without flooding the log.
void CountDatagram(const char* what, uint32_t socket, uint32_t ip, uint16_t port,
                   const uint8_t* data, int length) {
  static std::mutex mutex;
  static std::unordered_map<uint64_t, uint32_t> counts;
  uint32_t n;
  {
    std::lock_guard lock(mutex);
    n = ++counts[(uint64_t(socket) << 1) | (what[0] == 'r')];
  }
  // OSDK session control messages ("osdkctrl" + u32 length + "name|CMD|args"),
  // which carry each player's session attributes.
  const std::string_view view(reinterpret_cast<const char*>(data), size_t(std::max(length, 0)));
  if (const size_t at = view.find("osdkctrl"); at != std::string_view::npos && at + 12 <= view.size()) {
    const uint8_t* p = data + at + 8;
    const size_t text_length = (size_t(p[0]) << 24) | (size_t(p[1]) << 16) | (size_t(p[2]) << 8) | p[3];
    const std::string_view text = view.substr(at + 12, std::min(text_length, view.size() - at - 12));
    REXLOG_INFO("[osdk] {} {} {}: {}", what, what[0] == 'r' ? "from" : "to", FormatIp(ip), text);
    nhl3on3::team_lobby::OnControlMessage(text);
  }
  if (n <= 12 || (n & (n - 1)) == 0) {
    std::string hex;
    for (int i = 0; i < std::min(length, 48); ++i) {
      hex += fmt::format("{:02X}", data[i]);
    }
    REXLOG_INFO("[net] {} {:X} #{} {} {}:{} len {} [{}]", what, socket, n,
                what[0] == 'r' ? "from" : "to", FormatIp(ip), port, length, hex);
  }
}

// Logs each distinct guest call stack that sends on a datagram socket, to find
// the game code behind its in-match traffic.
void LogDatagramStack(uint32_t socket, const PPCContext& ctx, const uint8_t* base) {
  constexpr size_t kMaxStacks = 64;
  static std::mutex mutex;
  static std::unordered_set<std::string> seen;
  const std::string stack = Nhl3on3GuestBacktrace(ctx, base);
  std::lock_guard lock(mutex);
  if (seen.size() < kMaxStacks && seen.insert(stack).second) {
    REXLOG_INFO("[net] send stack {:X}: {}", socket, stack);
  }
}

bool IsDatagram(uint32_t socket) {
  std::lock_guard lock(g_dgram_mutex);
  return g_dgram_sockets.count(socket) != 0;
}

bool NativeHandle(uint32_t socket, NativeSocket& out) {
  auto object = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(socket);
  if (!object) {
    return false;
  }
  out = NativeSocket(object->native_handle());
  return true;
}

// The runtime's stream recv/send can fail without setting a socket error, so
// the guest reads whatever the thread last left behind (e.g. 2 from a missing
// game file, or 0x8007065B from an unimplemented XSession call when a player
// leaves) and DirtySDK drops the lobby connection. A non-Winsock code is
// replaced by the socket's real pending error, or WSAEWOULDBLOCK if none.
uint32_t ResolveStaleSocketError(uint32_t socket, uint32_t error) {
  constexpr uint32_t kSdkWouldBlock = 31;
  const bool winsock = error >= 10000 && error < 12000;
  if (winsock || error == kSdkWouldBlock || error == 996 || error == 997) {
    return error;
  }
  NativeSocket native;
  if (!NativeHandle(socket, native)) {
    return error;
  }
  int pending = 0;
  NativeSockLen len = sizeof(pending);
  if (::getsockopt(native, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&pending), &len) != 0) {
    return error;
  }
  const uint32_t resolved = pending ? uint32_t(pending) : kWsaEWouldBlock;
  static std::atomic<uint64_t> last{0};
  const uint64_t key = (uint64_t(socket) << 32) | (error << 16) ^ resolved;
  if (Online() && last.exchange(key) != key) {
    REXLOG_INFO("[net] socket {:X}: stale error {} replaced by {}", socket, error, resolved);
  }
  return resolved;
}

uint32_t NativeError() {
#ifdef _WIN32
  return uint32_t(WSAGetLastError());
#else
  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    return kWsaEWouldBlock;
  }
  return errno == ECONNREFUSED ? kWsaEConnReset : 10000 + uint32_t(errno);
#endif
}

bool Readable(NativeSocket native) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(native, &set);
  timeval timeout{};
  return ::select(int(native) + 1, &set, nullptr, nullptr, &timeout) > 0;
}

// Receives one pending datagram without blocking, reporting the sender in
// guest XSOCKADDR_IN form. Returns the length, or -1 with `error` set.
// The port peers address `socket` by: its virtual port through the tunnel.
uint16_t SocketPort(uint32_t socket, NativeSocket native) {
  const uint16_t port = VirtualPort(socket);
  return port ? port : NativeLocalPort(native);
}

void StoreFrom(uint8_t* base, uint32_t from, uint32_t fromlen, uint32_t ip, uint16_t port) {
  if (from) {
    uint8_t* sa = base + from;
    StoreBE16(sa, AF_INET);
    StoreBE16(sa + 2, port);
    StoreBE32(sa + 4, ip);
    std::memset(sa + 8, 0, 8);
  }
  if (fromlen) {
    StoreBE32(base + fromlen, 16);
  }
}

int ReceiveDatagram(uint8_t* base, uint32_t socket, NativeSocket native, uint32_t buffer,
                    uint32_t length, uint32_t from, uint32_t fromlen, uint32_t& error) {
  if (TunnelActive()) {
    TunnelDatagram d;
    if (TunnelPop(SocketPort(socket, native), d)) {
      const int n = int(std::min<size_t>(length, d.data.size()));
      std::memcpy(base + buffer, d.data.data(), size_t(n));
      CountDatagram("recv", socket, d.ip, d.port, base + buffer, n);
      NoteGamePeer(d.ip, d.port);
      StoreFrom(base, from, fromlen, d.ip, d.port);
      return n;
    }
  }
  for (;;) {
    if (!Readable(native)) {
      error = kWsaEWouldBlock;
      return -1;
    }
    sockaddr_in addr{};
    NativeSockLen addr_len = sizeof(addr);
    const int n = ::recvfrom(native, reinterpret_cast<char*>(base + buffer), int(length), 0,
                             reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (n < 0) {
      error = NativeError();
      // An ICMP port-unreachable from an earlier send; not a receive failure.
      if (error == kWsaEConnReset) {
        continue;
      }
      return -1;
    }
    const uint32_t ip = ntohl(addr.sin_addr.s_addr);
    CountDatagram("recv", socket, ip, ntohs(addr.sin_port), base + buffer, n);
    NoteGamePeer(ip, ntohs(addr.sin_port));
    StoreFrom(base, from, fromlen, ip, ntohs(addr.sin_port));
    return n;
  }
}

void WakeTunnelReceivers(uint16_t port) {
  std::vector<uint32_t> events;
  {
    std::lock_guard lock(g_dgram_mutex);
    for (auto& [overlapped, op] : g_pending_recvs) {
      if (!op.signaled && op.port == port) {
        op.signaled = true;
        events.push_back(op.event);
      }
    }
  }
  for (uint32_t event : events) {
    SignalEvent(event);
  }
}

void WatchPendingRecvs() {
  for (;;) {
    struct Armed {
      uint32_t overlapped;
      NativeSocket native;
      uint16_t port;
    };
    std::vector<Armed> armed;
    {
      std::unique_lock lock(g_dgram_mutex);
      g_dgram_cv.wait(lock, [&] {
        armed.clear();
        for (const auto& [overlapped, op] : g_pending_recvs) {
          if (!op.signaled) {
            armed.push_back({overlapped, op.native, op.port});
          }
        }
        return !armed.empty();
      });
    }
    fd_set set;
    FD_ZERO(&set);
    int max_fd = 0;
    for (const Armed& a : armed) {
      FD_SET(a.native, &set);
      max_fd = std::max(max_fd, int(a.native));
    }
    timeval timeout{0, 5000};
    int ready = ::select(max_fd + 1, &set, nullptr, nullptr, &timeout);
    if (ready < 0) {
      FD_ZERO(&set);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::vector<uint16_t> tunneled;
    for (const Armed& a : armed) {
      if (TunnelActive() && TunnelPending(a.port)) {
        tunneled.push_back(a.port);
      }
    }
    if (ready <= 0 && tunneled.empty()) {
      continue;
    }
    std::vector<uint32_t> events;
    {
      std::lock_guard lock(g_dgram_mutex);
      for (const Armed& a : armed) {
        auto it = g_pending_recvs.find(a.overlapped);
        const bool has_data =
            (ready > 0 && FD_ISSET(a.native, &set)) ||
            std::find(tunneled.begin(), tunneled.end(), a.port) != tunneled.end();
        if (has_data && it != g_pending_recvs.end() && !it->second.signaled &&
            it->second.native == a.native) {
          it->second.signaled = true;
          events.push_back(it->second.event);
        }
      }
    }
    for (uint32_t event : events) {
      static std::atomic<uint32_t> signals{0};
      const uint32_t n = ++signals;
      if ((n & (n - 1)) == 0) {
        REXLOG_INFO("[net] recv watcher signal #{} event {:X}", n, event);
      }
      SignalEvent(event);
    }
  }
}

void StartRecvWatcher() {
  static std::once_flag once;
  std::call_once(once, [] { std::thread(WatchPendingRecvs).detach(); });
}

}  // namespace

// (caller, socket, buffer, length, flags, sockaddr* from, int* fromlen)
extern "C" REX_FUNC(__imp__NetDll_recvfrom) {
  NHL3ON3_ORIGINAL(__imp__NetDll_recvfrom);
  const uint32_t socket = ctx.r4.u32;
  NativeSocket native;
  if (!Online() || !IsDatagram(socket) || !NativeHandle(socket, native)) {
    return original(ctx, base);
  }
  uint32_t error = 0;
  const int n = ReceiveDatagram(base, socket, native, ctx.r5.u32, ctx.r6.u32, ctx.r8.u32, ctx.r9.u32,
                                error);
  if (n < 0) {
    t_forced_error = error;
  }
  ctx.r3.u64 = uint32_t(n);
}

// (caller, socket, sockaddr* name, int* namelen). The runtime only stubs this.
// CommUDP reads each open socket's bound port to share one socket between all
// peers on the game port; without it a second peer gets a socket of its own
// whose bind fails, and that connection never comes up.
extern "C" REX_FUNC(__imp__NetDll_getsockname) {
  const uint32_t socket = ctx.r4.u32, name = ctx.r5.u32, namelen = ctx.r6.u32;
  NativeSocket native;
  sockaddr_in local{};
  NativeSockLen len = sizeof(local);
  if (!name || !namelen || LoadBE32(base + namelen) < 16 || !NativeHandle(socket, native) ||
      ::getsockname(native, reinterpret_cast<sockaddr*>(&local), &len) != 0) {
    t_forced_error = 10022;  // WSAEINVAL
    ctx.r3.u64 = 0xFFFFFFFF;
    return;
  }
  uint8_t* sa = base + name;
  std::memset(sa, 0, 16);
  StoreBE16(sa, AF_INET);
  const uint16_t virtual_port = VirtualPort(socket);
  StoreBE16(sa + 2, virtual_port ? virtual_port : ntohs(local.sin_port));
  StoreBE32(sa + 4, ntohl(local.sin_addr.s_addr));
  StoreBE32(base + namelen, 16);
  ctx.r3.u64 = 0;
}

// (caller, socket, buffer, length, flags, const sockaddr* to, tolen)
// DirtySDK's send-address remap table turns the peer's address into 127.0.0.1
// on the wire, while CommUDP keeps matching replies against the real peer
// address. Instances sharing one PC listen on their own loopback address, so
// that traffic is redirected to the peer XNetXnAddrToInAddr resolved.
extern "C" REX_FUNC(__imp__NetDll_sendto) {
  NHL3ON3_ORIGINAL(__imp__NetDll_sendto);
  const uint32_t socket = ctx.r4.u32, buffer = ctx.r5.u32, length = ctx.r6.u32,
                 to = ctx.r8.u32;
  NativeSocket native;
  if (!Online() || !to || !IsDatagram(socket) || !NativeHandle(socket, native)) {
    return original(ctx, base);
  }
  const uint8_t* sa = base + to;
  const uint32_t requested = LoadBE32(sa + 4);
  const uint16_t port = uint16_t((sa[2] << 8) | sa[3]);
  const uint32_t peer = g_last_peer_ip.load();
  uint32_t ip = requested;
  if (requested == kLoopbackIp && peer && IsLoopback(State().local_ip) &&
      State().local_ip != kLoopbackIp) {
    ip = peer;
  }
  int n;
  if (IsTunnelAddress(ip)) {
    n = TunnelSendData(ip, port, SocketPort(socket, native), base + buffer, length);
  } else {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(ip);
    n = ::sendto(native, reinterpret_cast<const char*>(base + buffer), int(length), 0,
                 reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  }
  uint32_t error = 0;
  if (n < 0) {
    error = NativeError();
    t_forced_error = error;
  } else {
    CountDatagram("send", socket, ip, port, base + buffer, n);
    NoteGamePeer(ip, port);
    LogDatagramStack(socket, ctx, base);
  }
  LogOnChange("sendto", socket,
              fmt::format("to {}:{}{} = {} error {}", FormatIp(requested), port,
                          ip != requested ? " -> " + FormatIp(ip) : "",
                          n < 0 ? "fail" : "ok", error));
  ctx.r3.u64 = uint32_t(n);
}

// (caller, socket, WSABUF* buffers, count, DWORD* received, DWORD* flags,
//  sockaddr* from, int* fromlen, [stack] OVERLAPPED*, completion routine)
extern "C" REX_FUNC(__imp__NetDll_WSARecvFrom) {
  NHL3ON3_ORIGINAL(__imp__NetDll_WSARecvFrom);
  const uint32_t socket = ctx.r4.u32, buffers = ctx.r5.u32, count = ctx.r6.u32,
                 received = ctx.r7.u32, flags = ctx.r8.u32, from = ctx.r9.u32,
                 fromlen = ctx.r10.u32;
  const uint32_t overlapped = LoadBE32(base + ctx.r1.u32 + 84);
  NativeSocket native;
  if (!Online() || !IsDatagram(socket) || !count || !NativeHandle(socket, native)) {
    return original(ctx, base);
  }
  const uint32_t length = LoadBE32(base + buffers), buffer = LoadBE32(base + buffers + 4);
  const uint32_t event = overlapped ? LoadBE32(base + overlapped + 16) : 0;
  uint32_t error = 0;
  const int n = ReceiveDatagram(base, socket, native, buffer, length, from, fromlen, error);
  if (n >= 0) {
    if (received) {
      StoreBE32(base + received, uint32_t(n));
    }
    if (flags) {
      StoreBE32(base + flags, 0);
    }
    if (overlapped) {
      StoreBE32(base + overlapped + 4, uint32_t(n));
    }
    SignalEvent(event);
    ctx.r3.u64 = 0;
    return;
  }
  if (error != kWsaEWouldBlock || !overlapped) {
    t_forced_error = error;
    ctx.r3.u64 = 0xFFFFFFFF;
    return;
  }
  {
    std::lock_guard lock(g_dgram_mutex);
    g_pending_recvs[overlapped] = {socket, native, buffer, length, from,
                                   fromlen, event, false, SocketPort(socket, native)};
  }
  StartRecvWatcher();
  g_dgram_cv.notify_one();
  t_forced_error = kWsaIoPending;
  ctx.r3.u64 = 0xFFFFFFFF;
}

// (caller, socket, OVERLAPPED*, DWORD* transferred, wait, DWORD* flags)
extern "C" REX_FUNC(__imp__NetDll_WSAGetOverlappedResult) {
  NHL3ON3_ORIGINAL(__imp__NetDll_WSAGetOverlappedResult);
  const uint32_t overlapped = ctx.r5.u32, transferred = ctx.r6.u32, flags = ctx.r8.u32;
  PendingRecv op;
  {
    std::lock_guard lock(g_dgram_mutex);
    auto it = g_pending_recvs.find(overlapped);
    if (it == g_pending_recvs.end()) {
      return original(ctx, base);
    }
    op = it->second;
  }
  uint32_t error = 0;
  const int n =
      ReceiveDatagram(base, op.socket, op.native, op.buffer, op.length, op.from, op.fromlen, error);
  {
    std::lock_guard lock(g_dgram_mutex);
    auto it = g_pending_recvs.find(overlapped);
    if (it != g_pending_recvs.end()) {
      if (n < 0 && error == kWsaEWouldBlock) {
        it->second.signaled = false;
      } else {
        g_pending_recvs.erase(it);
      }
    }
  }
  if (n >= 0) {
    if (transferred) {
      StoreBE32(base + transferred, uint32_t(n));
    }
    if (flags) {
      StoreBE32(base + flags, 0);
    }
    StoreBE32(base + overlapped + 4, uint32_t(n));
    ctx.r3.u64 = 1;
    return;
  }
  g_dgram_cv.notify_one();
  t_forced_error = error == kWsaEWouldBlock ? kWsaIoIncomplete : error;
  ctx.r3.u64 = 0;
}

// (count, WSAEVENT* events, wait_all, timeout_ms, alertable)
// The runtime passes wait_all straight through as the NT wait type, where 0
// is WaitAll, so a wait for any one event (DirtySDK's receive thread) sleeps
// until every event fires. It also passes finite timeouts as absolute times.
extern "C" REX_FUNC(__imp__NetDll_WSAWaitForMultipleEvents) {
  constexpr uint32_t kWaitAll = 0, kWaitAny = 1, kUserMode = 1;
  constexpr uint32_t kStatusAlerted = 0x101;
  constexpr uint32_t kWsaWaitFailed = 0xFFFFFFFF, kErrorInvalidParameter = 87;
  const uint32_t count = ctx.r3.u32, events = ctx.r4.u32, wait_all = ctx.r5.u32,
                 timeout_ms = ctx.r6.u32, alertable = ctx.r7.u32;
  if (count == 0 || count > 64 || !events) {
    t_forced_error = kErrorInvalidParameter;
    ctx.r3.u64 = kWsaWaitFailed;
    return;
  }
  uint64_t timeout = uint64_t(-int64_t(timeout_ms) * 10000);
  uint32_t status;
  do {
    status = rex::kernel::xboxkrnl::xeNtWaitForMultipleObjectsEx(
        count, reinterpret_cast<rex::be<uint32_t>*>(base + events),
        wait_all ? kWaitAll : kWaitAny, kUserMode, alertable,
        timeout_ms == 0xFFFFFFFF ? nullptr : &timeout);
  } while (status == kStatusAlerted);
  if (status >= 0x40000000) {
    t_forced_error = kErrorInvalidParameter;
    ctx.r3.u64 = kWsaWaitFailed;
    return;
  }
  ctx.r3.u64 = status;
}

extern "C" REX_FUNC(__imp__NetDll_closesocket) {
  NHL3ON3_ORIGINAL(__imp__NetDll_closesocket);
  const uint32_t socket = ctx.r4.u32;
  {
    std::lock_guard lock(g_dgram_mutex);
    g_dgram_sockets.erase(socket);
    for (auto it = g_pending_recvs.begin(); it != g_pending_recvs.end();) {
      it = it->second.socket == socket ? g_pending_recvs.erase(it) : std::next(it);
    }
  }
  SetVirtualPort(socket, 0);
  original(ctx, base);
}

NHL3ON3_SOCKET_TRACE(getsockopt)
NHL3ON3_SOCKET_TRACE(setsockopt)

#ifdef _WIN32
NHL3ON3_SOCKET_TRACE(select)
NHL3ON3_SOCKET_TRACE(ioctlsocket)
#else
// Winsock fd_sets are a count plus a handle array and ignore nfds; passing
// them through to POSIX select checks the wrong descriptors (nfds is 1), so a
// connecting socket never reports writable. Poll the native handles instead.
extern "C" REX_FUNC(__imp__NetDll_select) {
  constexpr uint32_t kMaxSet = 64;
  const uint32_t sets[3] = {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};
  const uint32_t timeout = ctx.r8.u32;
  std::vector<pollfd> fds;
  std::vector<std::pair<uint32_t, uint32_t>> entries;  // (set index, guest socket)
  for (uint32_t s = 0; s < 3; ++s) {
    if (!sets[s]) continue;
    const uint32_t count = std::min(LoadBE32(base + sets[s]), kMaxSet);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t socket = LoadBE32(base + sets[s] + 4 + i * 4);
      NativeSocket native;
      if (!NativeHandle(socket, native)) {
        t_forced_error = 10038;  // WSAENOTSOCK
        ctx.r3.u64 = 0xFFFFFFFF;
        return;
      }
      const short events = s == 0 ? POLLIN : s == 1 ? POLLOUT : POLLPRI;
      fds.push_back({native, events, 0});
      entries.emplace_back(s, socket);
    }
  }
  int wait_ms = -1;
  if (timeout) {
    const uint64_t ms = uint64_t(LoadBE32(base + timeout)) * 1000 +
                        (LoadBE32(base + timeout + 4) + 999) / 1000;
    wait_ms = int(std::min<uint64_t>(ms, INT32_MAX));
  }
  int ready = ::poll(fds.data(), fds.size(), wait_ms);
  if (ready < 0) {
    t_forced_error = errno == EINTR ? 10004 : 10022;  // WSAEINTR / WSAEINVAL
    ctx.r3.u64 = 0xFFFFFFFF;
    return;
  }
  // A failed connect surfaces as an error: Winsock reports it in exceptfds,
  // but also flag it writable when no exceptfds was given so the caller
  // still wakes up and reads SO_ERROR.
  uint32_t counts[3] = {0, 0, 0};
  uint32_t total = 0;
  for (size_t i = 0; i < fds.size(); ++i) {
    const auto [s, socket] = entries[i];
    const short got = fds[i].revents;
    const bool failed = got & (POLLERR | POLLHUP);
    bool hit = got & fds[i].events;
    if (s == 0 && (got & POLLHUP)) hit = true;
    if (s == 1 && failed && !sets[2]) hit = true;
    if (s == 2 && failed) hit = true;
    if (!hit) continue;
    StoreBE32(base + sets[s] + 4 + counts[s] * 4, socket);
    ++counts[s];
    ++total;
  }
  for (uint32_t s = 0; s < 3; ++s) {
    if (sets[s]) StoreBE32(base + sets[s], counts[s]);
  }
  ctx.r3.u64 = total;
}

// FIONBIO's Winsock code means nothing to the POSIX ioctl, so non-blocking
// mode never took effect and recv could stall the game thread.
extern "C" REX_FUNC(__imp__NetDll_ioctlsocket) {
  NHL3ON3_ORIGINAL(__imp__NetDll_ioctlsocket);
  constexpr uint32_t kFionbio = 0x8004667E;
  constexpr uint32_t kFionread = 0x4004667F;
  const uint32_t socket = ctx.r4.u32, cmd = ctx.r5.u32, argp = ctx.r6.u32;
  NativeSocket native;
  if ((cmd != kFionbio && cmd != kFionread) || !argp || !NativeHandle(socket, native)) {
    original(ctx, base);
    return;
  }
  if (cmd == kFionbio) {
    const int flags = ::fcntl(native, F_GETFL, 0);
    const bool on = LoadBE32(base + argp) != 0;
    if (flags < 0 || ::fcntl(native, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) < 0) {
      t_forced_error = 10022;
      ctx.r3.u64 = 0xFFFFFFFF;
      return;
    }
  } else {
    int pending = 0;
    if (::ioctl(native, FIONREAD, &pending) < 0) {
      t_forced_error = 10022;
      ctx.r3.u64 = 0xFFFFFFFF;
      return;
    }
    StoreBE32(base + argp, uint32_t(std::max(pending, 0)));
  }
  ctx.r3.u64 = 0;
}
#endif

extern "C" REX_FUNC(__imp__NetDll_shutdown) {
  NHL3ON3_ORIGINAL(__imp__NetDll_shutdown);
  if (Online()) {
    REXLOG_INFO("[net] shutdown({:X}, {}) backtrace: {}", ctx.r4.u32, ctx.r5.u32,
                Nhl3on3GuestBacktrace(ctx, base));
  }
  original(ctx, base);
}
NHL3ON3_SOCKET_TRACE(WSARecv)

// The SDK reports a pending non-blocking connect/recv as 31; DirtySDK only
// treats WSAEWOULDBLOCK (10035) and WSA_IO_PENDING (997) as "try again" and
// closes the socket on anything else.
extern "C" REX_FUNC(__imp__NetDll_WSAGetLastError) {
  NHL3ON3_ORIGINAL(__imp__NetDll_WSAGetLastError);
  constexpr uint32_t kSdkWouldBlock = 31;
  original(ctx, base);
  if (t_forced_error) {
    ctx.r3.u64 = t_forced_error;
    t_forced_error = 0;
  }
  if (ctx.r3.u32 == kSdkWouldBlock) {
    ctx.r3.u64 = kWsaEWouldBlock;
  }
  if (ctx.r3.u32 == 0 && t_socket_error != 0) {
    ctx.r3.u64 = t_socket_error;
  }
  const uint32_t error = ctx.r3.u32;
  const bool routine = error == 0 || error == kWsaEWouldBlock || error == 996 || error == 997;
  static std::atomic<uint32_t> last{0};
  if (Online() && !routine && last.exchange(error) != error) {
    REXLOG_INFO("[net] WSAGetLastError() = {}", error);
  }
}

extern "C" REX_FUNC(__imp__NetDll_socket) {
  NHL3ON3_TRACE("socket");
  NHL3ON3_ORIGINAL(__imp__NetDll_socket);
  const uint32_t af = ctx.r4.u32, type = ctx.r5.u32, proto = ctx.r6.u32;
  original(ctx, base);
  if (!Online()) {
    return;
  }
  const uint32_t socket = ctx.r3.u32;
  REXLOG_INFO("[net] socket(af {}, type {}, proto {}) = {:X}", af, type, proto, socket);
  NativeSocket native;
  if (type != 2 || socket == 0xFFFFFFFF || !NativeHandle(socket, native)) {
    return;
  }
  {
    std::lock_guard lock(g_dgram_mutex);
    g_dgram_sockets.insert(socket);
  }
#ifdef _WIN32
  // Otherwise an ICMP port-unreachable fails the next recvfrom.
  constexpr DWORD kSioUdpConnReset = _WSAIOW(IOC_VENDOR, 12);
  BOOL report = FALSE;
  DWORD returned = 0;
  WSAIoctl(native, kSioUdpConnReset, &report, sizeof(report), nullptr, 0, &returned, nullptr,
           nullptr);
#endif
}

//----------------------------------------------------------------------------
// Diagnostics for the LIVE flow: other XAM/XNet calls the game makes while
// connecting, and the text of any error dialog it raises.
//----------------------------------------------------------------------------

namespace {

std::string GuestWideString(const uint8_t* base, uint32_t address) {
  std::string out;
  if (!address) {
    return out;
  }
  for (const uint8_t* p = base + address; out.size() < 512; p += 2) {
    char16_t c = char16_t((p[0] << 8) | p[1]);
    if (!c) {
      break;
    }
    out.push_back(c < 0x80 ? char(c) : '?');
  }
  return out;
}

}  // namespace

namespace {

std::string FourCC(uint32_t v) {
  std::string s;
  for (int shift = 24; shift >= 0; shift -= 8) {
    char c = char(v >> shift);
    s.push_back(c >= 0x20 && c < 0x7F ? c : '.');
  }
  return s;
}

}  // namespace

extern "C" void __imp__sub_827623F0(PPCContext& __restrict ctx, uint8_t* base);

// DirtySDK SockaddrCompare(a, b): CommUDP drops packets whose source does not
// match the connection's peer address. Logs the first mismatches.
extern "C" REX_FUNC(sub_827623F0) {
  const uint32_t a = ctx.r3.u32, b = ctx.r4.u32;
  __imp__sub_827623F0(ctx, base);
  if (!Online() || ctx.r3.u32 == 0) {
    return;
  }
  static std::atomic<uint32_t> logged{0};
  if (logged++ >= 24) {
    return;
  }
  auto hex = [&](uint32_t address) {
    std::string out;
    for (int i = 0; i < 16; ++i) {
      out += fmt::format("{:02X}", base[address + i]);
    }
    return out;
  };
  REXLOG_INFO("[net] SockaddrCompare mismatch {} vs {} (lr {:08X})", hex(a), hex(b),
              uint32_t(ctx.lr));
}

extern "C" void __imp__sub_82753958(PPCContext& __restrict ctx, uint8_t* base);

// DirtySDK send-address remap(out, in) -> sockaddr to use. It turns the
// lobby's 127.0.0.1 into the online server's address, but also rewrites a
// peer's address to the server's (127.0.0.1 with a local server), which sends
// peer traffic to the server instead of the peer, so peers keep their own
// address (real, virtual or another instance's loopback).
extern "C" REX_FUNC(sub_82753958) {
  const uint32_t in = ctx.r4.u32;
  const uint32_t requested = in ? LoadBE32(base + in + 4) : 0;
  __imp__sub_82753958(ctx, base);
  const uint32_t out = ctx.r3.u32;
  if (!Online() || !out || out == in || !requested) {
    return;
  }
  const uint32_t server = State().server_ip, local = State().local_ip;
  const uint32_t remapped = LoadBE32(base + out + 4);
  const bool peer = IsLoopback(requested)
                        ? IsLoopback(local) && requested != kLoopbackIp
                        : requested != server && requested != local;
  if (peer && requested != remapped && (remapped == server || remapped == kLoopbackIp)) {
    StoreBE32(base + out + 4, requested);
  }
}

extern "C" void __imp__sub_8275E0A8(PPCContext& __restrict ctx, uint8_t* base);

// DirtySDK NetConnStatus(selector, data, buffer, size): logs each selector's
// result whenever it changes.
extern "C" REX_FUNC(sub_8275E0A8) {
  const uint32_t selector = ctx.r3.u32, data = ctx.r4.u32;
  __imp__sub_8275E0A8(ctx, base);
  if (!Online()) {
    return;
  }
  static std::mutex mutex;
  static std::unordered_map<uint64_t, uint32_t> last;
  const uint64_t key = (uint64_t(selector) << 32) | data;
  std::lock_guard lock(mutex);
  auto [it, inserted] = last.try_emplace(key, ctx.r3.u32);
  if (inserted || it->second != ctx.r3.u32) {
    it->second = ctx.r3.u32;
    REXLOG_INFO("[net] NetConnStatus('{}', {}) = {:08X} '{}'", FourCC(selector), data,
                ctx.r3.u32, FourCC(ctx.r3.u32));
  }
}

namespace {

std::string GuestText(const uint8_t* base, uint32_t address, size_t max) {
  std::string out;
  for (size_t i = 0; i < max && base[address + i]; ++i) {
    const char c = char(base[address + i]);
    out.push_back(c >= 0x20 && c < 0x7F ? c : '.');
  }
  return out;
}

// Logs `text` for `key` only when it differs from the last text logged for it.
void LogOnChange(const char* what, uint64_t key, const std::string& text) {
  static std::mutex mutex;
  static std::unordered_map<std::string, std::string> last;
  std::lock_guard lock(mutex);
  auto& seen = last[std::string(what) + ":" + std::to_string(key)];
  if (seen != text) {
    seen = text;
    REXLOG_INFO("[mm] {} {:08X}: {}", what, uint32_t(key), text);
  }
}

constexpr uint32_t kIdle = 0x69646C65;  // 'idle'

}  // namespace

extern "C" void __imp__sub_82BC0410(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82761188(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82789C60(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82788BE0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82788FD8(PPCContext& __restrict ctx, uint8_t* base);

// LobbyMatchupGameModule update: search state and the flags that gate
// EVENT_MATCHUP_SUCCESS (8815 own G changed, 8816 game joined).
extern "C" REX_FUNC(sub_82BC0410) {
  const uint32_t module = ctx.r3.u32;
  __imp__sub_82BC0410(ctx, base);
  if (!Online()) {
    return;
  }
  const uint8_t* m = base + module;
  LogOnChange("matchup", module,
              fmt::format("state {} saved G {} sub {} left {} gchg {} joined {}",
                          LoadBE32(m + 8796), int32_t(LoadBE32(m + 8808)), m[8813], m[8814],
                          m[8815], m[8816]));
}

std::string SessionState(const uint8_t* base, uint32_t session) {
  if (!session) {
    return "no session";
  }
  const uint8_t* s = base + session;
  return fmt::format("session {:08X} state {} overlapped {:08X} flags {:X} host {} done {}",
                     session, LoadBE32(s + 112), LoadBE32(s + 100), LoadBE32(s + 164), s[1131],
                     s[1128]);
}

// ConnApi status; success also needs its 'idle' to be non-zero.
extern "C" REX_FUNC(sub_82761188) {
  const uint32_t link = ctx.r3.u32, selector = ctx.r4.u32;
  __imp__sub_82761188(ctx, base);
  if (Online() && selector == kIdle) {
    LogOnChange("link idle", link,
                fmt::format("{} ({})", int32_t(ctx.r3.u32),
                            SessionState(base, LoadBE32(base + link + 76))));
  }
}

extern "C" void __imp__sub_82768468(PPCContext& __restrict ctx, uint8_t* base);

// XSession wrapper control(session, selector, value, ..., data). 'slot' sets the
// public slot count that XSessionCreate/XSessionModify receive; the game asks
// for 2, which leaves no room for a third console.
extern "C" REX_FUNC(sub_82768468) {
  constexpr uint32_t kSlot = 0x736C6F74;  // 'slot'
  if (ctx.r4.u32 == kSlot && REXCVAR_GET(team_play) >= 3 &&
      ctx.r5.u32 < uint32_t(REXCVAR_GET(team_play))) {
    ctx.r5.u64 = uint32_t(REXCVAR_GET(team_play));
  }
  const uint32_t session = ctx.r3.u32, selector = ctx.r4.u32, value = ctx.r5.u32,
                 data = ctx.r7.u32;
  __imp__sub_82768468(ctx, base);
  if (Online() && selector != 0x74696D65) {  // 'time' is polled every frame
    REXLOG_INFO("[mm] session control '{}' value {} data {:08X} -> {}; {}", FourCC(selector),
                value, data, int32_t(ctx.r3.u32), SessionState(base, session));
  }
}

extern "C" void __imp__sub_82761E08(PPCContext& __restrict ctx, uint8_t* base);

// ConnApiCreate(game port, max clients, callback, user data, ...). The match's
// ConnApi takes 2 clients (itself and one opponent); later lobby members are
// dropped from its list and never connected.
extern "C" REX_FUNC(sub_82761E08) {
  const int32_t team = REXCVAR_GET(team_play);
  if (Online()) {
    REXLOG_INFO("[mm] ConnApiCreate(port {}, max clients {})", ctx.r3.u32, ctx.r4.u32);
  }
  // The match ConnApi is the one sized for 2 clients; voice has its own.
  if (ctx.r4.u32 == 2 && ctx.r3.u32 && ctx.r3.u32 < 0x10000) {
    g_game_port.store(ctx.r3.u32);
  }
  if (team >= 3 && ctx.r4.u32 == 2) {
    REXLOG_INFO("[mm] team play: ConnApi port {} max clients 2 -> {}", ctx.r3.u32, team);
    ctx.r4.u64 = uint32_t(team);
  }
  __imp__sub_82761E08(ctx, base);
}

// The online module's config (from the manager at 0x83216B34, virtual 192)
// holds the session's maximum user count at +184. It is 2, and sizes the
// game session's user pool, game setup and peer connection tables, and the
// match ConnApi; a third console overruns them. Every reader raises it first.
void ApplyTeamPlayConfig(PPCContext& caller, uint8_t* base) {
  const int32_t team = REXCVAR_GET(team_play);
  const uint32_t manager = LoadBE32(base + 0x83216B34);
  if (team < 3 || !manager) {
    return;
  }
  PPCFunc* get_config =
      rex::runtime::ResolveIndirectFunction(LoadBE32(base + LoadBE32(base + manager) + 192));
  if (!get_config) {
    return;
  }
  PPCContext ctx = caller;
  ctx.r3.u64 = manager;
  get_config(ctx, base);
  const uint32_t config = ctx.r3.u32;
  if (config && LoadBE32(base + config + 184) < uint32_t(team)) {
    REXLOG_INFO("[mm] team play: session max users {} -> {}", LoadBE32(base + config + 184), team);
    StoreBE32(base + config + 184, uint32_t(team));
  }
}

#define TEAM_PLAY_CONFIG_READER(name)                                  \
  extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base); \
  extern "C" REX_FUNC(name) {                                          \
    ApplyTeamPlayConfig(ctx, base);                                    \
    __imp__##name(ctx, base);                                          \
  }

TEAM_PLAY_CONFIG_READER(sub_82AE5628)
TEAM_PLAY_CONFIG_READER(sub_82AE57F8)
TEAM_PLAY_CONFIG_READER(sub_82AE59B8)
TEAM_PLAY_CONFIG_READER(sub_82BC9840)  // peer connection manager
TEAM_PLAY_CONFIG_READER(sub_82BDC148)  // game session (user pool)
TEAM_PLAY_CONFIG_READER(sub_82BDC8D8)  // GameSetupConcrete constructor

extern "C" void __imp__sub_82767968(PPCContext& __restrict ctx, uint8_t* base);

// XSession wrapper 'sess' request: (session, is host, session info text).
// It refuses (-1) while the wrapper still holds a session handle (+100). A
// session the game gave up on (state 17, e.g. after a failed host migration
// when the host quit) or reset to idle (0) is never deleted, so its handle
// stays and every later game this console hosts or joins fails to start.
// The kernel keeps no session state, so a stale handle is just dropped.
extern "C" REX_FUNC(sub_82767968) {
  const uint32_t session = ctx.r3.u32, host = ctx.r4.u32, info = ctx.r5.u32;
  if (Online() && session && LoadBE32(base + session + 100)) {
    const uint32_t state = LoadBE32(base + session + 112);
    if (state == 0 || state == 17) {
      REXLOG_INFO("[mm] sess request: dropping stale session handle {:08X} (state {})",
                  LoadBE32(base + session + 100), state);
      StoreBE32(base + session + 100, 0);
    }
  }
  const std::string before = Online() ? SessionState(base, session) : "";
  __imp__sub_82767968(ctx, base);
  if (Online()) {
    REXLOG_INFO("[mm] sess request host {} info '{}' -> {}; before: {}; after: {}", host,
                info ? GuestText(base, info, 96) : "<null>", int32_t(ctx.r3.u32), before,
                SessionState(base, session));
    const uint8_t* s = base + session;
    std::string users;
    const uint32_t netconn = LoadBE32(base + 0x831BB5F4);
    for (int i = 0; netconn && i < 4; ++i) {
      users += fmt::format(" [{} '{}' {:08X}{:08X}]", i, GuestText(base, netconn + 72 + 32 * i, 16),
                           LoadBE32(base + netconn + 88 + 32 * i),
                           LoadBE32(base + netconn + 92 + 32 * i));
    }
    REXLOG_INFO("[mm]   host xuid {:08X}{:08X} self xuid {:08X}{:08X}; local users{}",
                LoadBE32(s + 120), LoadBE32(s + 124), LoadBE32(s + 136), LoadBE32(s + 140),
                users);
  }
}

// Lobby game manager status; 'idle' is its state (27768) == 0.
extern "C" REX_FUNC(sub_82789C60) {
  const uint32_t gm = ctx.r3.u32, selector = ctx.r4.u32;
  __imp__sub_82789C60(ctx, base);
  if (Online() && selector == kIdle) {
    LogOnChange("gm idle", gm, std::to_string(int32_t(ctx.r3.u32)));
  }
}

// Game manager: first sight of a game; hosts create the session, others join
// the SESS the host published.
extern "C" REX_FUNC(sub_82788BE0) {
  const uint32_t gm = ctx.r3.u32, info = ctx.r4.u32;
  __imp__sub_82788BE0(ctx, base);
  if (Online()) {
    REXLOG_INFO("[mm] gm {:08X} start: self '{}' host '{}' sess '{}' count {} -> {} state {}",
                gm, GuestText(base, gm + 27752, 32), GuestText(base, info + 40, 32),
                GuestText(base, info + 76, 64), LoadBE32(base + info + 524), ctx.r3.u32,
                LoadBE32(base + gm + 27768));
  }
}

extern "C" REX_FUNC(sub_82788FD8) {
  const uint32_t gm = ctx.r3.u32;
  __imp__sub_82788FD8(ctx, base);
  if (Online()) {
    LogOnChange("gm", gm,
                fmt::format("state {} count {}", LoadBE32(base + gm + 27768),
                            LoadBE32(base + gm + 544)));
  }
}

// (user, title, text, button_count, buttons, active, flags, result, overlapped)
extern "C" REX_FUNC(__imp__XamShowMessageBoxUIEx) {
  NHL3ON3_ORIGINAL(__imp__XamShowMessageBoxUIEx);
  if (Online()) {
    REXLOG_INFO("[net] XamShowMessageBoxUIEx title='{}' text='{}'",
                GuestWideString(base, ctx.r4.u32), GuestWideString(base, ctx.r5.u32));
  }
  original(ctx, base);
}

// (app id, message id, buffer, buffer size, overlapped)
extern "C" REX_FUNC(__imp__XMsgStartIORequestEx) {
  NHL3ON3_ORIGINAL(__imp__XMsgStartIORequestEx);
  const uint32_t app = ctx.r3.u32, message = ctx.r4.u32;
  original(ctx, base);
  if (Online()) {
    REXLOG_INFO("[net] XMsgStartIORequestEx(app {:X}, message {:X}) = {:X}", app, message,
                ctx.r3.u32);
  }
}

// (listener, match id, DWORD* id out, ULONG_PTR* param out); logs deliveries only.
extern "C" REX_FUNC(__imp__XNotifyGetNext) {
  NHL3ON3_ORIGINAL(__imp__XNotifyGetNext);
  const uint32_t id_ptr = ctx.r5.u32, param_ptr = ctx.r6.u32;
  original(ctx, base);
  if (Online() && ctx.r3.u32 && id_ptr) {
    REXLOG_INFO("[net] XNotifyGetNext delivered id {:08X} param {:08X}",
                LoadBE32(base + id_ptr), param_ptr ? LoadBE32(base + param_ptr) : 0);
  }
}
NHL3ON3_TRACE_PASSTHROUGH(XamShowSigninUI)
NHL3ON3_TRACE_PASSTHROUGH(XamSessionCreateHandle)
NHL3ON3_TRACE_PASSTHROUGH(XamSessionRefObjByHandle)
NHL3ON3_TRACE_PASSTHROUGH(XamUserAreUsersFriends)
// (title, user_index, xuid_count, xuids, ...). The SDK only has user 0's
// profile, so extra local players read its settings.
extern "C" REX_FUNC(__imp__XamUserReadProfileSettings) {
  NHL3ON3_ORIGINAL(__imp__XamUserReadProfileSettings);
  NHL3ON3_TRACE("XamUserReadProfileSettings");
  if (ctx.r4.u32 != 0 && ctx.r5.u32 == 0 && IsOnlineUser(ctx.r4.u32)) {
    ctx.r4.u64 = 0;
  }
  original(ctx, base);
}
NHL3ON3_TRACE_PASSTHROUGH(XamUserCreateStatsEnumerator)

// Save data calls, logged every time, online or not.
#define NHL3ON3_SAVE_TRACE(name)                                                         \
  extern "C" REX_FUNC(__imp__##name) {                                                   \
    NHL3ON3_ORIGINAL(__imp__##name);                                                     \
    const uint32_t a[4] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};              \
    original(ctx, base);                                                                 \
    REXLOG_INFO("[save] " #name "({:X}, {:X}, {:X}, {:X}) = {:X}", a[0], a[1], a[2], a[3], \
                ctx.r3.u32);                                                             \
  }
NHL3ON3_SAVE_TRACE(XamShowDeviceSelectorUI)
NHL3ON3_SAVE_TRACE(XamContentGetDeviceState)
NHL3ON3_SAVE_TRACE(XamContentGetDeviceData)
NHL3ON3_SAVE_TRACE(XamContentCreateEnumerator)
NHL3ON3_SAVE_TRACE(XamContentCreateEx)
NHL3ON3_SAVE_TRACE(XamContentClose)
NHL3ON3_SAVE_TRACE(XamContentDelete)
NHL3ON3_SAVE_TRACE(XMsgStartIORequest)
NHL3ON3_TRACE_PASSTHROUGH(XNetLogonGetTitleID)
NHL3ON3_TRACE_PASSTHROUGH(XNetLogonGetMachineID)
NHL3ON3_TRACE_PASSTHROUGH(NetDll_XNetStartup)
NHL3ON3_TRACE_PASSTHROUGH(NetDll_WSAStartup)
NHL3ON3_TRACE_PASSTHROUGH(NetDll_XNetQosListen)

// (caller, cxna, XNADDR* apxna[], XNKID* apxnkid[], XNKEY* apxnkey[], cina,
//  IN_ADDR aina[], DWORD adwServiceId[], cProbes, dwBitsPerSec, dwFlags,
//  WSAEVENT, XNQOS** out): QoS probes of other consoles (cxna) and servers
// (cina). The SDK's is a stub that never completes, and the lobby waits on
// it, so every target is reported as reached with a good connection.
extern "C" REX_FUNC(__imp__NetDll_XNetQosLookup) {
  NHL3ON3_TRACE("XNetQosLookup");
  NHL3ON3_ORIGINAL(__imp__NetDll_XNetQosLookup);
  if (!Online()) {
    return original(ctx, base);
  }
  const auto stack_arg = [&](int arg) {
    return LoadBE32(base + ctx.r1.u32 + 0x54 + (arg - 8) * 8);
  };
  const uint32_t targets = ctx.r4.u32 + ctx.r8.u32;
  const uint32_t event = stack_arg(11), out = stack_arg(12);
  REXLOG_INFO("[net] XNetQosLookup({} consoles, {} servers)", ctx.r4.u32, ctx.r8.u32);
  if (out) {
    const uint32_t entries = std::max<uint32_t>(targets, 1);
    const uint32_t size = uint32_t(offsetof(XnQos, info) + sizeof(XnQosInfo) * entries);
    const uint32_t guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
    std::memset(base + guest, 0, size);
    auto* qos = reinterpret_cast<XnQos*>(base + guest);
    StoreBE32(qos->count, targets);
    for (uint32_t i = 0; i < targets; ++i) {
      XnQosInfo& info = qos->info[i];
      info.flags = kQosInfoContacted;
      StoreBE16(info.probes_xmit, 4);
      StoreBE16(info.probes_recv, 4);
      StoreBE16(info.rtt_min, 20);
      StoreBE16(info.rtt_med, 25);
      StoreBE32(info.up_bps, 10'000'000);
      StoreBE32(info.down_bps, 50'000'000);
    }
    StoreBE32(base + out, guest);
  }
  SignalEvent(event);
  ctx.r3.u64 = 0;
}

uint32_t Nhl3on3LocalIp() {
  return State().local_ip;
}

std::vector<uint32_t> Nhl3on3GamePeers() {
  std::lock_guard lock(g_game_peers_mutex);
  return g_game_peers;
}

bool Nhl3on3RankedSearch() {
  return g_ranked_search.load();
}

int Nhl3on3GameConsoles() {
  return g_game_consoles.load();
}

bool Nhl3on3TunnelActive() {
  State();
  return TunnelActive();
}

void Nhl3on3TunnelSendRelay(const uint8_t* data, size_t size) {
  auto packet = TunnelPacket(kTunnelRelay, size);
  packet.insert(packet.end(), data, data + size);
  TunnelSend(packet);
}

bool Nhl3on3TunnelReceiveRelay(std::vector<uint8_t>& packet,
                               std::chrono::steady_clock::time_point& arrived) {
  std::lock_guard lock(g_tunnel.mutex);
  if (g_tunnel.relay_inbox.empty()) {
    return false;
  }
  arrived = g_tunnel.relay_inbox.front().first;
  packet = std::move(g_tunnel.relay_inbox.front().second);
  g_tunnel.relay_inbox.pop_front();
  return true;
}
