/**
 ** Opens a port on the home router by itself (UPnP "Internet Gateway Device"), so a friend can
 ** connect straight to this headset without anyone touching router settings.
 **
 **   Upnp u;  u.Start(1990);  ...  u.State() == Upnp::OK  ->  u.ExternalIp()
 **
 ** Runs in its own thread (looking for the router takes a few seconds). Many routers have UPnP
 ** switched off and a few ISPs share one public address between customers; both end as FAILED
 ** with a message the caller can show or fall back from.
 **/

#ifndef INCLUDED_UPNP_H
#define INCLUDED_UPNP_H

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define UPNP_AVAILABLE 1
#endif

class Upnp
{
public:
  enum State { IDLE = 0, WORKING, OK, FAILED };

  // Testing: where the "is there a router?" shout goes
  std::string ssdpAddr = "239.255.255.250";
  int ssdpPort = 1900;

  ~Upnp() { Stop(); }

  State State_() const { return (State)state.load(); }
  std::string ExternalIp() const { std::lock_guard<std::mutex> l(mu); return extIp; }
  std::string Message() const { std::lock_guard<std::mutex> l(mu); return msg; }

  void Start(int port)
  {
#ifdef UPNP_AVAILABLE
    Stop();
    extPort = port;
    state = WORKING;
    SetMsg("looking for your router");
    th = std::thread([this] { Run(); });
#else
    state = FAILED;
    SetMsg("not available on this platform");
#endif
  }

  // Give the port back to the router
  void Stop()
  {
#ifdef UPNP_AVAILABLE
    if (th.joinable())
      th.join();
    if (mapped)
    {
      Soap("DeletePortMapping",
           "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(extPort) +
           "</NewExternalPort><NewProtocol>TCP</NewProtocol>");
      mapped = false;
    }
#endif
    state = IDLE;
  }

private:
  std::atomic<int> state{IDLE};
  mutable std::mutex mu;
  std::string extIp, msg;
  std::thread th;
  int extPort = 0;
  bool mapped = false;
  std::string host, ctlPath, svcType, localIp;
  int hostPort = 0;

