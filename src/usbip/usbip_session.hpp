// SPDX-License-Identifier: MIT
// USB/IP session: runs the URB protocol on one connected socket whose other
// end is the vhci-hcd kernel driver (or a test peer). There is no OP_* handshake
// here: the first bytes on the descriptor are USBIP_CMD_SUBMIT / CMD_UNLINK.
//
// Isochronous answers are paced in real time (1 ms per packet), as in the vds
// Windows export (src/platform/win32/vds_usbip.cc): snd-usb-audio is driven by
// URB completions and would otherwise consume audio at network speed. HID input
// reports leave every 4 ms, as a real DualSense does on USB.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "dualsense_device.hpp"
#include "log.hpp"

namespace ds5usbip {

  /**
   * @brief One imported device on one socket: a reader thread handles the
   * commands from the peer, a timer thread sends the deferred replies.
   *
   * The session owns the descriptor passed to the constructor: stop() (or the
   * destructor) closes it. Calls to start() and stop() must not run
   * concurrently with each other.
   */
  class UsbipSession {
  public:
    /** @brief Notifications sent to the host application. */
    struct Callbacks {
      /** @brief One isochronous output packet: S16_LE, 4 interleaved channels. */
      std::function<void(std::span<const std::uint8_t>)> audio_out;
      /** @brief Peer closed the connection or an error occurred (not called after stop()). */
      std::function<void()> closed;
    };

    /**
     * @brief Creates a session; nothing runs until start().
     * @param fd connected stream socket (blocking, no receive timeout).
     * @param device emulated device; must outlive the session.
     * @param callbacks audio and close notifications.
     * @param log diagnostics; an empty function is silent.
     */
    UsbipSession(int fd, DualSenseDevice &device, Callbacks callbacks, LogFn log = {});

    /** @brief Calls stop(). */
    ~UsbipSession();

    UsbipSession(const UsbipSession &) = delete;
    UsbipSession &operator=(const UsbipSession &) = delete;

    /**
     * @brief Sets the verbosity function: control requests and
     * UNLINK commands are logged while it returns true. Empty (default): never. Call before start().
     */
    void set_verbose(std::function<bool()> verbose);

    /**
     * @brief Starts the reader and timer threads.
     * @return false if already started, stopped, or a thread could not be
     * created (the session is then stopped and the descriptor closed).
     */
    bool start();

    /**
     * @brief Stops the session: shuts the socket down (waking the blocked
     * reader and any blocked send), joins both threads and closes the
     * descriptor. Idempotent. Returns quickly, without waiting for the peer.
     */
    void stop();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
  };

}  // namespace ds5usbip
