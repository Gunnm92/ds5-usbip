// SPDX-License-Identifier: MIT
// USB device model of a DualSense: standard, HID and UAC1 control requests,
// interface state, input reports. Independent of the transport (USB/IP here).
// Port to C++ of module/vds_usb_core.c from vds, Copyright (C) 2026 Jihong Min,
// MIT License.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "dualsense_descriptors.hpp"

namespace ds5usbip {

  /** @brief Parsed 8-byte USB SETUP packet. */
  struct SetupPacket {
    std::uint8_t bmRequestType;
    std::uint8_t bRequest;
    std::uint16_t wValue;
    std::uint16_t wIndex;
    std::uint16_t wLength;

    /** @brief Parses the 8 raw SETUP bytes (little-endian fields, USB format). */
    static SetupPacket parse(const std::uint8_t raw[8]);
  };

  /** @brief Outcome of a control request. */
  struct ControlResult {
    int status = 0;                  // 0 or -errno (-EPIPE = STALL)
    std::vector<std::uint8_t> data;  // reply of an IN request
  };

  /**
   * @brief Optional customisation of the emulated device.
   *
   * Every member is optional: a default-constructed DeviceConfig gives the
   * built-in DualSense (289-byte HID report descriptor, built-in feature
   * reports, device-generated input reports).
   */
  struct DeviceConfig {
    /**
     * @brief HID report descriptor to export.
     *
     * Empty: the built-in vds descriptor (289 bytes). The bytes are copied
     * at construction. Sizes above 65535 bytes fall back to the built-in one.
     * The wDescriptorLength fields of the configuration and HID descriptors
     * are recomputed from the size actually used.
     */
    std::span<const std::uint8_t> hid_report_descriptor {};

    /**
     * @brief Provider of feature reports for GET_REPORT(feature).
     *
     * Returning std::nullopt, or leaving the function empty, uses the built-in
     * tables (0x05, 0x09, 0x20; a zero report for other ids).
     */
    std::function<std::optional<std::vector<std::uint8_t>>(std::uint8_t report_id)> get_feature {};

    /**
     * @brief Provider of input reports (report 0x01).
     *
     * When set, the session calls it on each HID IN tick and on GET_REPORT(input),
     * and sends the returned 64 bytes unchanged: the provider owns byte 0 (report
     * id) and byte 7 (sequence number). When empty, the report stored with
     * set_input_report() is sent, with an incrementing sequence number.
     */
    std::function<std::array<std::uint8_t, 64>()> input_provider {};
  };

  /**
   * @brief Emulated DualSense: answers control requests and holds the input
   * report state. Thread-safe for the calls made by a session (reader thread)
   * and by its timer thread.
   */
  class DualSenseDevice {
  public:
    using Report = std::array<std::uint8_t, dualsense::kHidReportSize>;

    /** @brief Notifications sent to the host application. */
    struct Callbacks {
      /**
       * @brief Output report (0x02: rumble, triggers, LEDs), received on the
       * interrupt OUT endpoint or via SET_REPORT(output).
       */
      std::function<void(std::span<const std::uint8_t>)> output_report;
      /** @brief SET_REPORT(feature): byte 0 is the report number. */
      std::function<void(std::span<const std::uint8_t>)> feature_set;
      /** @brief SET_INTERFACE on an audio interface (alt 0 = stream stopped). */
      std::function<void(std::uint8_t interface, std::uint8_t alt)> interface_changed;
    };

    /**
     * @brief Builds the device.
     * @param config optional descriptor and report customisation (copied).
     * @param callbacks notifications to the host application.
     */
    explicit DualSenseDevice(DeviceConfig config = {}, Callbacks callbacks = {});

    /** @brief Handles one control request; out_data is the OUT payload, if any. */
    ControlResult control(const SetupPacket &setup, std::span<const std::uint8_t> out_data);

    /** @brief Handles a report received on the interrupt OUT endpoint. */
    void interrupt_out(std::span<const std::uint8_t> report);

    /** @brief Stores the input report (report id 0x01 is forced in byte 0). */
    void set_input_report(const Report &report);

    /** @brief Returns the stored input report with the next sequence number. */
    Report next_input_report();

    /**
     * @brief Input report for one HID IN tick: the input_provider result when
     * set (returned unchanged), otherwise next_input_report().
     */
    Report current_input_report();

    /** @brief Returns to the unconfigured state (no configuration, alt 0, default HID state). */
    void reset();

    /** @brief True when the audio output interface is on alternate setting 1. */
    bool audio_out_active() const;

    /** @brief True after SET_CONFIGURATION 1 and until reset(). */
    bool configured() const;

  private:
    ControlResult standard_request(const SetupPacket &setup);
    ControlResult hid_request(const SetupPacket &setup, std::span<const std::uint8_t> out_data);
    ControlResult audio_request(const SetupPacket &setup, std::span<const std::uint8_t> out_data);

    static Report neutral_input_report();

    Callbacks callbacks_;
    std::function<std::optional<std::vector<std::uint8_t>>(std::uint8_t)> feature_provider_;
    std::function<std::array<std::uint8_t, 64>()> input_provider_;
    std::vector<std::uint8_t> config_descriptor_;
    std::array<std::uint8_t, 9> hid_descriptor_ {};
    std::vector<std::uint8_t> hid_report_descriptor_;
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
