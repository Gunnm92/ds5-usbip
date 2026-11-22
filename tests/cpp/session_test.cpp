// SPDX-License-Identifier: MIT
// C++ tests of the library (no framework). A socketpair plays the vhci-hcd
// kernel on one end; UsbipSession runs on the other. Returns non-zero on failure.
//
// Build and run: make test
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "usbip/dualsense_device.hpp"
#include "usbip/haptics_rtp.hpp"
#include "usbip/usbip_protocol.hpp"
#include "usbip/usbip_session.hpp"
#include "usbip/vhci.hpp"

namespace {

  using namespace ds5usbip;
  using namespace ds5usbip::usbip;
  using Clock = std::chrono::steady_clock;
  using Bytes = std::vector<std::uint8_t>;

  int g_failures = 0;

  void check(bool cond, const std::string &what) {
    std::printf("%s %s\n", cond ? "ok   " : "ECHEC", what.c_str());
    if (!cond) {
      ++g_failures;
    }
  }

  double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  }

  bool recv_exact(int fd, void *buf, std::size_t len) {
    auto *p = static_cast<std::uint8_t *>(buf);
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

  bool send_all(int fd, const void *buf, std::size_t len) {
    const auto *p = static_cast<const std::uint8_t *>(buf);
    while (len) {
      const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
      if (n <= 0) {
        return false;
      }
      p += n;
      len -= static_cast<std::size_t>(n);
    }
    return true;
  }

  // Setup packet in USB byte order (little-endian fields).
  std::array<std::uint8_t, 8> setup_bytes(std::uint8_t type, std::uint8_t request, std::uint16_t value, std::uint16_t index, std::uint16_t length) {
    return {
      type,
      request,
      static_cast<std::uint8_t>(value & 0xff),
      static_cast<std::uint8_t>(value >> 8),
      static_cast<std::uint8_t>(index & 0xff),
      static_cast<std::uint8_t>(index >> 8),
      static_cast<std::uint8_t>(length & 0xff),
      static_cast<std::uint8_t>(length >> 8),
    };
  }

  // Sends one CMD_SUBMIT: header, OUT payload, then isochronous descriptors.
  void submit(int fd, std::uint32_t seqnum, std::uint32_t ep, std::uint32_t direction, std::int32_t length, const std::array<std::uint8_t, 8> &setup, std::span<const std::uint8_t> out, std::span<const IsoPacketDescriptor> iso = {}) {
    Header h {};
    h.base.command = be32(kCmdSubmit);
    h.base.seqnum = be32(seqnum);
    h.base.devid = be32(0x10001);
    h.base.direction = be32(direction);
    h.base.ep = be32(ep);
    h.u.cmd_submit.transfer_flags = 0;
    h.u.cmd_submit.transfer_buffer_length = be32s(length);
    h.u.cmd_submit.start_frame = 0;
    h.u.cmd_submit.number_of_packets = be32s(static_cast<std::int32_t>(iso.size()));
    h.u.cmd_submit.interval = 0;
    std::memcpy(h.u.cmd_submit.setup, setup.data(), 8);
    Bytes wire(sizeof(h));
    std::memcpy(wire.data(), &h, sizeof(h));
    wire.insert(wire.end(), out.begin(), out.end());
    for (const auto &d : iso) {
      const IsoPacketDescriptor n {be32(d.offset), be32(d.length), 0, 0};
      const auto *b = reinterpret_cast<const std::uint8_t *>(&n);
      wire.insert(wire.end(), b, b + sizeof(n));
    }
    send_all(fd, wire.data(), wire.size());
  }

  struct Reply {
    std::uint32_t seqnum = 0;
    std::int32_t status = 0;
    std::int32_t actual = 0;
    std::int32_t npackets = 0;
    Bytes data;
    std::vector<IsoPacketDescriptor> iso;
    bool ok = false;
  };

  // Reads one RET_SUBMIT. For IN requests the data follows the header; the
  // isochronous descriptors follow when present.
  Reply read_reply(int fd, bool in) {
    Reply r;
    Header h {};
    if (!recv_exact(fd, &h, sizeof(h)) || ntohl(h.base.command) != kRetSubmit) {
      return r;
    }
    r.seqnum = ntohl(h.base.seqnum);
    r.status = static_cast<std::int32_t>(ntohl(static_cast<std::uint32_t>(h.u.ret_submit.status)));
    r.actual = static_cast<std::int32_t>(ntohl(static_cast<std::uint32_t>(h.u.ret_submit.actual_length)));
    r.npackets = static_cast<std::int32_t>(ntohl(static_cast<std::uint32_t>(h.u.ret_submit.number_of_packets)));
    if (in && r.actual > 0) {
      r.data.resize(static_cast<std::size_t>(r.actual));
      if (!recv_exact(fd, r.data.data(), r.data.size())) {
        return r;
      }
    }
    if (r.npackets > 0) {
      r.iso.resize(static_cast<std::size_t>(r.npackets));
      if (!recv_exact(fd, r.iso.data(), r.iso.size() * sizeof(IsoPacketDescriptor))) {
        return r;
      }
      for (auto &d : r.iso) {
        d.offset = ntohl(d.offset);
        d.length = ntohl(d.length);
        d.actual_length = ntohl(d.actual_length);
      }
    }
    r.ok = true;
    return r;
  }

  // Session on one end of a socketpair, the test on the other end.
  class Rig {
  public:
    explicit Rig(DeviceConfig config = {}) {
      int sv[2] {-1, -1};
      if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return;
      }
      peer_ = sv[1];
      const timeval tv {2, 0};  // a stuck test must fail, not hang
      ::setsockopt(peer_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      DualSenseDevice::Callbacks dcb;
      dcb.output_report = [this](std::span<const std::uint8_t> r) {
        std::lock_guard lock(mutex_);
        output_reports_.emplace_back(r.begin(), r.end());
      };
      dcb.feature_set = [this](std::span<const std::uint8_t> r) {
        std::lock_guard lock(mutex_);
        feature_sets_.emplace_back(r.begin(), r.end());
      };
      device_ = std::make_unique<DualSenseDevice>(std::move(config), std::move(dcb));

      UsbipSession::Callbacks cb;
      cb.audio_out = [this](std::span<const std::uint8_t> pcm) {
        std::lock_guard lock(mutex_);
        audio_sizes_.push_back(pcm.size());
      };
      cb.closed = [this] {
        closed_.store(true);
      };
      session_ = std::make_unique<UsbipSession>(sv[0], *device_, std::move(cb));
      started_ = session_->start();
    }

    ~Rig() {
      session_.reset();  // stop() closes the session end
      if (peer_ >= 0) {
        ::close(peer_);
      }
    }

    Rig(const Rig &) = delete;
    Rig &operator=(const Rig &) = delete;

    bool started() const {
      return started_;
    }
    int peer() const {
      return peer_;
    }
    void close_peer() {
      ::close(peer_);
      peer_ = -1;
    }
    DualSenseDevice &device() {
      return *device_;
    }
    UsbipSession &session() {
      return *session_;
    }
    bool closed() const {
      return closed_.load();
    }

    Reply control(std::uint8_t type, std::uint8_t request, std::uint16_t value, std::uint16_t index, std::uint16_t length, std::span<const std::uint8_t> out = {}) {
      const bool in = type & 0x80;
      submit(peer_, next_seq(), 0, in ? 1 : 0, length, setup_bytes(type, request, value, index, length), out);
      return read_reply(peer_, in);
    }

    std::uint32_t next_seq() {
      return ++seq_;
    }

    std::vector<Bytes> output_reports() {
      std::lock_guard lock(mutex_);
      return output_reports_;
    }
    std::vector<Bytes> feature_sets() {
      std::lock_guard lock(mutex_);
      return feature_sets_;
    }
    std::vector<std::size_t> audio_sizes() {
      std::lock_guard lock(mutex_);
      return audio_sizes_;
    }

  private:
    int peer_ = -1;
    std::unique_ptr<DualSenseDevice> device_;
    std::unique_ptr<UsbipSession> session_;
    bool started_ = false;
    std::uint32_t seq_ = 0;
    std::atomic<bool> closed_ {false};
    std::mutex mutex_;
    std::vector<Bytes> output_reports_;
    std::vector<Bytes> feature_sets_;
    std::vector<std::size_t> audio_sizes_;
  };

  // Finds the HID descriptor (09 21 ...) embedded in a configuration descriptor.
  std::optional<std::uint16_t> embedded_hid_length(const Bytes &config) {
    for (std::size_t i = 0; i + 9 <= config.size(); ++i) {
      if (config[i] == 0x09 && config[i + 1] == 0x21) {
        return static_cast<std::uint16_t>(config[i + 7] | (config[i + 8] << 8));
      }
    }
    return std::nullopt;
  }

  // 1. Descriptors: wTotalLength 227, HID descriptor length equals the report descriptor size.
  void test_descriptors() {
    {
      Rig rig;
      const auto cfg = rig.control(0x80, 6, 0x0200, 0, 255);
      const std::uint16_t total = cfg.data.size() >= 4 ? static_cast<std::uint16_t>(cfg.data[2] | (cfg.data[3] << 8)) : 0;
      check(cfg.ok && cfg.status == 0 && cfg.data.size() == 227 && total == 227, "config descriptor: wTotalLength 227");
      const auto hid_len = embedded_hid_length(cfg.data);
      check(hid_len && *hid_len == 289, "config descriptor: embedded HID length 289 (default)");
      const auto rep = rig.control(0x81, 6, 0x2200, 3, 0x200);
      check(rep.ok && rep.data.size() == 289, "HID report descriptor: 289 bytes (default)");
      const auto hid = rig.control(0x81, 6, 0x2100, 3, 0x200);
      const std::uint16_t hid_standalone = hid.data.size() >= 9 ? static_cast<std::uint16_t>(hid.data[7] | (hid.data[8] << 8)) : 0;
      check(hid.ok && hid.data.size() == 9 && hid_standalone == 289, "standalone HID descriptor: wDescriptorLength 289");
    }
    {
      Bytes custom(273);
      for (std::size_t i = 0; i < custom.size(); ++i) {
        custom[i] = static_cast<std::uint8_t>(i);
      }
      DeviceConfig cfg;
      cfg.hid_report_descriptor = custom;
      Rig rig(std::move(cfg));
      const auto c = rig.control(0x80, 6, 0x0200, 0, 255);
      const auto hid_len = embedded_hid_length(c.data);
      check(hid_len && *hid_len == 273, "config descriptor: embedded HID length 273 (custom)");
      const auto rep = rig.control(0x81, 6, 0x2200, 3, 0x200);
      check(rep.ok && rep.data == custom, "HID report descriptor: custom 273 bytes returned unchanged");
      const auto hid = rig.control(0x81, 6, 0x2100, 3, 0x200);
      const std::uint16_t hid_standalone = hid.data.size() >= 9 ? static_cast<std::uint16_t>(hid.data[7] | (hid.data[8] << 8)) : 0;
      check(hid_standalone == 273, "standalone HID descriptor: wDescriptorLength 273 (custom)");
    }
  }

  // 2. SET_CONFIGURATION 1 -> configured().
  void test_configuration() {
    Rig rig;
    check(!rig.device().configured(), "not configured before SET_CONFIGURATION");
    const auto r = rig.control(0x00, 9, 1, 0, 0);
    check(r.ok && r.status == 0 && rig.device().configured(), "SET_CONFIGURATION 1 -> configured()");
  }

  // 3. HID IN: a 64-byte report 0x01, first one quickly; provider report when set.
  void test_hid_in() {
    {
      Rig rig;
      const auto t0 = Clock::now();
      submit(rig.peer(), rig.next_seq(), 4, 1, 64, setup_bytes(0, 0, 0, 0, 0), {});
      const auto r = read_reply(rig.peer(), true);
      const double dt = ms_since(t0);
      check(r.ok && r.status == 0 && r.data.size() == 64 && r.data[0] == 0x01, "HID IN: 64-byte report 0x01");
      check(dt < 25.0, "HID IN: first report within ~10 ms (measured " + std::to_string(static_cast<int>(dt)) + " ms)");
    }
    {
      DeviceConfig cfg;
      cfg.input_provider = [] {
        std::array<std::uint8_t, 64> r {};
        r[0] = 0x01;
        r[20] = 0xab;
        r[21] = 0xcd;
        return r;
      };
      Rig rig(std::move(cfg));
      submit(rig.peer(), rig.next_seq(), 4, 1, 64, setup_bytes(0, 0, 0, 0, 0), {});
      const auto r = read_reply(rig.peer(), true);
      check(r.ok && r.data.size() == 64 && r.data[20] == 0xab && r.data[21] == 0xcd, "HID IN: input_provider report returned");
    }
  }

  // 4. HID OUT: 48-byte report 0x02 reaches output_report.
  void test_hid_out() {
    Rig rig;
    Bytes out(48);
    out[0] = 0x02;
    for (std::size_t i = 1; i < out.size(); ++i) {
      out[i] = static_cast<std::uint8_t>(i);
    }
    submit(rig.peer(), rig.next_seq(), 3, 0, 48, setup_bytes(0, 0, 0, 0, 0), out);
    const auto r = read_reply(rig.peer(), false);
    const auto reports = rig.output_reports();
    check(r.ok && r.status == 0 && r.actual == 48, "HID OUT: acknowledged with 48 bytes");
    check(reports.size() == 1 && reports[0] == out, "HID OUT: output_report receives the 48-byte report");
  }

  // 5. SET_REPORT feature -> feature_set.
  void test_feature_set() {
    Rig rig;
    const Bytes data {0x05, 0x01, 0x02};
    const auto r = rig.control(0x21, 9, 0x0305, 3, 3, data);
    const auto sets = rig.feature_sets();
    check(r.ok && r.status == 0, "SET_REPORT feature: acknowledged");
    check(sets.size() == 1 && sets[0] == data, "SET_REPORT feature: feature_set called with the payload");
  }

  // 6. Isochronous OUT: 8 packets of 384 bytes -> 8 audio_out calls of 384 bytes.
  void test_iso_out() {
    Rig rig;
    Bytes data(8 * 384);
    std::vector<IsoPacketDescriptor> iso;
    for (std::uint32_t i = 0; i < 8; ++i) {
      iso.push_back({i * 384, 384, 0, 0});
    }
    submit(rig.peer(), rig.next_seq(), 1, 0, static_cast<std::int32_t>(data.size()), setup_bytes(0, 0, 0, 0, 0), data, iso);
    const auto r = read_reply(rig.peer(), false);
    const auto sizes = rig.audio_sizes();
    bool each_384 = sizes.size() == 8;
    for (auto s : sizes) {
      each_384 = each_384 && s == 384;
    }
    check(r.ok && r.status == 0 && r.actual == 8 * 384 && r.npackets == 8, "isochronous OUT: RET_SUBMIT with 8 packets");
    check(each_384, "isochronous OUT: audio_out called 8 times with 384 bytes");
  }

  // 7a. stop() returns quickly while the reader is blocked; closed() is not reported by our own stop.
  void test_stop_latency() {
    Rig rig;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // reader is now blocked in recv()
    const auto t0 = Clock::now();
    rig.session().stop();
    const double dt = ms_since(t0);
    check(dt < 100.0, "stop() returns in < 100 ms (measured " + std::to_string(static_cast<int>(dt)) + " ms)");
    check(!rig.closed(), "closed callback not fired by our own stop()");
    rig.session().stop();  // idempotent
    check(true, "stop() is idempotent");
  }

  // 7b. closed() fires when the peer closes its end.
  void test_closed_on_peer_close() {
    Rig rig;
    rig.close_peer();
    const auto t0 = Clock::now();
    while (!rig.closed() && ms_since(t0) < 1000.0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(rig.closed(), "closed callback fires when the peer closes");
  }

  // 8. vhci helpers: status parsing, free port selection, attach line.
  void test_vhci() {
    const std::string text =
      "hub port sta spd dev      sockfd local_busid\n"
      "hs  0000 006 003 00010001 000003 3-1\n"
      "hs  0001 004 000 00000000 000000 0-0\n"
      "ss  0008 004 000 00000000 000000 0-0\n";
    const auto ports = vhci::parse_status(text);
    check(ports.size() == 3, "parse_status: three ports, header ignored");
    check(ports.size() == 3 && ports[0].hub == "hs" && ports[0].port == 0 && ports[0].status == 6, "parse_status: first port fields");
    const auto free = vhci::pick_free_port(ports);
    check(free && *free == 1, "pick_free_port: port 1 (port 0 used, ss ignored)");
    const std::array<int, 1> skip {1};
    check(!vhci::pick_free_port(ports, skip), "pick_free_port: nothing left when 1 is skipped");
    check(vhci::attach_line(1, 7, 65537, 3) == "1 7 65537 3", "attach_line: \"1 7 65537 3\"");
  }

  void test_gain_file_reload() {
    const std::string path = "/tmp/ds5-gain-reload-test-" + std::to_string(::getpid());
    check(write_text_atomically(path, "channels=4\ngain=1.5\n"), "gain reload: settings-style file written");
    HapticsRtpSender sender;
    check(sender.set_destination("127.0.0.1:46199"), "gain reload: destination accepted");
    sender.set_gain(1.0F, path);
    check(sender.gain() == 1.5F, "gain reload: gain= line read by set_gain");

    check(write_text_atomically(path, "3\n"), "gain reload: plain number written");
    const Bytes pcm(384, 0);
    sender.send(pcm);  // the first send reloads the file: the last check is the epoch
    check(sender.gain() == 3.0F, "gain reload: plain number read on send");
    check(sender.packets() == 1, "gain reload: one RTP packet counted");

    check(write_text_atomically(path, "gain=2\n"), "gain reload: gain= line written");
    sender.send(pcm);  // within the same second: no reload yet
    check(sender.gain() == 3.0F, "gain reload: no reload before one second");
    static_cast<void>(std::remove(path.c_str()));
  }

  void test_haptics_settings() {
    const auto both = parse_haptics_settings("# DS5 haptics\n gain = 2.5 \nchannels=4 # speakers too\n");
    check(both.gain && *both.gain == 2.5F, "settings: gain=2.5 with spaces and a trailing comment");
    check(both.channels && *both.channels == 4U, "settings: channels=4");

    const auto two = parse_haptics_settings("channels=2\r\ngain=64\r\n");
    check(two.channels && *two.channels == 2U, "settings: channels=2 with CRLF");
    check(two.gain && *two.gain == 64.0F, "settings: gain=64 is the upper bound");

    check(!parse_haptics_settings("gain=0\n").gain, "settings: gain=0 ignored");
    check(!parse_haptics_settings("gain=65\n").gain, "settings: gain=65 ignored");
    check(!parse_haptics_settings("gain=2.5x\n").gain, "settings: gain with trailing text ignored");
    check(!parse_haptics_settings("channels=3\n").channels, "settings: channels=3 ignored");
    check(!parse_haptics_settings("# gain=3\nflavour=sweet\njust text\n").gain, "settings: comments and unknown keys ignored");
    const auto later = parse_haptics_settings("gain=2\ngain=bad\n");
    check(later.gain && *later.gain == 2.0F, "settings: an invalid later line keeps the earlier value");

    check(parse_gain_text("2.5\n") && *parse_gain_text("2.5\n") == 2.5F, "gain text: plain number");
    check(parse_gain_text("gain=3\n") && *parse_gain_text("gain=3\n") == 3.0F, "gain text: gain= line");
    check(parse_gain_text("channels=4\ngain=0.5\n") && *parse_gain_text("channels=4\ngain=0.5\n") == 0.5F, "gain text: gain= among other lines");
    check(!parse_gain_text("-1\n"), "gain text: negative rejected");
    check(!parse_gain_text("abc\n"), "gain text: text rejected");
    check(!parse_gain_text(""), "gain text: empty rejected");

    const std::string path = "/tmp/ds5-haptics-settings-test-" + std::to_string(::getpid());
    check(write_text_atomically(path, "gain=1.5\nchannels=4\n"), "atomic write: succeeds");
    const auto loaded = load_haptics_settings(path);
    check(loaded.gain && *loaded.gain == 1.5F && loaded.channels && *loaded.channels == 4U, "load_haptics_settings: reads the file");
    check(!write_text_atomically("/nonexistent-dir/x", "x"), "atomic write: fails on a missing directory");
    static_cast<void>(std::remove(path.c_str()));
    check(!load_haptics_settings(path).gain, "load_haptics_settings: missing file gives empty settings");
  }

}  // namespace

int main() {
  test_descriptors();
  test_configuration();
  test_hid_in();
  test_hid_out();
  test_feature_set();
  test_iso_out();
  test_stop_latency();
  test_closed_on_peer_close();
  test_vhci();
  test_haptics_settings();
  test_gain_file_reload();
  std::printf("\n%d echec(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
