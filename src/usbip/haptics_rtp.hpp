// SPDX-License-Identifier: MIT
// Sends the haptic channels (3-4) of the emulated DualSense over RTP to the
// client PC: stereo L16, big-endian, 48 kHz, one packet per isochronous packet
// (1 ms, 48 frames). The format is read as-is by PipeWire's module-rtp-source
// (audio.format S16BE, audio.channels 2, no SAP announcement).
#pragma once

#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "log.hpp"

namespace ds5usbip {

  /**
   * @brief Haptics settings read from a "key=value" text file (see load_haptics_settings).
   */
  struct HapticsSettings {
    /** @brief Gain from the "gain=" key, in the range 0 < g <= 64. */
    std::optional<float> gain;
    /** @brief Channel count from the "channels=" key: 2 or 4. */
    std::optional<unsigned> channels;
  };

  /**
   * @brief Parses settings text: one "key=value" per line, "#" starts a comment, blank lines are ignored.
   *
   * Unknown keys and out-of-range values are ignored. A later valid line replaces an earlier one.
   * @param text File content.
   * @return The valid settings found in text.
   */
  HapticsSettings parse_haptics_settings(std::string_view text);

  /**
   * @brief Reads and parses a settings file. A missing or unreadable file gives empty settings.
   * @param path Settings file path.
   * @return The valid settings found in the file.
   */
  HapticsSettings load_haptics_settings(const std::string &path);

  /**
   * @brief Parses a gain value: a plain number ("2.5") or a "gain=<float>" line among other lines.
   * @param text Gain file content.
   * @return The gain when a valid one (0 < g <= 64) is found, otherwise nullopt.
   */
  std::optional<float> parse_gain_text(std::string_view text);

  /**
   * @brief Writes text to "<path>.tmp" and renames it over path, so readers never see a partial file.
   * @return false on any I/O error (the target file is then left unchanged).
   */
  bool write_text_atomically(const std::string &path, std::string_view text);

  /** @brief RTP sender for the haptic channels 2-3 of the 4-channel audio output. */
  class HapticsRtpSender {
  public:
    /** @brief RTP payload type: 127, the default of PipeWire's RTP modules for raw audio. */
    static constexpr std::uint8_t kPayloadType = 127;

    /** @brief Creates an unconnected sender. @param log diagnostics; empty = silent. */
    explicit HapticsRtpSender(LogFn log = {});
    ~HapticsRtpSender();

    HapticsRtpSender(const HapticsRtpSender &) = delete;
    HapticsRtpSender &operator=(const HapticsRtpSender &) = delete;

    /**
     * @brief Sets the destination "IP:PORT" (IPv4 literal) and opens the UDP socket.
     * A previous destination is replaced.
     * @return false if the destination is invalid or the socket cannot be created.
     */
    bool set_destination(const std::string &ip_port);

    /**
     * @brief Gain applied to the haptics before sending, with soft clipping above
     * half of full scale. If gain_file is given, its content (a number, e.g. "2.5")
     * is read again every second, so the gain can be changed while running.
     */
    void set_gain(float gain, std::string gain_file = {});

    /**
     * @brief Choose how many channels are sent: 2 (haptics only, channels 3-4,
     *        default) or 4 (speaker channels 1-2 unchanged + haptics 3-4).
     *        The gain applies to the haptic channels only.
     */
    void set_channels(unsigned channels);

    /**
     * @brief Sends one isochronous output packet (S16_LE, 4 interleaved channels);
     * only channels 2 and 3 are sent. Non-blocking: packets are dropped on error.
     */
    void send(std::span<const std::uint8_t> pcm4ch);

    /** @brief Number of RTP packets sent so far. Safe to call from any thread. */
    std::uint64_t packets() const {
      return packets_.load(std::memory_order_relaxed);
    }

    /** @brief Current gain (after any reload from the gain file). Safe to call from any thread. */
    float gain() const {
      return gain_.load(std::memory_order_relaxed);
    }

  private:
    LogFn log_;
    int fd_ = -1;
    sockaddr_in dest_ {};
    std::uint16_t seq_ = 0;
    std::uint32_t timestamp_ = 0;
    std::uint32_t ssrc_ = 0;
    std::atomic<std::uint64_t> packets_ {0};
    std::atomic<float> gain_ {1.0f};
    unsigned channels_ = 2;
    std::string gain_file_;
    std::chrono::steady_clock::time_point last_gain_check_ {};

    void log(const std::string &msg) const;
    void reload_gain();
    static std::int16_t shape(std::int16_t sample, float gain);
  };

}  // namespace ds5usbip
