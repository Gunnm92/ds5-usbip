// SPDX-License-Identifier: MIT
// Serveur USB/IP exportant une DualSense émulée (un seul périphérique, busid
// "1-1"). Gère OP_REQ_DEVLIST / OP_REQ_IMPORT puis les URB : contrôle,
// interruption (HID) et isochrone (audio), avec UNLINK.
//
// Les réponses isochrones sont rythmées en temps réel (1 ms par paquet),
// comme dans l'export USB/IP Windows de vds (src/platform/win32/vds_usbip.cc) :
// sans cela snd-usb-audio, piloté par les fins d'URB, consommerait l'audio à
// la vitesse du réseau. Les rapports HID d'entrée partent toutes les 4 ms,
// comme une vraie DualSense en USB.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "dualsense_device.hpp"

namespace ds5usbip {

  class UsbipServer {
  public:
    struct Options {
      std::string bind_address = "0.0.0.0";
      std::uint16_t port = 3240;
      bool verbose = false;
      // Si fourni, remplace verbose et peut changer à chaud (SIGUSR1).
      const std::atomic<bool> *verbose_flag = nullptr;
    };

    struct Callbacks {
      // Un paquet isochrone de sortie : PCM S16_LE entrelacé, 4 canaux.
      std::function<void(std::span<const std::uint8_t> pcm)> audio_out;
      // Début (true) / fin (false) d'une session importée.
      std::function<void(bool attached)> session;
    };

    UsbipServer(DualSenseDevice &device, Options options, Callbacks callbacks = {});

    // Boucle d'acceptation, bloquante, jusqu'à ce que stop passe à true.
    // Renvoie 0, ou 1 si l'écoute est impossible.
    int run(const std::atomic<bool> &stop);

  private:
    void handle_connection(int fd, const std::atomic<bool> &stop);
    void run_session(int fd, const std::atomic<bool> &stop);

    bool verbose() const;

    DualSenseDevice &device_;
    Options options_;
    Callbacks callbacks_;
  };

}  // namespace ds5usbip
