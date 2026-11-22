// SPDX-License-Identifier: MIT
// USB/IP server exporting an emulated DualSense (one device, busid "1-1").
// Handles OP_REQ_DEVLIST / OP_REQ_IMPORT on TCP; after a successful import the
// connected socket is handed to a ds5usbip::UsbipSession, one session at a time.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "usbip/dualsense_device.hpp"

namespace ds5usbip {

  /** @brief TCP listener that exports one DualSense to USB/IP clients. */
  class UsbipServer {
  public:
    /** @brief Listening options. */
    struct Options {
      std::string bind_address = "0.0.0.0";
      std::uint16_t port = 3240;
      bool verbose = false;
      // If set, replaces verbose and can change at run time (SIGUSR1).
      const std::atomic<bool> *verbose_flag = nullptr;
    };

    /** @brief Notifications for the tool. */
    struct Callbacks {
      // One isochronous output packet: S16_LE interleaved, 4 channels.
      std::function<void(std::span<const std::uint8_t> pcm)> audio_out;
      // Start (true) / end (false) of an imported session.
      std::function<void(bool attached)> session;
    };

    /** @brief Creates the server; nothing is bound until run(). */
    UsbipServer(DualSenseDevice &device, Options options, Callbacks callbacks = {});

    /**
     * @brief Accept loop, blocking until stop becomes true.
     * @return 0, or 1 if the listening socket cannot be set up.
     */
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
