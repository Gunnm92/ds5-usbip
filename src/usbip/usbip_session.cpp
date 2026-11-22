// SPDX-License-Identifier: MIT
#include "usbip_session.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#include "usbip_protocol.hpp"

namespace ds5usbip {

  namespace {
    namespace ds = dualsense;
    using namespace usbip;
    using Clock = std::chrono::steady_clock;

    constexpr std::int32_t kMaxTransfer = 1 << 20;
    constexpr std::int32_t kMaxIsoPackets = 1024;

    // Exact read; false on end of stream or error. Interruptions are retried;
    // shutdown() on the descriptor makes the blocked recv() return 0.
    bool recv_exact(int fd, void *buf, std::size_t len) {
      auto *p = static_cast<std::uint8_t *>(buf);
      while (len) {
        const ssize_t n = ::recv(fd, p, len, 0);
        if (n > 0) {
          p += n;
          len -= static_cast<std::size_t>(n);
          continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
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

    std::vector<std::uint8_t> ret_submit(
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
  }  // namespace

  // Everything that runs on the session's threads.
  class UsbipSession::Impl {
  public:
    Impl(int fd, DualSenseDevice &device, Callbacks callbacks, LogFn log):
        fd_(fd),
        device_(device),
        callbacks_(std::move(callbacks)),
        log_(std::move(log)) {}

    ~Impl() {
      stop();
    }

    void set_verbose(std::function<bool()> verbose) {
      verbose_ = std::move(verbose);
    }

    bool start() {
      if (started_ || stop_requested_.load() || fd_ < 0) {
        return false;
      }
      // The reader blocks in recv(): no receive timeout may be left from a
      // handshake, and stop() wakes it with shutdown().
      const timeval none {0, 0};
      ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
      try {
        timer_ = std::thread([this] {
          timer_loop();
        });
        reader_ = std::thread([this] {
          reader_loop();
        });
      } catch (const std::system_error &e) {
        log(std::format("usbip: impossible de créer les threads de session : {}", e.what()));
        stop();
        return false;
      }
      started_ = true;
      return true;
    }

    void stop() {
      if (!stop_requested_.exchange(true) && fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);  // wakes the reader's recv() and any blocked send()
        {
          std::lock_guard lock(mutex_);
          stopping_ = true;
        }
        cv_.notify_all();
      }
      join_threads();
      if (fd_ >= 0 && !fd_closed_) {
        ::close(fd_);
        fd_closed_ = true;
      }
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

    // stop() may run on the reader thread (from the closed callback): a thread
    // never joins itself.
    void join_threads() {
      const auto self = std::this_thread::get_id();
      if (timer_.joinable() && timer_.get_id() != self) {
        timer_.join();
      }
      if (reader_.joinable() && reader_.get_id() != self) {
        reader_.join();
      }
    }

    void log(const std::string &msg) const {
      if (log_) {
        log_(msg);
      }
    }

    bool verbose() const {
      return verbose_ && verbose_();
    }

    bool send_bytes(const std::vector<std::uint8_t> &bytes) {
      std::lock_guard lock(send_mutex_);
      return send_all(fd_, bytes.data(), bytes.size());
    }

    void reader_loop() {
      while (true) {
        Header h {};
        if (!recv_exact(fd_, &h, sizeof(h))) {
          break;
        }
        const auto command = ntohl(h.base.command);
        bool ok = false;
        if (command == kCmdSubmit) {
          ok = handle_submit(h);
        } else if (command == kCmdUnlink) {
          ok = handle_unlink(h);
        } else {
          log(std::format("usbip: commande inconnue 0x{:x}, fin de session", command));
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
      log(std::format("usbip: session terminée ({} URB isochrones sortants, {} rapports HID envoyés)", iso_out_count_, hid_in_count_));
      if (!stop_requested_.load() && callbacks_.closed) {
        callbacks_.closed();
      }
    }

    bool handle_submit(const Header &h) {
      const auto seqnum = ntohl(h.base.seqnum);
      const auto direction = ntohl(h.base.direction);
      const auto ep = ntohl(h.base.ep);
      const auto &c = h.u.cmd_submit;
      const auto length = static_cast<std::int32_t>(ntohl(static_cast<std::uint32_t>(c.transfer_buffer_length)));
      const auto start_frame = static_cast<std::int32_t>(ntohl(static_cast<std::uint32_t>(c.start_frame)));
      auto npackets = static_cast<std::int32_t>(ntohl(static_cast<std::uint32_t>(c.number_of_packets)));

      if (length < 0 || length > kMaxTransfer || npackets > kMaxIsoPackets) {
        log(std::format("usbip: CMD_SUBMIT invalide (longueur {}, {} paquets)", length, npackets));
        return false;
      }
      const bool iso = npackets > 0;
      if (npackets < 0) {
        npackets = 0;  // 0xffffffff: not isochronous (Windows client)
      }

      std::vector<std::uint8_t> out_data;
      if (direction == kDirOut && length > 0) {
        out_data.resize(static_cast<std::size_t>(length));
        if (!recv_exact(fd_, out_data.data(), out_data.size())) {
          return false;
        }
      }
      std::vector<IsoPacketDescriptor> iso_desc;
      if (iso) {
        iso_desc.resize(static_cast<std::size_t>(npackets));
        if (!recv_exact(fd_, iso_desc.data(), iso_desc.size() * sizeof(IsoPacketDescriptor))) {
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
        return send_bytes(ret_submit(seqnum, 0, length, 0, 0, {}, {}));
      }
      if (ep == ds::kAudioOutEndpoint && direction == kDirOut && iso) {
        return handle_audio_out(seqnum, start_frame, out_data, iso_desc);
      }
      if (ep == ds::kAudioInEndpoint && direction == kDirIn && iso) {
        return handle_audio_in(seqnum, start_frame, iso_desc);
      }
      log(std::format("usbip: point de terminaison inattendu ep={} dir={} iso={}", ep, direction, iso));
      return send_bytes(ret_submit(seqnum, -EPIPE, 0, 0, npackets, {}, {}));
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
      if (verbose() || r.status) {
        log(std::format(
          "ctrl: {:02x} {:02x} wValue={:04x} wIndex={:04x} wLength={} -> {} {}",
          setup.bmRequestType,
          setup.bRequest,
          setup.wValue,
          setup.wIndex,
          setup.wLength,
          r.status ? "STALL" : "ok",
          direction == kDirIn ? r.data.size() : out_data.size()
        ));
      }
      const std::int32_t actual = r.status ? 0 : static_cast<std::int32_t>(direction == kDirIn ? r.data.size() : out_data.size());
      return send_bytes(ret_submit(
        seqnum,
        r.status,
        actual,
        0,
        0,
        direction == kDirIn ? std::span<const std::uint8_t>(r.data) : std::span<const std::uint8_t> {},
        {}
      ));
    }

    // Due time of an isochronous reply: the URB completes when its packets
    // have been "played", at 1 ms per packet, after the previous URB of the
    // same direction.
    Clock::time_point schedule(Clock::time_point &anchor, std::size_t npackets) {
      const auto now = Clock::now();
      if (anchor < now - std::chrono::milliseconds(20)) {
        anchor = now;  // start or resume of the stream
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
      // Microphone: silence, 48 stereo frames per 1 ms packet. IN data is sent
      // packed (usbip_pad_iso on the vhci side puts it back in place).
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
      if (verbose()) {
        log(std::format("usbip: UNLINK {} -> {}", victim, found ? "annulé" : "déjà terminé"));
      }
      Header r {};
      r.base.command = be32(kRetUnlink);
      r.base.seqnum = be32(seqnum);
      r.u.ret_unlink.status = be32s(found ? -ECONNRESET : 0);
      std::vector<std::uint8_t> bytes(sizeof(r));
      std::memcpy(bytes.data(), &r, sizeof(r));
      return send_bytes(bytes);
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
          send_bytes(bytes);
          lock.lock();
          continue;
        }
        if (!hid_pending_.empty() && next_hid <= now) {
          const auto p = hid_pending_.front();
          hid_pending_.pop_front();
          lock.unlock();
          const auto report = device_.current_input_report();
          const auto n = std::min<std::size_t>(report.size(), static_cast<std::size_t>(p.length));
          send_bytes(ret_submit(p.seqnum, 0, static_cast<std::int32_t>(n), 0, 0, std::span(report).first(n), {}));
          ++hid_in_count_;
          lock.lock();
          // Keeps the phase when the delay is small; after a gap, restarts
          // from now (never two reports back to back).
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
    Callbacks callbacks_;
    LogFn log_;
    std::function<bool()> verbose_;

    bool started_ = false;
    bool fd_closed_ = false;
    std::atomic<bool> stop_requested_ {false};
    std::thread reader_;
    std::thread timer_;

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

  UsbipSession::UsbipSession(int fd, DualSenseDevice &device, Callbacks callbacks, LogFn log):
      impl_(std::make_unique<Impl>(fd, device, std::move(callbacks), std::move(log))) {}

  UsbipSession::~UsbipSession() {
    impl_->stop();
  }

  void UsbipSession::set_verbose(std::function<bool()> verbose) {
    impl_->set_verbose(std::move(verbose));
  }

  bool UsbipSession::start() {
    return impl_->start();
  }

  void UsbipSession::stop() {
    impl_->stop();
  }

}  // namespace ds5usbip
