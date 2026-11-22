// SPDX-License-Identifier: MIT
// Envoi des canaux haptiques (3-4) de la DualSense émulée en RTP, vers le
// PC client : L16 stéréo big-endian, 48 kHz, un paquet par paquet
// isochrone (1 ms, 48 trames). Format lu tel quel par module-rtp-source de
// PipeWire (audio.format S16BE, audio.channels 2, sans annonce SAP).
#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <span>
#include <string>

namespace ds5usbip {

  class HapticsRtpSender {
  public:
    // Type de charge utile : 127, valeur par défaut des modules RTP de
    // PipeWire pour l'audio brut.
    static constexpr std::uint8_t kPayloadType = 127;

    ~HapticsRtpSender();

    // destination « IP:PORT » ; false si invalide ou socket impossible.
    bool open(const std::string &destination);

    // Un paquet isochrone de la sortie audio : S16_LE, 4 canaux entrelacés.
    // N'envoie que les canaux 2 et 3 (haptiques gauche et droite).
    void send(std::span<const std::uint8_t> pcm4ch);

    std::uint64_t packets() const {
      return packets_;
    }

  private:
    int fd_ = -1;
    sockaddr_in dest_ {};
    std::uint16_t seq_ = 0;
    std::uint32_t timestamp_ = 0;
    std::uint32_t ssrc_ = 0;
    std::uint64_t packets_ = 0;
  };

}  // namespace ds5usbip
