/**
 ** Lockstep two-headset play: the network side.
 **
 ** Two headsets each run the whole game. They start from the same snapshot (the host sends its
 ** own over the network) and then, every game frame, swap only the player inputs, so both
 ** emulators stay identical (the emulator was verified to be deterministic with the
 ** LockstepTest record/replay in Main.cpp). Everything here is plain POSIX TCP.
 **
 ** Messages: 1 byte type, 4 bytes length (little endian), payload.
 **   'H' hello  : magic, version, config word, game name        (both ways, first)
 **   'S' state  : the host's snapshot                           (host -> joiner)
 **   'R' ready  : the joiner has loaded the snapshot            (joiner -> host)
 **   'I' input  : frame number + the sender's own GameState     (both ways, every frame)
 **   'V' voice  : 20 ms of microphone sound                     (both ways)
 **   'C' check  : frame number + a hash of the machine          (both ways, once a second)
 **/

#ifndef INCLUDED_LOCKSTEP_H
#define INCLUDED_LOCKSTEP_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include "Inputs/Inputs.h"

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define LOCKNET_AVAILABLE 1
#endif

class LockNet
{
public:
  enum Role { OFF = 0, HOST = 1, JOIN = 2 };
  typedef CInputs::GameState GameState;

  int role = OFF;
  int port = 1990;
  std::string addr;
  // Over the internet: both headsets connect OUT to a relay server (relayHost:relayPort), which
  // pairs them by room code and passes their bytes on. Nobody needs to open a port.
  bool relay = false;
  std::string relayHost, relayCode;
  int relayPort = 1992;
  int connectTimeout = 0;              // seconds to keep trying to reach the host (0 = for ever)
  int delay = 4;                       // game frames of input delay (hides the network latency)
  uint32_t config = 0;                 // must match on both machines
  std::string gameName;

  std::atomic<bool> peerHello{false}, helloBad{false}, stateReady{false}, peerReady{false}, dead{false}, connected{false};
  std::atomic<bool> desync{false};
  std::function<void(const uint8_t *, uint32_t)> onVoice;   // voice packets from the other headset
  std::vector<uint8_t> stateBuf;       // the joiner's copy of the host's snapshot (valid once stateReady)

  bool Start(std::string &err)
  {
#ifdef LOCKNET_AVAILABLE
    if (th.joinable())
      th.join();
    CloseAll();
    peerHello = helloBad = stateReady = peerReady = dead = connected = desync = false;
    stateBuf.clear();
    {
      std::lock_guard<std::mutex> l(mu);
      remote.clear(); myHash.clear(); theirHash.clear();
    }
    stop = false;
    th = std::thread([this] { Run(); });
    err.clear();
    return true;
#else
    err = "not available on this platform";
    return false;
#endif
  }

  void Stop()
  {
#ifdef LOCKNET_AVAILABLE
    stop = true;
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
    if (th.joinable()) th.join();
    CloseAll();
#endif
  }

  // ----- sending (any thread) -----
  void SendInput(uint32_t frame, const GameState &st)
  {
    uint8_t buf[4 + sizeof(GameState)];
    memcpy(buf, &frame, 4);
    memcpy(buf + 4, &st, sizeof(GameState));
    SendMsg('I', buf, sizeof(buf));
  }
  void SendHash(uint32_t frame, uint64_t h)
  {
    uint8_t buf[12];
    memcpy(buf, &frame, 4);
    memcpy(buf + 4, &h, 8);
    SendMsg('C', buf, sizeof(buf));
  }
  void SendVoice(const uint8_t *d, uint32_t n) { SendMsg('V', d, n); }
  void SendReady() { SendMsg('R', nullptr, 0); }
  void SendStateAsync(std::vector<uint8_t> bytes)
  {
    std::thread([this, b = std::move(bytes)] { SendMsg('S', b.data(), (uint32_t)b.size()); }).detach();
  }

  // ----- the peer's inputs -----
  bool GetRemote(uint32_t frame, GameState &out)
  {
    std::lock_guard<std::mutex> l(mu);
    auto it = remote.find(frame);
    if (it == remote.end())
      return false;
    out = it->second;
    remote.erase(remote.begin(), it);   // old frames are no longer needed
    return true;
  }

  // Compare a hash of our machine with the peer's (whichever arrives second does the comparing).
  // Returns false on a mismatch.
  bool CheckHash(uint32_t frame, uint64_t mine)
  {
    std::lock_guard<std::mutex> l(mu);
    auto it = theirHash.find(frame);
    if (it != theirHash.end())
    {
      bool ok = it->second == mine;
      theirHash.erase(theirHash.begin(), ++it);
      return ok;
    }
    myHash[frame] = mine;
    return true;
  }

