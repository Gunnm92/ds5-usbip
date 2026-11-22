// SPDX-License-Identifier: MIT
// ds5-usbip-attach : branche (ou débranche) un périphérique USB/IP sur
// vhci-hcd, sans l'outil usbip. Même séquence que « usbip attach » :
// OP_REQ_IMPORT sur TCP, puis « port sockfd devid speed » écrit dans
// /sys/devices/platform/vhci_hcd.0/attach ; le noyau garde la socket.
// À lancer en root sur la machine qui a vhci-hcd.
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "usbip_protocol.hpp"

namespace {
  using namespace ds5usbip::usbip;

  constexpr const char *kVhci = "/sys/devices/platform/vhci_hcd.0";

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
    // Adresse IPv4 littérale : sans getaddrinfo, qui dépend des modules NSS
    // de la glibc de l'hôte dans un binaire statique.
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

  // Premier port haute vitesse libre (statut 4 = VDEV_ST_NULL) du fichier
  // status : « hub port sta spd dev sockfd local_busid ».
  int free_hs_port() {
    std::ifstream f(std::string(kVhci) + "/status");
    if (!f) {
      std::fprintf(stderr, "%s/status illisible : vhci-hcd est-il chargé ?\n", kVhci);
      return -1;
    }
    std::string line;
    std::getline(f, line);  // en-tête
    while (std::getline(f, line)) {
      std::istringstream in(line);
      std::string hub;
      int port = 0, status = 0;
      if (in >> hub >> port >> status && hub == "hs" && status == 4) {
        return port;
      }
    }
    std::fprintf(stderr, "aucun port haute vitesse libre sur vhci-hcd\n");
    return -1;
  }

  bool write_sysfs(const std::string &file, const std::string &value) {
    std::ofstream f(std::string(kVhci) + "/" + file);
    f << value;
    f.flush();
    if (!f) {
      std::fprintf(stderr, "écriture de « %s » dans %s/%s refusée : %s\n", value.c_str(), kVhci, file.c_str(), std::strerror(errno));
      return false;
    }
    return true;
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
    const int vport = free_hs_port();
    if (vport < 0) {
      return 1;
    }
    const auto devid = (ntohl(dev.busnum) << 16) | ntohl(dev.devnum);
    const auto speed = ntohl(dev.speed);
    std::ostringstream v;
    v << vport << ' ' << fd << ' ' << devid << ' ' << speed;
    if (!write_sysfs("attach", v.str())) {
      return 1;
    }
    std::printf(
      "%s (%04x:%04x) branché sur le port %d de vhci-hcd\n",
      busid.c_str(),
      ntohs(dev.idVendor),
      ntohs(dev.idProduct),
      vport
    );
    return 0;  // le noyau a pris sa référence sur la socket
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
    std::ifstream f(std::string(kVhci) + "/status");
    if (!f) {
      std::fprintf(stderr, "%s/status illisible : vhci-hcd est-il chargé ?\n", kVhci);
      return 1;
    }
    std::string line;
    while (std::getline(f, line)) {
      if (line.rfind("hs", 0) != 0 || line.find(" 004 ") == std::string::npos) {
        std::puts(line.c_str());  // en-tête et ports occupés
      }
    }
    return 0;
  }
  if (cmd == "detach" && argc == 3) {
    return write_sysfs("detach", argv[2]) ? 0 : 1;
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
