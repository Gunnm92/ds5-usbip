// SPDX-License-Identifier: MIT
// Modèle de périphérique USB d'une DualSense : requêtes de contrôle standard,
// HID et UAC1, état des interfaces, rapports d'entrée. Indépendant du
// transport (USB/IP ici). Port en C++ de module/vds_usb_core.c de vds,
// Copyright (C) 2026 Jihong Min, licence MIT.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

#include "dualsense_descriptors.hpp"

namespace ds5usbip {

  struct SetupPacket {
    std::uint8_t bmRequestType;
    std::uint8_t bRequest;
    std::uint16_t wValue;
    std::uint16_t wIndex;
    std::uint16_t wLength;

    static SetupPacket parse(const std::uint8_t raw[8]);
  };

  struct ControlResult {
    int status = 0;  // 0 ou -errno (-EPIPE = STALL)
    std::vector<std::uint8_t> data;  // réponse d'une requête IN
  };

  class DualSenseDevice {
  public:
    using Report = std::array<std::uint8_t, dualsense::kHidReportSize>;

    struct Callbacks {
      // Rapport de sortie (0x02 : vibration, gâchettes, LED…), par
      // l'interruption OUT ou par SET_REPORT(output).
      std::function<void(std::span<const std::uint8_t>)> output_report;
      // SET_REPORT(feature) : octet 0 = numéro de rapport.
      std::function<void(std::span<const std::uint8_t>)> feature_set;
      // SET_INTERFACE sur une interface audio (alt 0 = flux arrêté).
      std::function<void(std::uint8_t interface, std::uint8_t alt)> interface_changed;
    };

    explicit DualSenseDevice(Callbacks callbacks = {});

    ControlResult control(const SetupPacket &setup, std::span<const std::uint8_t> out_data);

    // Rapport reçu sur le point de terminaison d'interruption OUT.
    void interrupt_out(std::span<const std::uint8_t> report);

    // Rapport d'entrée courant (0x01). Le numéro de séquence est incrémenté
    // à chaque lecture par next_input_report().
    void set_input_report(const Report &report);
    Report next_input_report();

    void reset();

    bool audio_out_active() const;

  private:
    ControlResult standard_request(const SetupPacket &setup);
    ControlResult hid_request(const SetupPacket &setup, std::span<const std::uint8_t> out_data);
    ControlResult audio_request(const SetupPacket &setup, std::span<const std::uint8_t> out_data);

    static Report neutral_input_report();

    Callbacks callbacks_;
    mutable std::mutex mutex_;
    std::uint8_t configuration_ = 0;
    std::uint8_t audio_out_alt_ = 0;
    std::uint8_t audio_in_alt_ = 0;
    std::uint8_t hid_idle_ = 0;
    std::uint8_t hid_protocol_ = 1;
    std::array<std::uint8_t, 2> audio_mute_ {};
    std::array<std::int16_t, 2> audio_volume_ {};
    Report input_report_;
    std::uint8_t input_seq_ = 0;
  };

}  // namespace ds5usbip