  // One short question to the relay's directory ("PUT code game port", "GET code game", "DEL ..."):
  // sends one line, returns the one-line reply. Blocks for at most a few seconds.
  static bool Ask(const std::string &host, int tcpPort, const std::string &line, std::string &reply, int timeoutMs = 5000)
  {
#ifdef LOCKNET_AVAILABLE
    reply.clear();
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(tcpPort).c_str(), &hints, &res) != 0 || !res)
      return false;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int r = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno == EINPROGRESS)
    {
      struct pollfd pf = { s, POLLOUT, 0 };
      int e = 1;
      if (poll(&pf, 1, timeoutMs) > 0)
      {
        socklen_t el = sizeof(e);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el);
      }
      r = e == 0 ? 0 : -1;
    }
    fcntl(s, F_SETFL, fl);
    bool ok = r == 0;
    if (ok)
      ok = SendAll(s, (line + "\n").data(), line.size() + 1);
    while (ok && reply.find('\n') == std::string::npos && reply.size() < 200)
    {
      struct pollfd pf = { s, POLLIN, 0 };
      if (poll(&pf, 1, timeoutMs) <= 0)
        { ok = false; break; }
      char buf[128];
      ssize_t n = recv(s, buf, sizeof(buf), 0);
      if (n <= 0)
        break;
      reply.append(buf, (size_t)n);
    }
    close(s);
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r'))
      reply.pop_back();
    return ok && !reply.empty();
#else
    (void)host; (void)tcpPort; (void)line; (void)timeoutMs;
    return false;
#endif
  }

  std::string Status() const { std::lock_guard<std::mutex> l(stMu); return status; }

private:
  std::atomic<bool> stop{false};
  std::thread th;
  std::mutex mu, sendMu;
  std::map<uint32_t, GameState> remote;
  std::map<uint32_t, uint64_t> myHash, theirHash;
  mutable std::mutex stMu;
  std::string status;
  void SetStatus(const std::string &t) { std::lock_guard<std::mutex> l(stMu); status = t; }
  int fd = -1, listenFd = -1;

