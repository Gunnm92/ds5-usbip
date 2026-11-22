// SPDX-License-Identifier: MIT
// ds5-usbip-attach : attaches (or detaches) a USB/IP device on vhci-hcd,
// without the usbip tool. Same sequence as "usbip attach": OP_REQ_IMPORT over
// TCP, then "port sockfd devid speed" written to
// /sys/devices/platform/vhci_hcd.0/attach; the kernel keeps the socket.
// Run as root on the machine that has vhci-hcd.
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "usbip/usbip_protocol.hpp"
#include "usbip/vhci.hpp"

namespace {
  using namespace ds5usbip::usbip;
  namespace vhci = ds5usbip::vhci;

  bool recv_exact(int fd, void *buf, std::size_t len) {
    auto *p = static_cast<char *>(buf);
    while (len) {
      const ssize_t n = ::recv(fd, p, len, 0);
      if (n <= 0) {
        return false;
      }
      p += n;
      len -= static_cast<std::size_t>(n);
    }
    return true;
  }

  int connect_to(const std::string &host, const std::string &port) {
    // Literal IPv4 address: no getaddrinfo, which depends on the NSS modules
    // of the host's glibc in a static binary.
    sockaddr_in sin {};
    if (::inet_pton(AF_INET, host.c_str(), &sin.sin_addr) == 1) {
      sin.sin_family = AF_INET;
      sin.sin_port = htons(static_cast<std::uint16_t>(std::atoi(port.c_str())));
      const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr *>(&sin), sizeof(sin)) == 0) {
        return fd;
      }
      std::fprintf(stderr, "connexion à %s:%s impossible : %s\n", host.c_str(), port.c_str(), std::strerror(errno));
      if (fd >= 0) {
        ::close(fd);
      }
      return -1;
    }
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (const int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc) {
      std::fprintf(stderr, "adresse %s : %s\n", host.c_str(), gai_strerror(rc));
      return -1;
    }
    int fd = -1;
    for (auto *ai = res; ai; ai = ai->ai_next) {
      fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd >= 0 && ::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
        break;
      }
      if (fd >= 0) {
        ::close(fd);
      }
      fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
      std::fprintf(stderr, "connexion à %s:%s impossible : %s\n", host.c_str(), port.c_str(), std::strerror(errno));
    }
    return fd;
  }

  int attach(const std::string &host, const std::string &port, const std::string &busid) {
    const int fd = connect_to(host, port);
    if (fd < 0) {
      return 1;
    }
    char req[sizeof(OpHeader) + kBusIdSize] {};
    const OpHeader h {be16(kVersion), be16(kOpReqImport), 0};
    std::memcpy(req, &h, sizeof(h));
    std::strncpy(req + sizeof(h), busid.c_str(), kBusIdSize - 1);
    if (::send(fd, req, sizeof(req), 0) != static_cast<ssize_t>(sizeof(req))) {
      std::fprintf(stderr, "envoi de OP_REQ_IMPORT impossible\n");
      return 1;
    }
    OpHeader rh;
    if (!recv_exact(fd, &rh, sizeof(rh)) || ntohs(rh.code) != kOpRepImport || rh.status != 0) {
      std::fprintf(stderr, "import de %s refusé par le serveur\n", busid.c_str());
      return 1;
    }
    UsbDevice dev;
    if (!recv_exact(fd, &dev, sizeof(dev))) {
      std::fprintf(stderr, "réponse OP_REP_IMPORT incomplète\n");
      return 1;
    }
    const auto ports = vhci::load_ports();
    if (!ports) {
      std::fprintf(stderr, "%s/status illisible : vhci-hcd est-il chargé ?\n", std::string(vhci::kDefaultSysfs).c_str());
      return 1;
    }
    const auto vport = vhci::pick_free_port(*ports);
    if (!vport) {
      std::fprintf(stderr, "aucun port haute vitesse libre sur vhci-hcd\n");
      return 1;
    }
    const auto devid = (ntohl(dev.busnum) << 16) | ntohl(dev.devnum);
    const auto speed = static_cast<int>(ntohl(dev.speed));
    if (const int err = vhci::attach_device(vhci::kDefaultSysfs, *vport, fd, devid, speed); err < 0) {
      std::fprintf(stderr, "écriture de « attach » refusée : %s\n", std::strerror(-err));
      return 1;
    }
    std::printf(
      "%s (%04x:%04x) branché sur le port %d de vhci-hcd\n",
      busid.c_str(),
      ntohs(dev.idVendor),
      ntohs(dev.idProduct),
      *vport
    );
    return 0;  // the kernel holds its own reference on the socket
  }

  void usage(const char *argv0) {
    std::printf(
      "Usage : %s attach HÔTE [-p PORT] [-b BUSID]\n"
      "        %s detach PORT_VHCI\n"
      "        %s status\n"
      "\n"
      "Remplace « usbip attach/detach/port » quand l'outil usbip est absent.\n"
      "Défauts : port TCP 3240, busid 1-1. À lancer en root.\n",
      argv0,
      argv0,
      argv0
    );
  }
}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "-h" || cmd == "--help") {
    usage(argv[0]);
    return 0;
  }
  if (cmd == "status") {
    const auto text = vhci::read_text(std::string(vhci::kDefaultSysfs) + "/status");
    if (!text) {
      std::fprintf(stderr, "%s/status illisible : vhci-hcd est-il chargé ?\n", std::string(vhci::kDefaultSysfs).c_str());
      return 1;
    }
    std::istringstream lines(*text);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.rfind("hs", 0) != 0 || line.find(" 004 ") == std::string::npos) {
        std::puts(line.c_str());  // en-tête et ports occupés
      }
    }
    return 0;
  }
  if (cmd == "detach" && argc == 3) {
    const int err = vhci::detach_port(vhci::kDefaultSysfs, std::atoi(argv[2]));
    if (err < 0) {
      std::fprintf(stderr, "écriture de « detach » refusée : %s\n", std::strerror(-err));
      return 1;
    }
    return 0;
  }
  if (cmd == "attach" && argc >= 3) {
    std::string host = argv[2], port = "3240", busid = "1-1";
    for (int i = 3; i + 1 < argc; i += 2) {
      if (!std::strcmp(argv[i], "-p")) {
        port = argv[i + 1];
      } else if (!std::strcmp(argv[i], "-b")) {
        busid = argv[i + 1];
      }
    }
    return attach(host, port, busid);
  }
  usage(argv[0]);
  return 2;
}
