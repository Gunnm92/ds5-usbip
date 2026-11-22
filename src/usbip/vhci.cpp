// SPDX-License-Identifier: MIT
#include "vhci.hpp"

#include <algorithm>
#include <cerrno>
#include <format>
#include <fstream>
#include <sstream>

namespace ds5usbip::vhci {

  std::vector<Port> parse_status(std::string_view text) {
    std::vector<Port> ports;
    std::istringstream lines {std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
      std::istringstream in(line);
      Port p {};
      if (in >> p.hub >> p.port >> p.status) {
        ports.push_back(std::move(p));
      }
    }
    return ports;
  }

  std::optional<int> pick_free_port(std::span<const Port> ports, std::span<const int> skip) {
    for (const auto &p : ports) {
      if (p.hub != "hs" || p.status != kStatusNull) {
        continue;
      }
      if (std::find(skip.begin(), skip.end(), p.port) != skip.end()) {
        continue;
      }
      return p.port;
    }
    return std::nullopt;
  }

  std::string attach_line(int port, int fd, std::uint32_t devid, int speed) {
    return std::format("{} {} {} {}", port, fd, devid, speed);
  }

  std::optional<std::string> read_text(const std::string &path) {
    std::ifstream f(path);
    if (!f) {
      return std::nullopt;
    }
    std::ostringstream all;
    all << f.rdbuf();
    return all.str();
  }

  int write_text(const std::string &path, std::string_view text) {
    std::ofstream f(path);
    if (!f) {
      return -(errno ? errno : EIO);
    }
    f << text;
    f.flush();
    if (!f) {
      return -(errno ? errno : EIO);
    }
    return 0;
  }

  std::optional<std::vector<Port>> load_ports(std::string_view sysfs) {
    const auto text = read_text(std::string(sysfs) + "/status");
    if (!text) {
      return std::nullopt;
    }
    return parse_status(*text);
  }

  int attach_device(std::string_view sysfs, int port, int fd, std::uint32_t devid, int speed) {
    return write_text(std::string(sysfs) + "/attach", attach_line(port, fd, devid, speed));
  }

  int detach_port(std::string_view sysfs, int port) {
    return write_text(std::string(sysfs) + "/detach", std::to_string(port));
  }

}  // namespace ds5usbip::vhci