#ifdef LOCKNET_AVAILABLE
  void CloseAll()
  {
    if (fd >= 0) { close(fd); fd = -1; }
    if (listenFd >= 0) { close(listenFd); listenFd = -1; }
  }

  bool SendMsg(char type, const void *data, uint32_t len)
  {
    std::lock_guard<std::mutex> l(sendMu);
    int s = fd;
    if (s < 0 || !connected)
      return false;
    uint8_t hdr[5];
    hdr[0] = (uint8_t)type;
    memcpy(hdr + 1, &len, 4);
    if (!SendAll(s, hdr, 5) || (len && !SendAll(s, data, len)))
    {
      connected = false;
      dead = true;
      return false;
    }
    return true;
  }

  static bool SendAll(int s, const void *data, size_t len)
  {
    const uint8_t *p = (const uint8_t *)data;
    while (len)
    {
      ssize_t n = send(s, p, len, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      p += n;
      len -= (size_t)n;
    }
    return true;
  }

  // Receive exactly len bytes, waking up now and then to see whether we should stop.
  bool RecvAll(void *data, size_t len)
  {
    uint8_t *p = (uint8_t *)data;
    while (len)
    {
      if (stop)
        return false;
      struct pollfd pf = { fd, POLLIN, 0 };
      int r = poll(&pf, 1, 250);
      if (r < 0 && errno == EINTR)
        continue;
      if (r < 0)
        return false;
      if (r == 0)
        continue;
      ssize_t n = recv(fd, p, len, 0);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      p += n;
      len -= (size_t)n;
    }
    return true;
  }

  bool Connect() { return ConnectTo(addr, port, connectTimeout); }

  // Connect to host:port (an address or a name), trying again every second until it works.
  bool ConnectTo(const std::string &host, int tcpPort, int timeoutSec = 0)
  {
    const auto t0 = std::chrono::steady_clock::now();
    while (!stop)
    {
      if (timeoutSec && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(timeoutSec))
      {
        SetStatus("could not reach the host - check the code");
        return false;
      }
      struct addrinfo hints, *res = nullptr;
      memset(&hints, 0, sizeof(hints));
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      if (getaddrinfo(host.c_str(), std::to_string(tcpPort).c_str(), &hints, &res) != 0 || !res)
      {
        SetStatus("cannot find " + host);
        for (int i = 0; i < 20 && !stop; i++)
          usleep(100000);
        continue;
      }
      int s = socket(AF_INET, SOCK_STREAM, 0);
      int fl = fcntl(s, F_GETFL, 0);
      fcntl(s, F_SETFL, fl | O_NONBLOCK);
      int r = connect(s, res->ai_addr, res->ai_addrlen);
      freeaddrinfo(res);
      bool ok = r == 0;
      if (r < 0 && errno == EINPROGRESS)
      {
        struct pollfd pf = { s, POLLOUT, 0 };
        if (poll(&pf, 1, 3000) > 0)
        {
          int e = 0;
          socklen_t el = sizeof(e);
          getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el);
          ok = e == 0;
        }
      }
      if (ok)
      {
        fcntl(s, F_SETFL, fl);
        fd = s;
        return true;
      }
      close(s);
      for (int i = 0; i < 10 && !stop; i++)
        usleep(100000);
    }
    return false;
  }

  // Read one text line (up to the newline) from the relay
  bool RecvLine(std::string &line)
  {
    line.clear();
    while (line.size() < 200)
    {
      char c;
      if (!RecvAll(&c, 1))
        return false;
      if (c == '\n')
        return true;
      line += c;
    }
    return false;
  }

  // Meet the other headset through the relay
  bool ConnectRelay()
  {
    SetStatus("connecting to the relay server");
    if (!ConnectTo(relayHost, relayPort))
      return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    const std::string req = std::string(role == HOST ? "HOST " : "JOIN ") + relayCode + " " + gameName + "\n";
    if (!SendAll(fd, req.data(), req.size()))
    {
      SetStatus("the relay server hung up");
      return false;
    }
    SetStatus(role == HOST ? "waiting for your friend" : "joining the room");
    std::string line;
    if (!RecvLine(line))
    {
      if (!stop) SetStatus("the relay server hung up");
      return false;
    }
    if (line != "OK")
    {
      SetStatus(line == "NO" ? "no such room - check the code" : "the relay refused: " + line);
      return false;
    }
    return true;
  }

  bool Accept()
  {
    listenFd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(listenFd, (sockaddr *)&sa, sizeof(sa)) != 0 || listen(listenFd, 1) != 0)
    {
      SetStatus("cannot listen on the port");
      return false;
    }
    while (!stop)
    {
      struct pollfd pf = { listenFd, POLLIN, 0 };
      int r = poll(&pf, 1, 300);
      if (r > 0)
      {
        int s = accept(listenFd, nullptr, nullptr);
        if (s >= 0)
        {
          fd = s;
          return true;
        }
      }
    }
    return false;
  }

  void Run()
  {
    SetStatus(role == HOST ? "waiting for the other headset" : "looking for the host");
    helloBad = false;
    if (!(relay ? ConnectRelay() : (role == HOST ? Accept() : Connect())))
    {
      dead = true;
      return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    connected = true;
    SetStatus("connected");

    // hello
    {
      std::vector<uint8_t> h;
      auto put32 = [&h](uint32_t v) { h.insert(h.end(), (uint8_t *)&v, (uint8_t *)&v + 4); };
      put32(0x4C4B5354);
      put32(1);
      put32(config);
      put32((uint32_t)gameName.size());
      h.insert(h.end(), gameName.begin(), gameName.end());
      SendMsg('H', h.data(), (uint32_t)h.size());
    }

    while (!stop)
    {
      uint8_t hdr[5];
      if (!RecvAll(hdr, 5))
        break;
      uint32_t len;
      memcpy(&len, hdr + 1, 4);
      if (len > (96u << 20))
        break;
      std::vector<uint8_t> pay(len);
      if (len && !RecvAll(pay.data(), len))
        break;
      Handle((char)hdr[0], pay);
    }
    connected = false;
    dead = true;
    SetStatus("connection lost");
  }

  void Handle(char type, const std::vector<uint8_t> &p)
  {
    switch (type)
    {
    case 'H':
    {
      uint32_t magic = 0, ver = 0, cfg = 0, n = 0;
      if (p.size() >= 16)
      {
        memcpy(&magic, &p[0], 4); memcpy(&ver, &p[4], 4); memcpy(&cfg, &p[8], 4); memcpy(&n, &p[12], 4);
      }
      std::string name = (p.size() >= 16 + n) ? std::string((const char *)&p[16], n) : std::string();
      if (magic != 0x4C4B5354 || ver != 1)
        SetStatus("the other headset runs a different version");
      else if (name != gameName)
        SetStatus("the other headset runs a different game");
      else if (cfg != config)
        SetStatus("the settings differ (PowerPCFrequency must match)");
      else
      {
        peerHello = true;
        return;
      }
      helloBad = true;
      break;
    }
    case 'S':
      stateBuf = p;
      stateReady = true;
      break;
    case 'V':
      if (onVoice)
        onVoice(p.data(), (uint32_t)p.size());
      break;
    case 'R':
      peerReady = true;
      break;
    case 'I':
      if (p.size() == 4 + sizeof(GameState))
      {
        uint32_t f;
        GameState st;
        memcpy(&f, &p[0], 4);
        memcpy(&st, &p[4], sizeof(st));
        std::lock_guard<std::mutex> l(mu);
        remote[f] = st;
      }
      break;
    case 'C':
      if (p.size() == 12)
      {
        uint32_t f;
        uint64_t h;
        memcpy(&f, &p[0], 4);
        memcpy(&h, &p[4], 8);
        std::lock_guard<std::mutex> l(mu);
        auto it = myHash.find(f);
        if (it != myHash.end())
        {
          if (it->second != h)
            desync = true;
          myHash.erase(myHash.begin(), ++it);
        }
        else
          theirHash[f] = h;
      }
      break;
    default:
      break;
    }
  }
#else
  bool SendMsg(char, const void *, uint32_t) { return false; }
#endif
};

