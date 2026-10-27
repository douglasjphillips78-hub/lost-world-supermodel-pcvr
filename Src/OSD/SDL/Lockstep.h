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
 **   'C' check  : frame number + a hash of the machine          (both ways, once a second)
 **/

#ifndef INCLUDED_LOCKSTEP_H
#define INCLUDED_LOCKSTEP_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Inputs/Inputs.h"

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <errno.h>
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
  int delay = 4;                       // game frames of input delay (hides the network latency)
  uint32_t config = 0;                 // must match on both machines
  std::string gameName;

  std::atomic<bool> peerHello{false}, helloBad{false}, stateReady{false}, peerReady{false}, dead{false}, connected{false};
  std::atomic<bool> desync{false};
  std::vector<uint8_t> stateBuf;       // the joiner's copy of the host's snapshot (valid once stateReady)

  bool Start(std::string &err)
  {
#ifdef LOCKNET_AVAILABLE
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

  std::string Status() const { return status; }

private:
  std::atomic<bool> stop{false};
  std::thread th;
  std::mutex mu, sendMu;
  std::map<uint32_t, GameState> remote;
  std::map<uint32_t, uint64_t> myHash, theirHash;
  std::string status;
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

  bool Connect()
  {
    // Joiner: try again every second until the host is there
    while (!stop)
    {
      int s = socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in sa;
      memset(&sa, 0, sizeof(sa));
      sa.sin_family = AF_INET;
      sa.sin_port = htons((uint16_t)port);
      if (inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1)
      {
        status = "LockstepHost is not an IPv4 address: " + addr;
        close(s);
        return false;
      }
      // connect with a timeout (non-blocking + poll)
      int fl = fcntl(s, F_GETFL, 0);
      fcntl(s, F_SETFL, fl | O_NONBLOCK);
      int r = connect(s, (sockaddr *)&sa, sizeof(sa));
      bool ok = r == 0;
      if (r < 0 && errno == EINPROGRESS)
      {
        struct pollfd pf = { s, POLLOUT, 0 };
        if (poll(&pf, 1, 1500) > 0)
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
      status = "cannot listen on the port";
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
    status = role == HOST ? "waiting for the other headset" : "looking for the host";
    if (!(role == HOST ? Accept() : Connect()))
    {
      dead = true;
      return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    connected = true;
    status = "connected";

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
    status = "connection lost";
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
        status = "the other headset runs a different version";
      else if (name != gameName)
        status = "the other headset runs a different game";
      else if (cfg != config)
        status = "the settings differ (PowerPCFrequency must match)";
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

#endif
