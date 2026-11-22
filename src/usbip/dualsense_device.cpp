// SPDX-License-Identifier: MIT
// C++ port of module/vds_usb_core.c from vds (Copyright (C) 2026 Jihong Min,
// MIT License). Differences: the feature reports come from fixed values
// (libvirtualhid) instead of the real controller, and the volume starts at 0 dB.
#include "dualsense_device.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <span>
#include <string_view>

namespace ds5usbip {

  namespace {
    namespace ds = dualsense;

    // Standard requests (USB 2.0, chapter 9).
    constexpr std::uint8_t kReqGetStatus = 0x00;
    constexpr std::uint8_t kReqClearFeature = 0x01;
    constexpr std::uint8_t kReqSetFeature = 0x03;
    constexpr std::uint8_t kReqSetAddress = 0x05;
    constexpr std::uint8_t kReqGetDescriptor = 0x06;
    constexpr std::uint8_t kReqGetConfiguration = 0x08;
    constexpr std::uint8_t kReqSetConfiguration = 0x09;
    constexpr std::uint8_t kReqGetInterface = 0x0a;
    constexpr std::uint8_t kReqSetInterface = 0x0b;

    constexpr std::uint8_t kDtDevice = 0x01;
    constexpr std::uint8_t kDtConfig = 0x02;
    constexpr std::uint8_t kDtString = 0x03;
    constexpr std::uint8_t kDtDeviceQualifier = 0x06;
    constexpr std::uint8_t kDtHid = 0x21;
    constexpr std::uint8_t kDtHidReport = 0x22;

    constexpr std::uint8_t kDirIn = 0x80;
    constexpr std::uint8_t kTypeMask = 0x60;
    constexpr std::uint8_t kTypeStandard = 0x00;
    constexpr std::uint8_t kTypeClass = 0x20;
    constexpr std::uint8_t kRecipMask = 0x1f;
    constexpr std::uint8_t kRecipInterface = 0x01;

    // HID class requests.
    constexpr std::uint8_t kHidGetReport = 0x01;
    constexpr std::uint8_t kHidGetIdle = 0x02;
    constexpr std::uint8_t kHidGetProtocol = 0x03;
    constexpr std::uint8_t kHidSetReport = 0x09;
    constexpr std::uint8_t kHidSetIdle = 0x0a;
    constexpr std::uint8_t kHidSetProtocol = 0x0b;
    constexpr std::uint8_t kHidReportInput = 0x01;
    constexpr std::uint8_t kHidReportOutput = 0x02;
    constexpr std::uint8_t kHidReportFeature = 0x03;

    // UAC1 class requests (Feature Unit).
    constexpr std::uint8_t kUacSetCur = 0x01;
    constexpr std::uint8_t kUacGetCur = 0x81;
    constexpr std::uint8_t kUacGetMin = 0x82;
    constexpr std::uint8_t kUacGetMax = 0x83;
    constexpr std::uint8_t kUacGetRes = 0x84;
    constexpr std::uint8_t kUacFuMute = 0x01;
    constexpr std::uint8_t kUacFuVolume = 0x02;

    // UAC1 volume: signed 8.8 dB, from -100 dB to 0 dB in 1 dB steps.
    constexpr std::int16_t kVolumeMin = -100 * 256;
    constexpr std::int16_t kVolumeMax = 0;
    constexpr std::int16_t kVolumeRes = 0x0100;

    ControlResult stall() {
      return {-EPIPE, {}};
    }