// Finding each other on the LAN without typing an address: the host shouts "room <code>" on UDP
// port 1991 once a second (to every network's broadcast address), the joiner listens and lists
// what it hears. Nothing here carries game data.
class LockDisco
{
public:
  struct Room { std::string code, addr; int port; };
  static constexpr int kPort = 1991;

  ~LockDisco() { StopBeacon(); StopListen(); }

  void StartBeacon(const std::string &code, const std::string &game, int tcpPort)
  {
#ifdef LOCKNET_AVAILABLE
    StopBeacon();
    beaconStop = false;
    msg = "LWVR1|" + code + "|" + game + "|" + std::to_string(tcpPort);
    beaconTh = std::thread([this] { BeaconRun(); });
#endif
  }
  void StopBeacon()
  {
#ifdef LOCKNET_AVAILABLE
    beaconStop = true;
    if (beaconTh.joinable()) beaconTh.join();
#endif
  }
  void StartListen(const std::string &game)
  {
#ifdef LOCKNET_AVAILABLE
    StopListen();
    listenGame = game;
    listenStop = false;
    { std::lock_guard<std::mutex> l(mu); rooms.clear(); }
    listenTh = std::thread([this] { ListenRun(); });
#endif
  }
  void StopListen()
  {
#ifdef LOCKNET_AVAILABLE
    listenStop = true;
    if (listenTh.joinable()) listenTh.join();
#endif
  }
  // Rooms heard in the last few seconds
  std::vector<Room> Rooms()
  {
    std::vector<Room> out;
    std::lock_guard<std::mutex> l(mu);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = rooms.begin(); it != rooms.end();)
    {
      if (now - it->second.seen > std::chrono::seconds(5))
        it = rooms.erase(it);
      else
      {
        out.push_back(it->second.room);
        ++it;
      }
    }
    return out;
  }

private:
  struct Seen { Room room; std::chrono::steady_clock::time_point seen; };
  std::atomic<bool> beaconStop{true}, listenStop{true};
  std::thread beaconTh, listenTh;
  std::mutex mu;
  std::map<std::string, Seen> rooms;
  std::string msg, listenGame;

#ifdef LOCKNET_AVAILABLE
  void BeaconRun()
  {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
      return;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    while (!beaconStop)
    {
      std::vector<in_addr_t> targets;
      targets.push_back(htonl(INADDR_BROADCAST));
      struct ifaddrs *ifa = nullptr;
      if (getifaddrs(&ifa) == 0)
      {
        for (struct ifaddrs *i = ifa; i; i = i->ifa_next)
          if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && i->ifa_broadaddr && (i->ifa_flags & IFF_BROADCAST))
            targets.push_back(((sockaddr_in *)i->ifa_broadaddr)->sin_addr.s_addr);
        freeifaddrs(ifa);
      }
      for (in_addr_t t : targets)
      {
        sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(kPort);
        sa.sin_addr.s_addr = t;
        sendto(s, msg.data(), msg.size(), 0, (sockaddr *)&sa, sizeof(sa));
      }
      for (int i = 0; i < 10 && !beaconStop; i++)
        usleep(100000);
    }
    close(s);
  }

  void ListenRun()
  {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
      return;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(kPort);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr *)&sa, sizeof(sa)) != 0)
    {
      close(s);
      return;
    }
    while (!listenStop)
    {
      struct pollfd pf = { s, POLLIN, 0 };
      if (poll(&pf, 1, 250) <= 0)
        continue;
      char buf[256];
      sockaddr_in from;
      socklen_t fl = sizeof(from);
      ssize_t n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fl);
      if (n <= 0)
        continue;
      buf[n] = 0;
      std::string m(buf);
      // LWVR1|code|game|port
      std::vector<std::string> f;
      size_t a = 0;
      for (size_t b; (b = m.find('|', a)) != std::string::npos; a = b + 1)
        f.push_back(m.substr(a, b - a));
      f.push_back(m.substr(a));
      if (f.size() != 4 || f[0] != "LWVR1" || f[2] != listenGame)
        continue;
      char ip[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
      Seen sn;
      sn.room = { f[1], ip, atoi(f[3].c_str()) };
      sn.seen = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> l(mu);
      rooms[f[1] + "@" + ip] = sn;
    }
    close(s);
  }
#endif
};

#endif
