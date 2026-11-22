// SPDX-License-Identifier: MIT
#include "haptics_rtp.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
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
    // Canaux 2-3, S16_LE → S16_BE.
    for (std::size_t f = 0; f < frames; ++f) {
      const std::uint8_t *in = pcm4ch.data() + f * kInFrame + 4;
      std::uint8_t *out = pkt.data() + 12 + f * 4;
      out[0] = in[1];
      out[1] = in[0];
      out[2] = in[3];
      out[3] = in[2];
    }
    ::sendto(fd_, pkt.data(), pkt.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr *>(&dest_), sizeof(dest_));
    ++seq_;
    timestamp_ += static_cast<std::uint32_t>(frames);
    ++packets_;
  }

}  // namespace ds5usbip
