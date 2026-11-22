// SPDX-License-Identifier: MIT
// Helpers for the vhci-hcd kernel driver: parsing of its "status" file and
// writes to its "attach" / "detach" files. Pure text handling; the file
// accesses use the iostreams and never touch the host's sysfs unless a tool
// asks for it.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ds5usbip::vhci {

  /** @brief Sysfs directory of the vhci-hcd platform device. */
  inline constexpr std::string_view kDefaultSysfs = "/sys/devices/platform/vhci_hcd.0";

  /** @brief Port status value of a free port (VDEV_ST_NULL). */
  inline constexpr int kStatusNull = 4;

  /** @brief One line of the status file. */
  struct Port {
    std::string hub;  // "hs" (high speed) or "ss" (super speed)
    int port;
    int status;       // 4 = free (kStatusNull)
  };

  /**
   * @brief Parses the text of the status file (header line
   * "hub port sta spd dev sockfd local_busid", then one line per port).
   * Lines that do not parse are skipped, so the header is ignored.
   */
  std::vector<Port> parse_status(std::string_view text);

  /**
   * @brief First high-speed port with status kStatusNull that is not in skip.
   * @return the port number, or std::nullopt if none is free.
   */
  std::optional<int> pick_free_port(std::span<const Port> ports, std::span<const int> skip = {});

  /** @brief Text written to the attach file: "port fd devid speed" (no newline). */
  std::string attach_line(int port, int fd, std::uint32_t devid, int speed);

  /**
   * @brief Reads a whole text file.
   * @return its content, or std::nullopt if it cannot be opened.
   */
  std::optional<std::string> read_text(const std::string &path);

  /**
   * @brief Writes a whole text file (truncating it).
   * @return 0, or -errno on failure.
   */
  int write_text(const std::string &path, std::string_view text);

  /**
   * @brief Reads and parses <sysfs>/status.
   * @return the ports, or std::nullopt if the file cannot be loaded (vhci-hcd not loaded?).
   */
  std::optional<std::vector<Port>> load_ports(std::string_view sysfs = kDefaultSysfs);

  /**
   * @brief Writes "port fd devid speed" to <sysfs>/attach. The kernel takes its
   * own reference on fd, so the caller may close its copy afterwards.
   * @return 0, or -errno on failure.
   */
  int attach_device(std::string_view sysfs, int port, int fd, std::uint32_t devid, int speed);

  /**
   * @brief Writes the port number to <sysfs>/detach.
   * @return 0, or -errno on failure.
   */
  int detach_port(std::string_view sysfs, int port);

}  // namespace ds5usbip::vhci
