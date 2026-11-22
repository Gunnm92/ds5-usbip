// SPDX-License-Identifier: MIT
#include "haptics_rtp.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace ds5usbip {

  HapticsRtpSender::~HapticsRtpSender() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  bool HapticsRtpSender::open(const std::string &destination) {
    const auto colon = destination.rfind(':');
    if (colon == std::string::npos) {
      std::fprintf(stderr, "haptiques : destination attendue IP:PORT, reçu « %s »\n", destination.c_str());
      return false;
    }
    dest_.sin_family = AF_INET;
    dest_.sin_port = htons(static_cast<std::uint16_t>(std::atoi(destination.c_str() + colon + 1)));
    if (::inet_pton(AF_INET, destination.substr(0, colon).c_str(), &dest_.sin_addr) != 1 || dest_.sin_port == 0) {
      std::fprintf(stderr, "haptiques : destination invalide « %s »\n", destination.c_str());
      return false;
    }
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
      std::fprintf(stderr, "haptiques : socket : %s\n", std::strerror(errno));
      return false;
    }
    // Priorité réseau élevée (DSCP EF), comme l'audio temps réel.
    const int tos = 0xb8;
    ::setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    std::random_device rd;
    ssrc_ = rd();
    seq_ = static_cast<std::uint16_t>(rd());
    timestamp_ = rd();
    std::fprintf(stderr, "haptiques : RTP L16 stéréo 48 kHz vers %s (PT %u)\n", destination.c_str(), kPayloadType);
    return true;
  }

  void HapticsRtpSender::set_gain(float gain, std::string gain_file) {
    gain_ = gain > 0 ? gain : 1.0f;
    gain_file_ = std::move(gain_file);
    reload_gain();
    std::fprintf(stderr, "haptiques : gain ×%.2f%s%s\n", gain_, gain_file_.empty() ? "" : ", réglable dans ", gain_file_.c_str());
  }

  void HapticsRtpSender::reload_gain() {
    if (gain_file_.empty()) {
      return;
    }
    std::FILE *f = std::fopen(gain_file_.c_str(), "r");
    if (!f) {
      return;
    }
    float g = 0;
    if (std::fscanf(f, "%f", &g) == 1 && g > 0 && g <= 64 && g != gain_) {
      gain_ = g;
      std::fprintf(stderr, "haptiques : gain ×%.2f\n", gain_);
    }
    std::fclose(f);
  }

  // Gain puis écrêtage doux : linéaire jusqu'à la moitié de la pleine
  // échelle, puis courbe tanh qui tend vers la pleine échelle sans cassure.
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

  void HapticsRtpSender::send(std::span<const std::uint8_t> pcm4ch) {
    if (fd_ < 0) {
      return;
    }
    constexpr std::size_t kInFrame = 8;  // 4 canaux × 2 octets
    const std::size_t frames = pcm4ch.size() / kInFrame;
    if (!frames) {
      return;
    }
    std::vector<std::uint8_t> pkt(12 + frames * 4);
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
    // Canaux 2-3, S16_LE → gain → S16_BE.
    for (std::size_t f = 0; f < frames; ++f) {
      const std::uint8_t *in = pcm4ch.data() + f * kInFrame + 4;
      std::uint8_t *out = pkt.data() + 12 + f * 4;
      for (int c = 0; c < 2; ++c) {
        const auto v = static_cast<std::uint16_t>(shape(static_cast<std::int16_t>(in[2 * c] | (in[2 * c + 1] << 8)), gain_));
        out[2 * c] = static_cast<std::uint8_t>(v >> 8);
        out[2 * c + 1] = static_cast<std::uint8_t>(v);
      }
    }
    ::sendto(fd_, pkt.data(), pkt.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr *>(&dest_), sizeof(dest_));
    ++seq_;
    timestamp_ += static_cast<std::uint32_t>(frames);
    ++packets_;
  }

}  // namespace ds5usbip
