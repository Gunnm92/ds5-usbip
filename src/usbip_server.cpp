// SPDX-License-Identifier: MIT
#include "usbip_server.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "usbip_protocol.hpp"

namespace ds5usbip {

  namespace {
    namespace ds = dualsense;
    using namespace usbip;
    using Clock = std::chrono::steady_clock;

    constexpr const char *kBusId = "1-1";
    constexpr std::uint32_t kBusNum = 1;
    constexpr std::uint32_t kDevNum = 1;
    constexpr std::int32_t kMaxTransfer = 1 << 20;
    constexpr std::int32_t kMaxIsoPackets = 1024;

    __attribute__((format(printf, 1, 2))) void log(const char *fmt, ...) {
      va_list ap;
      va_start(ap, fmt);
      std::vfprintf(stderr, fmt, ap);
      va_end(ap);
      std::fputc('\n', stderr);
    }

    // Lecture exacte ; false si la connexion se ferme ou si stop passe à true.
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
      // Classe / sous-classe / protocole des 4 interfaces.
      static constexpr UsbInterface interfaces[] {
        {0x01, 0x01, 0x00, 0},  // audio, contrôle
        {0x01, 0x02, 0x00, 0},  // audio, flux de sortie
        {0x01, 0x02, 0x00, 0},  // audio, flux d'entrée
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

    // Une session importée : lecture des commandes sur le fil appelant,
    // réponses différées (HID IN, isochrone) sur un fil d'horloge.
    class Session {
    public:
      Session(int fd, DualSenseDevice &device, const UsbipServer::Callbacks &callbacks, std::function<bool()> verbose):
          fd_(fd),
          device_(device),
          callbacks_(callbacks),
          verbose_(std::move(verbose)) {}

      void run(const std::atomic<bool> &stop) {
        std::thread timer([this] {
          timer_loop();
        });
        while (!stop.load()) {
          Header h;
          if (!recv_exact(fd_, &h, sizeof(h), stop)) {
            break;
          }
          const auto command = ntohl(h.base.command);
          bool ok = false;
          if (command == kCmdSubmit) {
            ok = handle_submit(h, stop);
          } else if (command == kCmdUnlink) {
            ok = handle_unlink(h);
          } else {
            log("usbip: commande inconnue 0x%x, fin de session", command);
          }
          if (!ok) {
            break;
          }
        }
        {
          std::lock_guard lock(mutex_);
          stopping_ = true;
        }
        cv_.notify_all();
        timer.join();
        log("usbip: session terminée (%zu URB isochrones sortants, %zu rapports HID envoyés)", iso_out_count_, hid_in_count_);
      }

    private:
      struct Deferred {
        std::uint32_t seqnum;
        std::vector<std::uint8_t> bytes;
      };

      struct PendingHidIn {
        std::uint32_t seqnum;
        std::int32_t length;
      };

      static std::vector<std::uint8_t> ret_submit(
        std::uint32_t seqnum,
        std::int32_t status,
        std::int32_t actual_length,
        std::int32_t start_frame,
        std::int32_t number_of_packets,
        std::span<const std::uint8_t> data,
        std::span<const IsoPacketDescriptor> iso
      ) {
        Header h {};
        h.base.command = be32(kRetSubmit);
        h.base.seqnum = be32(seqnum);
        h.u.ret_submit.status = be32s(status);
        h.u.ret_submit.actual_length = be32s(actual_length);
        h.u.ret_submit.start_frame = be32s(start_frame);
        h.u.ret_submit.number_of_packets = be32s(number_of_packets);
        h.u.ret_submit.error_count = 0;
        std::vector<std::uint8_t> out(sizeof(h) + data.size() + iso.size_bytes());
        std::memcpy(out.data(), &h, sizeof(h));
        if (!data.empty()) {
          std::memcpy(out.data() + sizeof(h), data.data(), data.size());
        }
        if (!iso.empty()) {
          std::memcpy(out.data() + sizeof(h) + data.size(), iso.data(), iso.size_bytes());
        }
        return out;
      }

      bool send(const std::vector<std::uint8_t> &bytes) {
        std::lock_guard lock(send_mutex_);
        return send_all(fd_, bytes.data(), bytes.size());
      }

      bool handle_submit(const Header &h, const std::atomic<bool> &stop) {
        const auto seqnum = ntohl(h.base.seqnum);
        const auto direction = ntohl(h.base.direction);
        const auto ep = ntohl(h.base.ep);
        const auto &c = h.u.cmd_submit;
        const auto length = static_cast<std::int32_t>(ntohl(static_cast<std::uint32_t>(c.transfer_buffer_length)));
        const auto start_frame = static_cast<std::int32_t>(ntohl(static_cast<std::uint32_t>(c.start_frame)));
        auto npackets = static_cast<std::int32_t>(ntohl(static_cast<std::uint32_t>(c.number_of_packets)));

        if (length < 0 || length > kMaxTransfer || npackets > kMaxIsoPackets) {
          log("usbip: CMD_SUBMIT invalide (longueur %d, %d paquets)", length, npackets);
          return false;
        }
        const bool iso = npackets > 0;
        if (npackets < 0) {
          npackets = 0;  // 0xffffffff : pas isochrone (client Windows)
        }

        std::vector<std::uint8_t> out_data;
        if (direction == kDirOut && length > 0) {
          out_data.resize(static_cast<std::size_t>(length));
          if (!recv_exact(fd_, out_data.data(), out_data.size(), stop)) {
            return false;
          }
        }
        std::vector<IsoPacketDescriptor> iso_desc;
        if (iso) {
          iso_desc.resize(static_cast<std::size_t>(npackets));
          if (!recv_exact(fd_, iso_desc.data(), iso_desc.size() * sizeof(IsoPacketDescriptor), stop)) {
            return false;
          }
          for (auto &d : iso_desc) {
            d.offset = ntohl(d.offset);
            d.length = ntohl(d.length);
            if (static_cast<std::uint64_t>(d.offset) + d.length > static_cast<std::uint64_t>(length)) {
              log("usbip: paquet isochrone hors du tampon");
              return false;
            }
          }
        }

        if (ep == 0) {
          return handle_control(seqnum, direction, c, out_data, length);
        }
        if (ep == ds::kHidInEndpoint && direction == kDirIn && !iso) {
          std::lock_guard lock(mutex_);
          hid_pending_.push_back({seqnum, length});
          cv_.notify_all();
          return true;
        }
        if (ep == ds::kHidOutEndpoint && direction == kDirOut && !iso) {
          device_.interrupt_out(out_data);
          return send(ret_submit(seqnum, 0, length, 0, 0, {}, {}));
        }
        if (ep == ds::kAudioOutEndpoint && direction == kDirOut && iso) {
          return handle_audio_out(seqnum, start_frame, out_data, iso_desc);
        }
        if (ep == ds::kAudioInEndpoint && direction == kDirIn && iso) {
          return handle_audio_in(seqnum, start_frame, iso_desc);
        }
        log("usbip: point de terminaison inattendu ep=%u dir=%u iso=%d", ep, direction, iso);
        return send(ret_submit(seqnum, -EPIPE, 0, 0, npackets, {}, {}));
      }

      bool handle_control(
        std::uint32_t seqnum,
        std::uint32_t direction,
        const CmdSubmit &c,
        const std::vector<std::uint8_t> &out_data,
        std::int32_t length
      ) {
        const auto setup = SetupPacket::parse(c.setup);
        auto r = device_.control(setup, out_data);
        if (direction == kDirIn && r.data.size() > static_cast<std::size_t>(length)) {
          r.data.resize(static_cast<std::size_t>(length));
        }
        if (verbose_() || r.status) {
          log(
            "ctrl: %02x %02x wValue=%04x wIndex=%04x wLength=%u -> %s %zu",
            setup.bmRequestType,
            setup.bRequest,
            setup.wValue,
            setup.wIndex,
            setup.wLength,
            r.status ? "STALL" : "ok",
            direction == kDirIn ? r.data.size() : out_data.size()
          );
        }
        const std::int32_t actual = r.status ? 0 : static_cast<std::int32_t>(direction == kDirIn ? r.data.size() : out_data.size());
        return send(ret_submit(seqnum, r.status, actual, 0, 0, direction == kDirIn ? std::span<const std::uint8_t>(r.data) : std::span<const std::uint8_t> {}, {}));
      }

      // Échéance d'une réponse isochrone : l'URB se termine quand ses
      // paquets ont été « joués », à raison de 1 ms par paquet, à la suite
      // de l'URB précédent du même sens.
      Clock::time_point schedule(Clock::time_point &anchor, std::size_t npackets) {
        const auto now = Clock::now();
        if (anchor < now - std::chrono::milliseconds(20)) {
          anchor = now;  // début ou reprise du flux
        }
        anchor += std::chrono::microseconds(ds::kIsoPacketUs) * npackets;
        return anchor;
      }

      bool handle_audio_out(
        std::uint32_t seqnum,
        std::int32_t start_frame,
        const std::vector<std::uint8_t> &data,
        std::vector<IsoPacketDescriptor> &iso_desc
      ) {
        std::int32_t total = 0;
        for (auto &d : iso_desc) {
          if (callbacks_.audio_out && d.length) {
            callbacks_.audio_out(std::span(data).subspan(d.offset, d.length));
          }
          total += static_cast<std::int32_t>(d.length);
          const auto len = d.length;
          d.offset = be32(d.offset);
          d.length = be32(len);
          d.actual_length = be32(len);
          d.status = 0;
        }
        ++iso_out_count_;
        auto bytes = ret_submit(seqnum, 0, total, start_frame, static_cast<std::int32_t>(iso_desc.size()), {}, iso_desc);
        std::lock_guard lock(mutex_);
        const auto due = schedule(iso_out_anchor_, iso_desc.size());
        deferred_.emplace(due, Deferred {seqnum, std::move(bytes)});
        cv_.notify_all();
        return true;
      }

      bool handle_audio_in(std::uint32_t seqnum, std::int32_t start_frame, std::vector<IsoPacketDescriptor> &iso_desc) {
        // Micro : silence, 48 trames stéréo par paquet de 1 ms. Les données
        // IN partent compactées (usbip_pad_iso les replace côté vhci).
        constexpr std::uint32_t kPacketBytes = ds::kSampleRate / 1000 * ds::kAudioInChannels * 2;
        std::int32_t total = 0;
        for (auto &d : iso_desc) {
          const auto actual = std::min(d.length, kPacketBytes);
          total += static_cast<std::int32_t>(actual);
          d.offset = be32(d.offset);
          d.length = be32(d.length);
          d.actual_length = be32(actual);
          d.status = 0;
        }
        const std::vector<std::uint8_t> silence(static_cast<std::size_t>(total), 0);
        auto bytes = ret_submit(seqnum, 0, total, start_frame, static_cast<std::int32_t>(iso_desc.size()), silence, iso_desc);
        std::lock_guard lock(mutex_);
        const auto due = schedule(iso_in_anchor_, iso_desc.size());
        deferred_.emplace(due, Deferred {seqnum, std::move(bytes)});
        cv_.notify_all();
        return true;
      }

      bool handle_unlink(const Header &h) {
        const auto seqnum = ntohl(h.base.seqnum);
        const auto victim = ntohl(h.u.cmd_unlink.seqnum);
        bool found = false;
        {
          std::lock_guard lock(mutex_);
          for (auto it = hid_pending_.begin(); it != hid_pending_.end(); ++it) {
            if (it->seqnum == victim) {
              hid_pending_.erase(it);
              found = true;
              break;
            }
          }
          if (!found) {
            for (auto it = deferred_.begin(); it != deferred_.end(); ++it) {
              if (it->second.seqnum == victim) {
                deferred_.erase(it);
                found = true;
                break;
              }
            }
          }
        }
        if (verbose_()) {
          log("usbip: UNLINK %u -> %s", victim, found ? "annulé" : "déjà terminé");
        }
        Header r {};
        r.base.command = be32(kRetUnlink);
        r.base.seqnum = be32(seqnum);
        r.u.ret_unlink.status = be32s(found ? -ECONNRESET : 0);
        std::vector<std::uint8_t> bytes(sizeof(r));
        std::memcpy(bytes.data(), &r, sizeof(r));
        return send(bytes);
      }

      void timer_loop() {
        const auto hid_interval = std::chrono::microseconds(ds::kHidIntervalUs);
        auto next_hid = Clock::now();
        std::unique_lock lock(mutex_);
        while (!stopping_) {
          const auto now = Clock::now();
          if (!deferred_.empty() && deferred_.begin()->first <= now) {
            auto bytes = std::move(deferred_.begin()->second.bytes);
            deferred_.erase(deferred_.begin());
            lock.unlock();
            send(bytes);
            lock.lock();
            continue;
          }
          if (!hid_pending_.empty() && next_hid <= now) {
            const auto p = hid_pending_.front();
            hid_pending_.pop_front();
            lock.unlock();
            const auto report = device_.next_input_report();
            const auto n = std::min<std::size_t>(report.size(), static_cast<std::size_t>(p.length));
            send(ret_submit(p.seqnum, 0, static_cast<std::int32_t>(n), 0, 0, std::span(report).first(n), {}));
            ++hid_in_count_;
            lock.lock();
            // Garde la phase si le retard est faible ; après un temps mort,
            // repart de maintenant (jamais deux rapports d'affilée).
            next_hid += hid_interval;
            if (next_hid <= now) {
              next_hid = now + hid_interval;
            }
            continue;
          }
          auto wake = Clock::time_point::max();
          if (!deferred_.empty()) {
            wake = deferred_.begin()->first;
          }
          if (!hid_pending_.empty()) {
            wake = std::min(wake, next_hid);
          }
          if (wake == Clock::time_point::max()) {
            cv_.wait(lock);
          } else {
            cv_.wait_until(lock, wake);
          }
        }
      }

      int fd_;
      DualSenseDevice &device_;
      const UsbipServer::Callbacks &callbacks_;
      std::function<bool()> verbose_;

      std::mutex send_mutex_;
      std::mutex mutex_;
      std::condition_variable cv_;
      bool stopping_ = false;
      std::multimap<Clock::time_point, Deferred> deferred_;
      std::deque<PendingHidIn> hid_pending_;
      Clock::time_point iso_out_anchor_ {};
      Clock::time_point iso_in_anchor_ {};
      std::size_t iso_out_count_ = 0;
      std::size_t hid_in_count_ = 0;
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
      log("usbip: socket: %s", std::strerror(errno));
      return 1;
    }
    const int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(options_.port);
    if (::inet_pton(AF_INET, options_.bind_address.c_str(), &addr.sin_addr) != 1) {
      log("usbip: adresse invalide : %s", options_.bind_address.c_str());
      ::close(lfd);
      return 1;
    }
    if (::bind(lfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(lfd, 4) < 0) {
      log("usbip: écoute sur %s:%u impossible : %s", options_.bind_address.c_str(), options_.port, std::strerror(errno));
      ::close(lfd);
      return 1;
    }
    log("usbip: en écoute sur %s:%u, busid %s (054c:0ce6 DualSense)", options_.bind_address.c_str(), options_.port, kBusId);

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
        log("usbip: connexion de %s", ip);
      }
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      timeval tv {0, 200000};
      ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      handle_connection(fd, stop);
      ::close(fd);
    }
    ::close(lfd);
    return 0;
  }

