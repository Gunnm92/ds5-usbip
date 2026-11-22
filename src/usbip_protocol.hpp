// SPDX-License-Identifier: MIT
// Protocole USB/IP (côté serveur), d'après Documentation/usb/usbip_protocol.rst
// du noyau Linux. Tous les champs sont big-endian sur le fil, sauf le paquet
// SETUP (little-endian, format USB) et les données.
#pragma once

#include <arpa/inet.h>

#include <cstddef>
#include <cstdint>

namespace ds5usbip::usbip {

  inline constexpr std::uint16_t kVersion = 0x0111;
  inline constexpr std::uint16_t kDefaultPort = 3240;

  inline constexpr std::uint16_t kOpReqDevlist = 0x8005;
  inline constexpr std::uint16_t kOpRepDevlist = 0x0005;
  inline constexpr std::uint16_t kOpReqImport = 0x8003;
  inline constexpr std::uint16_t kOpRepImport = 0x0003;

  inline constexpr std::uint32_t kCmdSubmit = 0x1;
  inline constexpr std::uint32_t kCmdUnlink = 0x2;
  inline constexpr std::uint32_t kRetSubmit = 0x3;
  inline constexpr std::uint32_t kRetUnlink = 0x4;

  inline constexpr std::uint32_t kDirOut = 0;
  inline constexpr std::uint32_t kDirIn = 1;

  inline constexpr std::uint32_t kSpeedHigh = 3;  // enum usb_device_speed

  inline constexpr std::size_t kBusIdSize = 32;

#pragma pack(push, 1)
  struct OpHeader {
    std::uint16_t version;
    std::uint16_t code;
    std::uint32_t status;
  };

  struct UsbDevice {
    char path[256];
    char busid[kBusIdSize];
    std::uint32_t busnum;
    std::uint32_t devnum;
    std::uint32_t speed;
    std::uint16_t idVendor;
    std::uint16_t idProduct;
    std::uint16_t bcdDevice;
    std::uint8_t bDeviceClass;
    std::uint8_t bDeviceSubClass;
    std::uint8_t bDeviceProtocol;
    std::uint8_t bConfigurationValue;
    std::uint8_t bNumConfigurations;
    std::uint8_t bNumInterfaces;
  };

  struct UsbInterface {
    std::uint8_t bInterfaceClass;
    std::uint8_t bInterfaceSubClass;
    std::uint8_t bInterfaceProtocol;
    std::uint8_t padding;
  };

  struct HeaderBasic {
    std::uint32_t command;
    std::uint32_t seqnum;
    std::uint32_t devid;
    std::uint32_t direction;
    std::uint32_t ep;
  };

  struct CmdSubmit {
    std::uint32_t transfer_flags;
    std::int32_t transfer_buffer_length;
    std::int32_t start_frame;
    std::int32_t number_of_packets;
    std::int32_t interval;
    std::uint8_t setup[8];
  };

  struct RetSubmit {
    std::int32_t status;
    std::int32_t actual_length;
    std::int32_t start_frame;
    std::int32_t number_of_packets;
    std::int32_t error_count;
    std::uint8_t padding[8];
  };

  struct CmdUnlink {
    std::uint32_t seqnum;
    std::uint8_t padding[24];
  };

  struct RetUnlink {
    std::int32_t status;
    std::uint8_t padding[24];
  };

  struct Header {
    HeaderBasic base;

    union {
      CmdSubmit cmd_submit;
      RetSubmit ret_submit;
      CmdUnlink cmd_unlink;
      RetUnlink ret_unlink;
    } u;
  };

  struct IsoPacketDescriptor {
    std::uint32_t offset;
    std::uint32_t length;
    std::uint32_t actual_length;
    std::uint32_t status;
  };
#pragma pack(pop)

  static_assert(sizeof(OpHeader) == 8);
  static_assert(sizeof(UsbDevice) == 312);
  static_assert(sizeof(Header) == 48);
  static_assert(sizeof(IsoPacketDescriptor) == 16);

  inline std::uint32_t be32(std::uint32_t v) {
    return htonl(v);
  }

  inline std::int32_t be32s(std::int32_t v) {
    return static_cast<std::int32_t>(htonl(static_cast<std::uint32_t>(v)));
  }

  inline std::uint16_t be16(std::uint16_t v) {
    return htons(v);
  }

}  // namespace ds5usbip::usbip