  void SetMsg(const std::string &m) { std::lock_guard<std::mutex> l(mu); msg = m; }
  void Fail(const std::string &m) { SetMsg(m); state = FAILED; }

#ifdef UPNP_AVAILABLE
  static bool Private(const std::string &ip)
  {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
      return true;
    return a == 0 || a == 10 || a == 127 || (a == 100 && b >= 64 && b <= 127) || (a == 169 && b == 254) ||
           (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
  }

  // Shout for a router and return the address of its description file
  bool Discover(std::string &location)
  {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
      return false;
    sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)ssdpPort);
    inet_pton(AF_INET, ssdpAddr.c_str(), &to.sin_addr);
    static const char *const kTargets[] = {
      "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
      "urn:schemas-upnp-org:device:InternetGatewayDevice:2" };
    for (int round = 0; round < 3; round++)
    {
      for (const char *st : kTargets)
      {
        std::string req = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + st + "\r\n\r\n";
        sendto(s, req.data(), req.size(), 0, (sockaddr *)&to, sizeof(to));
      }
      for (int waited = 0; waited < 1000;)
      {
        struct pollfd pf = { s, POLLIN, 0 };
        const int r = poll(&pf, 1, 200);
        waited += 200;
        if (r <= 0)
          continue;
        char buf[1500];
        ssize_t n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
          continue;
        buf[n] = 0;
        std::string t(buf);
        for (auto &c : t)
          c = (char)tolower((unsigned char)c);
        size_t p = t.find("location:");
        if (p == std::string::npos)
          continue;
        size_t e = t.find("\r", p);
        std::string orig(buf);
        location = orig.substr(p + 9, (e == std::string::npos ? orig.size() : e) - p - 9);
        while (!location.empty() && location[0] == ' ')
          location.erase(0, 1);
        close(s);
        return true;
      }
    }
    close(s);
    return false;
  }

  static bool ParseUrl(const std::string &u, std::string &h, int &port, std::string &path)
  {
    if (u.compare(0, 7, "http://") != 0)
      return false;
    size_t slash = u.find('/', 7);
    std::string hp = u.substr(7, slash == std::string::npos ? std::string::npos : slash - 7);
    path = slash == std::string::npos ? "/" : u.substr(slash);
    size_t colon = hp.find(':');
    h = hp.substr(0, colon);
    port = colon == std::string::npos ? 80 : atoi(hp.c_str() + colon + 1);
    return !h.empty() && port > 0;
  }

  // One small HTTP exchange; returns the whole reply (headers + body)
  bool Http(const std::string &h, int port, const std::string &request, std::string &reply, std::string *myIp = nullptr)
  {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, h.c_str(), &sa.sin_addr) != 1)
    {
      close(s);
      return false;
    }
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int r = connect(s, (sockaddr *)&sa, sizeof(sa));
    if (r < 0 && errno == EINPROGRESS)
    {
      struct pollfd pf = { s, POLLOUT, 0 };
      int e = 1;
      if (poll(&pf, 1, 3000) > 0)
      {
        socklen_t el = sizeof(e);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el);
      }
      r = e == 0 ? 0 : -1;
    }
    fcntl(s, F_SETFL, fl);
    if (r != 0)
    {
      close(s);
      return false;
    }
    if (myIp)
    {
      sockaddr_in me;
      socklen_t ml = sizeof(me);
      char ip[INET_ADDRSTRLEN] = "";
      if (getsockname(s, (sockaddr *)&me, &ml) == 0)
        inet_ntop(AF_INET, &me.sin_addr, ip, sizeof(ip));
      *myIp = ip;
    }
    size_t off = 0;
    while (off < request.size())
    {
      ssize_t n = send(s, request.data() + off, request.size() - off, MSG_NOSIGNAL);
      if (n <= 0)
      {
        close(s);
        return false;
      }
      off += (size_t)n;
    }
    reply.clear();
    for (;;)
    {
      struct pollfd pf = { s, POLLIN, 0 };
      if (poll(&pf, 1, 3000) <= 0)
        break;
      char buf[4096];
      ssize_t n = recv(s, buf, sizeof(buf), 0);
      if (n <= 0)
        break;
      reply.append(buf, (size_t)n);
      if (reply.size() > 200000)
        break;
    }
    close(s);
    return !reply.empty();
  }

  static std::string Between(const std::string &t, const std::string &a, const std::string &b, size_t from = 0)
  {
    size_t i = t.find(a, from);
    if (i == std::string::npos)
      return std::string();
    i += a.size();
    size_t j = t.find(b, i);
    return j == std::string::npos ? std::string() : t.substr(i, j - i);
  }

  // The service that can open ports, and the address to send commands to
  bool ReadDescription(const std::string &location)
  {
    std::string h, path;
    int port;
    if (!ParseUrl(location, h, port, path))
      return false;
    std::string reply;
    if (!Http(h, port, "GET " + path + " HTTP/1.0\r\nHost: " + h + ":" + std::to_string(port) + "\r\n\r\n", reply, &localIp))
      return false;
    static const char *const kSvc[] = {
      "urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1",
      "urn:schemas-upnp-org:service:WANPPPConnection:1" };
    for (const char *svc : kSvc)
    {
      size_t at = reply.find(std::string("<serviceType>") + svc);
      if (at == std::string::npos)
        continue;
      std::string ctl = Between(reply, "<controlURL>", "</controlURL>", at);
      if (ctl.empty())
        continue;
      std::string base = Between(reply, "<URLBase>", "</URLBase>");
      host = h;
      hostPort = port;
      if (ctl.compare(0, 7, "http://") == 0)
      {
        std::string h2, p2;
        int pt;
        if (!ParseUrl(ctl, h2, pt, p2))
          continue;
        host = h2; hostPort = pt; ctlPath = p2;
      }
      else
      {
        if (!base.empty() && ParseUrl(base, h, port, path))
        {
          host = h;
          hostPort = port;
        }
        ctlPath = ctl[0] == '/' ? ctl : "/" + ctl;
      }
      svcType = svc;
      return true;
    }
    return false;
  }

  // A command to the router; returns its reply (empty on failure)
  std::string Soap(const std::string &action, const std::string &args, bool *fault = nullptr)
  {
    if (host.empty())
      return std::string();
    const std::string body =
      "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" + svcType + "\">" +
      args + "</u:" + action + "></s:Body></s:Envelope>";
    const std::string req =
      "POST " + ctlPath + " HTTP/1.0\r\nHost: " + host + ":" + std::to_string(hostPort) +
      "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + svcType + "#" + action +
      "\"\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::string reply;
    if (!Http(host, hostPort, req, reply))
      return std::string();
    const bool bad = reply.find(" 200") == std::string::npos || reply.find("<s:Fault>") != std::string::npos ||
                     reply.find("Fault>") != std::string::npos;
    if (fault)
      *fault = bad;
    return reply;
  }

  void Run()
  {
    std::string location;
    if (!Discover(location))
    {
      Fail("your router does not offer automatic port opening (UPnP)");
      return;
    }
    if (!ReadDescription(location) || localIp.empty())
    {
      Fail("could not talk to your router");
      return;
    }
    const std::string ps = std::to_string(extPort);
    auto add = [&](const char *lease) {
      bool fault = true;
      Soap("AddPortMapping",
           "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + ps + "</NewExternalPort><NewProtocol>TCP</NewProtocol>"
           "<NewInternalPort>" + ps + "</NewInternalPort><NewInternalClient>" + localIp + "</NewInternalClient>"
           "<NewEnabled>1</NewEnabled><NewPortMappingDescription>Lost World VR</NewPortMappingDescription>"
           "<NewLeaseDuration>" + std::string(lease) + "</NewLeaseDuration>", &fault);
      return !fault;
    };
    if (!add("7200") && !add("0"))      // some routers only accept a lease of 0 (= until removed)
    {
      Fail("your router refused to open the port");
      return;
    }
    mapped = true;
    std::string r = Soap("GetExternalIPAddress", "");
    std::string ip = Between(r, "<NewExternalIPAddress>", "</NewExternalIPAddress>");
    if (ip.empty() || Private(ip))
    {
      Fail("your internet provider does not give you a public address, so a direct connection is not possible");
      return;
    }
    { std::lock_guard<std::mutex> l(mu); extIp = ip; msg = "port opened"; }
    state = OK;
  }
#endif
};

#endif
