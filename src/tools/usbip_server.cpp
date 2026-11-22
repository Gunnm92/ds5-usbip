// SPDX-License-Identifier: MIT
#include "usbip_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <thread>
#include <vector>

#include "tool_common.hpp"
#include "usbip/usbip_protocol.hpp"
#include "usbip/usbip_session.hpp"

namespace ds5usbip {

  namespace {
    namespace ds = dualsense;
    using namespace usbip;

    constexpr const char *kBusId = "1-1";
    constexpr std::uint32_t kBusNum = 1;
    constexpr std::uint32_t kDevNum = 1;

    void note(const std::string &msg) {
      tools::log_stderr(msg);
    }

    // Exact read; false if the connection closes or stop becomes true.
    bool recv_exact(int fd, void *buf, std::size_t len, const std::atomic<bool> &stop) {
      auto *p = static_cast<std::uint8_t *>(buf);
      while (len) {
        const ssize_t n = ::recv(fd, p, len, 0);
        if (n > 0) {
          p += n;
          len -= static_cast<std::size_t>(n);
          continue;
        }
        if (n == 0) {
          return false;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          if (stop.load()) {
            return false;
          }
          continue;
        }
        return false;
      }
      return true;
    }

    bool send_all(int fd, const void *buf, std::size_t len) {
      const auto *p = static_cast<const std::uint8_t *>(buf);
      while (len) {
        const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
          if (errno == EINTR) {
            continue;
          }
          return false;
        }
        p += n;
        len -= static_cast<std::size_t>(n);
      }
      return true;
    }

    UsbDevice wire_device() {
      UsbDevice d {};
      std::snprintf(d.path, sizeof(d.path), "/sys/devices/ds5-usbip/%s", kBusId);
      std::snprintf(d.busid, sizeof(d.busid), "%s", kBusId);
      d.busnum = be32(kBusNum);
      d.devnum = be32(kDevNum);
      d.speed = be32(kSpeedHigh);
      d.idVendor = be16(ds::kVendorId);
      d.idProduct = be16(ds::kProductId);
      d.bcdDevice = be16(ds::kBcdDevice);
      d.bDeviceClass = 0;
      d.bDeviceSubClass = 0;
      d.bDeviceProtocol = 0;
      d.bConfigurationValue = 0;
      d.bNumConfigurations = 1;
      d.bNumInterfaces = ds::kNumInterfaces;
      return d;
    }

    std::vector<std::uint8_t> devlist_reply() {
      // Class / subclass / protocol of the 4 interfaces.
      static constexpr UsbInterface interfaces[] {
        {0x01, 0x01, 0x00, 0},  // audio control
        {0x01, 0x02, 0x00, 0},  // audio output stream
        {0x01, 0x02, 0x00, 0},  // audio input stream
        {0x03, 0x00, 0x00, 0},  // HID
      };
      std::vector<std::uint8_t> out;
      const OpHeader h {be16(kVersion), be16(kOpRepDevlist), 0};
      const std::uint32_t ndev = be32(1);
      const UsbDevice dev = wire_device();
      auto append = [&out](const void *p, std::size_t n) {
        const auto *b = static_cast<const std::uint8_t *>(p);
        out.insert(out.end(), b, b + n);
      };
      append(&h, sizeof(h));
      append(&ndev, sizeof(ndev));
      append(&dev, sizeof(dev));
      append(interfaces, sizeof(interfaces));
      return out;
    }

    // Closes the descriptor on scope exit unless it was released.
    class FdGuard {
    public:
      explicit FdGuard(int fd):
          fd_(fd) {}
      ~FdGuard() {
        if (fd_ >= 0) {
          ::close(fd_);
        }
      }
      FdGuard(const FdGuard &) = delete;
      FdGuard &operator=(const FdGuard &) = delete;
      int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
      }

