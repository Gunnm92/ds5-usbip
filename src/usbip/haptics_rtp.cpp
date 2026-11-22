// SPDX-License-Identifier: MIT
#include "haptics_rtp.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace ds5usbip {

  namespace {

    constexpr float kMaxGain = 64.0F;

    /** @brief Removes leading and trailing spaces, tabs and line breaks. */
    std::string_view trim(std::string_view text) {
      constexpr std::string_view spaces = " \t\r\n";
      const auto first = text.find_first_not_of(spaces);
      if (first == std::string_view::npos) {
        return {};
      }
      const auto last = text.find_last_not_of(spaces);
      return text.substr(first, last - first + 1);
    }

    /** @brief Parses a whole string as a float; nullopt on trailing text or a non-number. */
    std::optional<float> parse_number(std::string_view text) {
      const std::string copy {text};
      char *end = nullptr;
      const float value = std::strtof(copy.c_str(), &end);
      if (copy.empty() || end != copy.c_str() + copy.size()) {
        return std::nullopt;
      }
      return value;
    }

    /** @brief Gain range accepted in settings and gain files: 0 < g <= 64. */
    bool valid_gain(float gain) {
      return gain > 0.0F && gain <= kMaxGain;
    }

  }  // namespace

  HapticsSettings parse_haptics_settings(std::string_view text) {
    HapticsSettings settings;
    while (!text.empty()) {
      const auto newline = text.find('\n');
      auto line = text.substr(0, newline);
      text = newline == std::string_view::npos ? std::string_view {} : text.substr(newline + 1);

      line = line.substr(0, line.find('#'));
      const auto equal = line.find('=');
      if (equal == std::string_view::npos) {
        continue;
      }
      const auto key = trim(line.substr(0, equal));
      const auto value = trim(line.substr(equal + 1));
      if (key == "gain") {
        if (const auto gain = parse_number(value); gain && valid_gain(*gain)) {
          settings.gain = gain;
        }
      } else if (key == "channels") {
        if (value == "2") {
          settings.channels = 2U;
        } else if (value == "4") {
          settings.channels = 4U;
        }
      }
    }
    return settings;
  }

  HapticsSettings load_haptics_settings(const std::string &path) {
    std::FILE *f = std::fopen(path.c_str(), "r");
    if (!f) {
      return {};
    }
    std::string text;
    std::array<char, 512> buffer {};
    std::size_t count = 0;
    while ((count = std::fread(buffer.data(), 1, buffer.size(), f)) > 0) {
      text.append(buffer.data(), count);
    }
    std::fclose(f);
    return parse_haptics_settings(text);
  }

  std::optional<float> parse_gain_text(std::string_view text) {
    while (!text.empty()) {
      const auto newline = text.find('\n');
      auto line = trim(text.substr(0, newline));
      text = newline == std::string_view::npos ? std::string_view {} : text.substr(newline + 1);

      line = trim(line.substr(0, line.find('#')));
      if (line.empty()) {
        continue;
      }
      std::optional<float> gain;
      if (const auto equal = line.find('='); equal == std::string_view::npos) {
        gain = parse_number(line);
      } else if (trim(line.substr(0, equal)) == "gain") {
        gain = parse_number(trim(line.substr(equal + 1)));
      }
      if (gain && valid_gain(*gain)) {
        return gain;
      }
    }
    return std::nullopt;
  }

  bool write_text_atomically(const std::string &path, std::string_view text) {
    const auto temporary = path + ".tmp";
    std::FILE *f = std::fopen(temporary.c_str(), "w");
    if (!f) {
      return false;
    }
    const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (!written || !closed) {
      static_cast<void>(std::remove(temporary.c_str()));
      return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
      static_cast<void>(std::remove(temporary.c_str()));
      return false;
    }
    return true;
  }

  HapticsRtpSender::HapticsRtpSender(LogFn log):
      log_(std::move(log)) {}

  HapticsRtpSender::~HapticsRtpSender() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  void HapticsRtpSender::log(const std::string &msg) const {
    if (log_) {
      log_(msg);
    }
  }

  bool HapticsRtpSender::set_destination(const std::string &ip_port) {
    const auto colon = ip_port.rfind(':');
    if (colon == std::string::npos) {
      log(std::format("haptiques : destination attendue IP:PORT, reçu « {} »", ip_port));
      return false;
    }
    sockaddr_in dest {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(static_cast<std::uint16_t>(std::atoi(ip_port.c_str() + colon + 1)));
    if (::inet_pton(AF_INET, ip_port.substr(0, colon).c_str(), &dest.sin_addr) != 1 || dest.sin_port == 0) {
      log(std::format("haptiques : destination invalide « {} »", ip_port));
      return false;
    }
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      log(std::format("haptiques : socket : {}", std::strerror(errno)));
      return false;
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
    dest_ = dest;
    // High network priority (DSCP EF), like real-time audio.
    const int tos = 0xb8;
    ::setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    std::random_device rd;
    ssrc_ = rd();
    seq_ = static_cast<std::uint16_t>(rd());
    timestamp_ = rd();
    log(std::format("haptiques : RTP L16 stéréo 48 kHz vers {} (PT {})", ip_port, kPayloadType));
    return true;
  }

  void HapticsRtpSender::set_gain(float gain, std::string gain_file) {
    gain_.store(gain > 0 ? gain : 1.0f, std::memory_order_relaxed);
    gain_file_ = std::move(gain_file);
    reload_gain();
    log(std::format("haptiques : gain ×{:.2f}{}{}", gain_.load(std::memory_order_relaxed), gain_file_.empty() ? "" : ", réglable dans ", gain_file_));
  }

  void HapticsRtpSender::reload_gain() {
    if (gain_file_.empty()) {
      return;
    }
    std::FILE *f = std::fopen(gain_file_.c_str(), "r");
    if (!f) {
      return;
    }
    std::string text;
    std::array<char, 256> buffer {};
    std::size_t count = 0;
    while ((count = std::fread(buffer.data(), 1, buffer.size(), f)) > 0) {
      text.append(buffer.data(), count);
    }
    std::fclose(f);
    const auto g = parse_gain_text(text);
    if (g && *g != gain_.load(std::memory_order_relaxed)) {
      gain_.store(*g, std::memory_order_relaxed);
      log(std::format("haptiques : gain ×{:.2f}", *g));
    }
  }

  // Gain, then soft clipping: linear up to half of full scale, then a tanh
  // curve that approaches full scale without a break.
  std::int16_t HapticsRtpSender::shape(std::int16_t sample, float gain) {
    if (gain == 1.0f) {
      return sample;
    }
    float x = sample / 32768.0f * gain;
    const float a = std::fabs(x);
    if (a > 0.5f) {
      const float y = 0.5f + 0.5f * std::tanh((a - 0.5f) / 0.5f);
      x = x < 0 ? -y : y;
    }
    return static_cast<std::int16_t>(std::lrint(std::clamp(x, -1.0f, 32767.0f / 32768.0f) * 32768.0f));
  }

  void HapticsRtpSender::set_channels(unsigned channels) {
    channels_ = channels == 4 ? 4 : 2;
  }

  void HapticsRtpSender::send(std::span<const std::uint8_t> pcm4ch) {
    if (fd_ < 0) {
      return;
    }
    constexpr std::size_t kInFrame = 8;  // 4 channels x 2 bytes
    const std::size_t frames = pcm4ch.size() / kInFrame;
    if (!frames) {
      return;
    }
    const std::size_t out_frame = channels_ * 2;
    std::vector<std::uint8_t> pkt(12 + frames * out_frame);
    pkt[0] = 0x80;  // version 2
    pkt[1] = kPayloadType;
    pkt[2] = static_cast<std::uint8_t>(seq_ >> 8);
    pkt[3] = static_cast<std::uint8_t>(seq_);
    for (int i = 0; i < 4; ++i) {
      pkt[4 + i] = static_cast<std::uint8_t>(timestamp_ >> (24 - 8 * i));
      pkt[8 + i] = static_cast<std::uint8_t>(ssrc_ >> (24 - 8 * i));
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_gain_check_ >= std::chrono::seconds(1)) {
      last_gain_check_ = now;
      reload_gain();
    }
    // S16_LE -> S16_BE. 2 channels: haptics (input channels 2-3) with gain.
    // 4 channels: speaker (0-1) unchanged, then haptics (2-3) with gain.
    const unsigned first = channels_ == 4 ? 0 : 2;
    const float gain = gain_.load(std::memory_order_relaxed);
    for (std::size_t f = 0; f < frames; ++f) {
      const std::uint8_t *in = pcm4ch.data() + f * kInFrame;
      std::uint8_t *out = pkt.data() + 12 + f * out_frame;
      for (unsigned c = first, o = 0; c < 4; ++c, ++o) {
        auto s = static_cast<std::int16_t>(in[2 * c] | (in[2 * c + 1] << 8));
        if (c >= 2) {
          s = shape(s, gain);
        }
        const auto v = static_cast<std::uint16_t>(s);
        out[2 * o] = static_cast<std::uint8_t>(v >> 8);
        out[2 * o + 1] = static_cast<std::uint8_t>(v);
      }
    }
    ::sendto(fd_, pkt.data(), pkt.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr *>(&dest_), sizeof(dest_));
    ++seq_;
    timestamp_ += static_cast<std::uint32_t>(frames);
    packets_.fetch_add(1, std::memory_order_relaxed);
  }

}  // namespace ds5usbip