  void UsbipServer::handle_connection(int fd, const std::atomic<bool> &stop) {
    OpHeader h;
    if (!recv_exact(fd, &h, sizeof(h), stop)) {
      return;
    }
    const auto version = ntohs(h.version);
    const auto code = ntohs(h.code);
    if (version != kVersion) {
      log("usbip: version de protocole 0x%04x non gérée", version);
      return;
    }
    if (code == kOpReqDevlist) {
      const auto r = devlist_reply();
      send_all(fd, r.data(), r.size());
      return;
    }
    if (code != kOpReqImport) {
      log("usbip: opération 0x%04x non gérée", code);
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
      log("usbip: import refusé, busid inconnu « %s »", busid);
      return;
    }
    const UsbDevice dev = wire_device();
    if (!send_all(fd, &dev, sizeof(dev))) {
      return;
    }
    run_session(fd, stop);
  }

  void UsbipServer::run_session(int fd, const std::atomic<bool> &stop) {
    log("usbip: périphérique importé, session ouverte");
    device_.reset();
    if (callbacks_.session) {
      callbacks_.session(true);
    }
    Session session(fd, device_, callbacks_, [this] {
      return verbose();
    });
    session.run(stop);
    device_.reset();
    if (callbacks_.session) {
      callbacks_.session(false);
    }
  }

}  // namespace ds5usbip