    private:
      int fd_;
    };
  }  // namespace

  UsbipServer::UsbipServer(DualSenseDevice &device, Options options, Callbacks callbacks):
      device_(device),
      options_(std::move(options)),
      callbacks_(std::move(callbacks)) {}

  bool UsbipServer::verbose() const {
    return options_.verbose_flag ? options_.verbose_flag->load() : options_.verbose;
  }

  int UsbipServer::run(const std::atomic<bool> &stop) {
    const int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) {
      note(std::format("usbip: socket : {}", std::strerror(errno)));
      return 1;
    }
    const int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(options_.port);
    if (::inet_pton(AF_INET, options_.bind_address.c_str(), &addr.sin_addr) != 1) {
      note(std::format("usbip: adresse invalide : {}", options_.bind_address));
      ::close(lfd);
      return 1;
    }
    if (::bind(lfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(lfd, 4) < 0) {
      note(std::format("usbip: écoute sur {}:{} impossible : {}", options_.bind_address, options_.port, std::strerror(errno)));
      ::close(lfd);
      return 1;
    }
    note(std::format("usbip: en écoute sur {}:{}, busid {} (054c:0ce6 DualSense)", options_.bind_address, options_.port, kBusId));

    while (!stop.load()) {
      pollfd p {lfd, POLLIN, 0};
      if (::poll(&p, 1, 200) <= 0) {
        continue;
      }
      sockaddr_in peer {};
      socklen_t plen = sizeof(peer);
      const int fd = ::accept4(lfd, reinterpret_cast<sockaddr *>(&peer), &plen, SOCK_CLOEXEC);
      if (fd < 0) {
        continue;
      }
      char ip[INET_ADDRSTRLEN] = "?";
      ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
      if (verbose()) {
        note(std::format("usbip: connexion de {}", ip));
      }
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      // Receive timeout for the handshake only; cleared before the session.
      timeval tv {0, 200000};
      ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      handle_connection(fd, stop);  // owns fd
    }
    ::close(lfd);
    return 0;
  }

  void UsbipServer::handle_connection(int fd_in, const std::atomic<bool> &stop) {
    FdGuard guard(fd_in);
    const int fd = fd_in;
    OpHeader h {};
    if (!recv_exact(fd, &h, sizeof(h), stop)) {
      return;
    }
    const auto version = ntohs(h.version);
    const auto code = ntohs(h.code);
    if (version != kVersion) {
      note(std::format("usbip: version de protocole 0x{:04x} non gérée", version));
      return;
    }
    if (code == kOpReqDevlist) {
      const auto r = devlist_reply();
      send_all(fd, r.data(), r.size());
      return;
    }
    if (code != kOpReqImport) {
      note(std::format("usbip: opération 0x{:04x} non gérée", code));
      return;
    }
    char busid[kBusIdSize + 1] {};
    if (!recv_exact(fd, busid, kBusIdSize, stop)) {
      return;
    }
    const bool match = std::strcmp(busid, kBusId) == 0;
    const OpHeader rh {be16(kVersion), be16(kOpRepImport), be32(match ? 0 : 1)};
    if (!send_all(fd, &rh, sizeof(rh))) {
      return;
    }
    if (!match) {
      note(std::format("usbip: import refusé, busid inconnu « {} »", busid));
      return;
    }
    const UsbDevice dev = wire_device();
    if (!send_all(fd, &dev, sizeof(dev))) {
      return;
    }
    run_session(guard.release(), stop);
  }

  void UsbipServer::run_session(int fd, const std::atomic<bool> &stop) {
    // From here the session owns fd: UsbipSession::stop() closes it.
    note("usbip: périphérique importé, session ouverte");
    device_.reset();
    if (callbacks_.session) {
      callbacks_.session(true);
    }
    std::atomic<bool> closed {false};
    UsbipSession::Callbacks cb;
    cb.audio_out = [this](std::span<const std::uint8_t> pcm) {
      if (callbacks_.audio_out) {
        callbacks_.audio_out(pcm);
      }
    };
    cb.closed = [&closed] {
      closed.store(true);
    };
    UsbipSession session(fd, device_, std::move(cb), tools::log_stderr);
    session.set_verbose([this] {
      return verbose();
    });
    if (session.start()) {
      while (!stop.load() && !closed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    session.stop();
    device_.reset();
    if (callbacks_.session) {
      callbacks_.session(false);
    }
  }

}  // namespace ds5usbip
