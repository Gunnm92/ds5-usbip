// SPDX-License-Identifier: MIT
// Logging hook of the library: the caller decides where messages go.
#pragma once

#include <functional>
#include <string_view>

namespace ds5usbip {

  /**
   * @brief Receives one log line, without the trailing newline.
   *
   * An empty function means silent: the library never writes to stdout or
   * stderr by itself.
   */
  using LogFn = std::function<void(std::string_view)>;

}  // namespace ds5usbip