    ControlResult reply(std::span<const std::uint8_t> data, std::uint16_t w_length) {
      ControlResult r;
      const auto n = std::min<std::size_t>(data.size(), w_length);
      r.data.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n));
      return r;
    }

    ControlResult reply_u8(std::uint8_t v, std::uint16_t w_length) {
      return reply(std::span {&v, 1}, w_length);
    }

    ControlResult reply_le16(std::int16_t v, std::uint16_t w_length) {
      const auto u = static_cast<std::uint16_t>(v);
      const std::uint8_t b[2] {static_cast<std::uint8_t>(u & 0xff), static_cast<std::uint8_t>(u >> 8)};
      return reply(b, w_length);
    }

    ControlResult string_descriptor(std::string_view s, std::uint16_t w_length) {
      std::vector<std::uint8_t> d {0, kDtString};
      for (char c : s) {
        if (d.size() + 2 > 255) {
          break;
        }
        d.push_back(static_cast<std::uint8_t>(c));
        d.push_back(0);
      }
      d[0] = static_cast<std::uint8_t>(d.size());
      return reply(d, w_length);
    }

    int audio_feature_index(std::uint8_t entity) {
      if (entity == ds::kSpeakerFeatureUnit) {
        return 0;
      }
      if (entity == ds::kMicFeatureUnit) {
        return 1;
      }
      return -1;
    }
  }  // namespace

  SetupPacket SetupPacket::parse(const std::uint8_t raw[8]) {
    return {
      raw[0],
      raw[1],
      static_cast<std::uint16_t>(raw[2] | (raw[3] << 8)),
      static_cast<std::uint16_t>(raw[4] | (raw[5] << 8)),
      static_cast<std::uint16_t>(raw[6] | (raw[7] << 8)),
    };
  }

  DualSenseDevice::DualSenseDevice(DeviceConfig config, Callbacks callbacks):
      callbacks_(std::move(callbacks)),
      feature_provider_(std::move(config.get_feature)),
      input_provider_(std::move(config.input_provider)),
      input_report_(neutral_input_report()) {
    // Empty, oversized or missing report descriptor: the built-in one.
    std::span<const std::uint8_t> report = config.hid_report_descriptor;
    if (report.empty() || report.size() > 0xffff) {
      report = std::span(ds::kHidReportDescriptor);
    }
    hid_report_descriptor_.assign(report.begin(), report.end());
    const auto length = static_cast<std::uint16_t>(hid_report_descriptor_.size());
    const auto lo = static_cast<std::uint8_t>(length & 0xff);
    const auto hi = static_cast<std::uint8_t>(length >> 8);

    // Standalone HID descriptor (GET_DESCRIPTOR 0x21) and the copy embedded in
    // the configuration descriptor: wDescriptorLength must match the report
    // descriptor actually exported.
    hid_descriptor_ = ds::kHidDescriptor;
    hid_descriptor_[7] = lo;
    hid_descriptor_[8] = hi;
    config_descriptor_.assign(ds::kConfigurationDescriptor.begin(), ds::kConfigurationDescriptor.end());
    static constexpr std::array<std::uint8_t, 7> kHidPrefix {0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22};
    const auto embedded = std::search(config_descriptor_.begin(), config_descriptor_.end(), kHidPrefix.begin(), kHidPrefix.end());
    if (embedded != config_descriptor_.end()) {
      embedded[7] = lo;
      embedded[8] = hi;
    }
    reset();
  }

  DualSenseDevice::Report DualSenseDevice::neutral_input_report() {
    // USB report 0x01 (offsets include the report id byte):
    // 1-4 sticks, 5-6 triggers, 7 sequence, 8 d-pad (bits 0-3, 8 = released)
    // + buttons, 33 and 37 touchpad contacts (bit 7 = no contact),
    // 53 battery (bits 0-3 level, 4-7 charging state).
    Report r {};
    r[0] = ds::kInputReportId;
    r[1] = r[2] = r[3] = r[4] = 0x80;
    r[8] = 0x08;
    r[33] = 0x80;
    r[37] = 0x80;
    r[53] = 0x0a;
    return r;
  }

  void DualSenseDevice::reset() {
    std::lock_guard lock(mutex_);
    configuration_ = 0;
    audio_out_alt_ = 0;
    audio_in_alt_ = 0;
    hid_idle_ = 0;
    hid_protocol_ = 1;
    audio_mute_ = {};
    audio_volume_ = {kVolumeMax, kVolumeMax};
  }

  bool DualSenseDevice::audio_out_active() const {
    std::lock_guard lock(mutex_);
    return audio_out_alt_ == 1;
  }

  void DualSenseDevice::set_input_report(const Report &report) {
    std::lock_guard lock(mutex_);
    input_report_ = report;
    input_report_[0] = ds::kInputReportId;
  }

  DualSenseDevice::Report DualSenseDevice::next_input_report() {
    std::lock_guard lock(mutex_);
    input_report_[7] = input_seq_++;
    return input_report_;
  }

  DualSenseDevice::Report DualSenseDevice::current_input_report() {
    if (input_provider_) {
      // Called without holding mutex_: the provider may call set_input_report().
      return input_provider_();
    }
    return next_input_report();
  }

  bool DualSenseDevice::configured() const {
    std::lock_guard lock(mutex_);
    return configuration_ != 0;
  }

  void DualSenseDevice::interrupt_out(std::span<const std::uint8_t> report) {
    if (callbacks_.output_report && !report.empty()) {
      callbacks_.output_report(report);
    }
  }

  ControlResult DualSenseDevice::control(const SetupPacket &setup, std::span<const std::uint8_t> out_data) {
    const auto type = setup.bmRequestType & kTypeMask;
    if (type == kTypeStandard) {
      return standard_request(setup);
    }
    if (type == kTypeClass && (setup.bmRequestType & kRecipMask) == kRecipInterface) {
      if ((setup.wIndex & 0xff) == ds::kHidInterface) {
        return hid_request(setup, out_data);
      }
      return audio_request(setup, out_data);
    }
    return stall();
  }

  ControlResult DualSenseDevice::standard_request(const SetupPacket &setup) {
    const std::uint8_t desc_type = setup.wValue >> 8;
    const std::uint8_t desc_index = setup.wValue & 0xff;
    const std::uint8_t intf = setup.wIndex & 0xff;
    const std::uint8_t value = setup.wValue & 0xff;

    switch (setup.bRequest) {
      case kReqGetDescriptor:
        switch (desc_type) {
          case kDtDevice:
            return reply(ds::kDeviceDescriptor, setup.wLength);
          case kDtConfig:
            return reply(config_descriptor_, setup.wLength);
          case kDtDeviceQualifier:
            return reply(ds::kDeviceQualifierDescriptor, setup.wLength);
          case kDtString:
            switch (desc_index) {
              case 0:
                {
                  static constexpr std::uint8_t langids[] {0x04, kDtString, 0x09, 0x04};
                  return reply(langids, setup.wLength);
                }
              case 1:
                return string_descriptor(ds::kManufacturer, setup.wLength);
              case 2:
                return string_descriptor(ds::kProduct, setup.wLength);
              default:
                return stall();
            }
          case kDtHidReport:
            if (intf != ds::kHidInterface) {
              return stall();
            }
            return reply(hid_report_descriptor_, setup.wLength);
          case kDtHid:
            if (intf != ds::kHidInterface) {
              return stall();
            }
            return reply(hid_descriptor_, setup.wLength);
          default:
            return stall();
        }

      case kReqSetAddress:
        return {};

      case kReqSetConfiguration:
        {
          if (value && value != ds::kConfigurationValue) {
            return stall();
          }
          std::lock_guard lock(mutex_);
          configuration_ = value;
          audio_out_alt_ = 0;
          audio_in_alt_ = 0;
          return {};
        }

      case kReqGetConfiguration:
        {
          std::lock_guard lock(mutex_);
          return reply_u8(configuration_, setup.wLength);
        }

      case kReqSetInterface:
        {
          const std::uint8_t alt = setup.wValue & 0xff;
          if (intf == ds::kHidInterface) {
            return alt ? stall() : ControlResult {};
          }
          if (intf == ds::kAudioControlInterface) {
            return alt ? stall() : ControlResult {};
          }
          if (alt > 1 || (intf != ds::kAudioOutInterface && intf != ds::kAudioInInterface)) {
            return stall();
          }
          {
            std::lock_guard lock(mutex_);
            (intf == ds::kAudioOutInterface ? audio_out_alt_ : audio_in_alt_) = alt;
          }
          if (callbacks_.interface_changed) {
            callbacks_.interface_changed(intf, alt);
          }
          return {};
        }

      case kReqGetInterface:
        {
          std::lock_guard lock(mutex_);
          if (intf == ds::kAudioOutInterface) {
            return reply_u8(audio_out_alt_, setup.wLength);
          }
          if (intf == ds::kAudioInInterface) {
            return reply_u8(audio_in_alt_, setup.wLength);
          }
          if (intf == ds::kHidInterface || intf == ds::kAudioControlInterface) {
            return reply_u8(0, setup.wLength);
          }
          return stall();
        }

      case kReqGetStatus:
        {
          static constexpr std::uint8_t status[2] {0, 0};
          return reply(status, setup.wLength);
        }

      case kReqClearFeature:
      case kReqSetFeature:
        return {};

      default:
        return stall();
    }
  }

  ControlResult DualSenseDevice::hid_request(const SetupPacket &setup, std::span<const std::uint8_t> out_data) {
    const std::uint8_t report_type = setup.wValue >> 8;
    const std::uint8_t report_id = setup.wValue & 0xff;

    switch (setup.bRequest) {
      case kHidGetReport:
        if (report_type == kHidReportFeature) {
          if (feature_provider_) {
            if (auto custom = feature_provider_(report_id)) {
              return reply(*custom, setup.wLength);
            }
          }
          switch (report_id) {
            case 0x05:
              return reply(ds::kFeatureCalibration, setup.wLength);
            case 0x09:
              return reply(ds::kFeaturePairing, setup.wLength);
            case 0x20:
              return reply(ds::kFeatureFirmware, setup.wLength);
            default:
              {
                // Unknown report: a zero report rather than a STALL, as vds
                // does for reports that are not features.
                std::vector<std::uint8_t> zero(std::max<std::uint16_t>(setup.wLength, 1), 0);
                zero[0] = report_id;
                return reply(zero, setup.wLength);
              }
          }
        }
        if (report_type == kHidReportInput && report_id == ds::kInputReportId) {
          const auto r = current_input_report();
          return reply(r, setup.wLength);
        }
        {
          std::vector<std::uint8_t> zero(ds::kHidReportSize, 0);
          zero[0] = report_id;
          return reply(zero, setup.wLength);
        }

      case kHidSetReport:
        if (report_type == kHidReportOutput) {
          if (callbacks_.output_report) {
            callbacks_.output_report(out_data);
          }
          return {};
        }
        if (report_type != kHidReportFeature) {
          return stall();
        }
        if (out_data.size() > ds::kHidReportSize) {
          return stall();
        }
        if (callbacks_.feature_set && !out_data.empty()) {
          callbacks_.feature_set(out_data);
        }
        return {};

      case kHidGetIdle:
        {
          std::lock_guard lock(mutex_);
          return reply_u8(hid_idle_, setup.wLength);
        }

      case kHidGetProtocol:
        {
          std::lock_guard lock(mutex_);
          return reply_u8(hid_protocol_, setup.wLength);
        }

      case kHidSetIdle:
        {
          std::lock_guard lock(mutex_);
          hid_idle_ = setup.wValue >> 8;
          return {};
        }

      case kHidSetProtocol:
        {
          std::lock_guard lock(mutex_);
          hid_protocol_ = setup.wValue & 0xff;
          return {};
        }

      default:
        return stall();
    }
  }

  ControlResult DualSenseDevice::audio_request(const SetupPacket &setup, std::span<const std::uint8_t> out_data) {
    const std::uint8_t entity = setup.wIndex >> 8;
    const std::uint8_t intf = setup.wIndex & 0xff;
    const std::uint8_t selector = setup.wValue >> 8;

    if (intf != ds::kAudioControlInterface) {
      return stall();
    }
    const int fi = audio_feature_index(entity);
    if (fi < 0) {
      return stall();
    }

    if (setup.bmRequestType & kDirIn) {
      std::lock_guard lock(mutex_);
      switch (selector) {
        case kUacFuMute:
          if (setup.bRequest != kUacGetCur) {
            return stall();
          }
          return reply_u8(audio_mute_[fi], setup.wLength);
        case kUacFuVolume:
          switch (setup.bRequest) {
            case kUacGetCur:
              return reply_le16(audio_volume_[fi], setup.wLength);
            case kUacGetMin:
              return reply_le16(kVolumeMin, setup.wLength);
            case kUacGetMax:
              return reply_le16(kVolumeMax, setup.wLength);
            case kUacGetRes:
              return reply_le16(kVolumeRes, setup.wLength);
            default:
              return stall();
          }
        default:
          return stall();
      }
    }

    if (setup.bRequest != kUacSetCur) {
      return stall();
    }
    std::lock_guard lock(mutex_);
    if (selector == kUacFuMute && !out_data.empty()) {
      audio_mute_[fi] = out_data[0];
      return {};
    }
    if (selector == kUacFuVolume && out_data.size() >= 2) {
      audio_volume_[fi] = static_cast<std::int16_t>(out_data[0] | (out_data[1] << 8));
      return {};
    }
    return stall();
  }

}  // namespace ds5usbip
