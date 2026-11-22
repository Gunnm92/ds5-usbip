// SPDX-License-Identifier: MIT
// Helpers shared by the command-line tools (not part of the library):
// signal handling, stderr logging, audio level meter, output-report log.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <csignal>
#include <signal.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "usbip/dualsense_descriptors.hpp"

namespace ds5usbip::tools {

  inline std::atomic<bool> g_stop {false};
  inline std::atomic<bool> g_verbose {false};

  inline void on_stop_signal(int) {
    g_stop.store(true);
  }

  // SIGUSR1: toggles verbose mode without ending the session.
  inline void on_usr1(int) {
    g_verbose.store(!g_verbose.load());
  }

  /** @brief Installs SIGINT/SIGTERM (stop) and SIGUSR1 (verbose toggle) handlers. */
  inline void install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = on_stop_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    struct sigaction su {};
    su.sa_handler = on_usr1;
    su.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &su, nullptr);
  }

  /** @brief Library log sink: one line on stderr. */
  inline void log_stderr(std::string_view msg) {
    std::fprintf(stderr, "%.*s\n", static_cast<int>(msg.size()), msg.data());
  }

  // Statistics per one-second window (48 000 frames).
  class AudioMeter {
  public:
    explicit AudioMeter(std::FILE *dump):
        dump_(dump) {}

    void feed(std::span<const std::uint8_t> pcm) {
      constexpr unsigned kCh = ds5usbip::dualsense::kAudioOutChannels;
      if (dump_) {
        std::fwrite(pcm.data(), 1, pcm.size(), dump_);
      }
      std::lock_guard lock(mutex_);
      const std::size_t frames = pcm.size() / (2 * kCh);
      for (std::size_t f = 0; f < frames; ++f) {
        for (unsigned ch = 0; ch < kCh; ++ch) {
          const std::size_t i = (f * kCh + ch) * 2;
          const auto s = static_cast<std::int16_t>(pcm[i] | (pcm[i + 1] << 8));
          const int a = std::abs(static_cast<int>(s));
          peak_[ch] = std::max(peak_[ch], a);
          sum_sq_[ch] += static_cast<double>(s) * s;
          if (s) {
            ++nonzero_[ch];
          }
        }
        if (++frames_ >= ds5usbip::dualsense::kSampleRate) {
          print_locked();
        }
      }
    }

    void flush() {
      std::lock_guard lock(mutex_);
      if (frames_) {
        print_locked();
      }
      if (dump_) {
        std::fflush(dump_);
      }
    }

  private:
    void print_locked() {
      auto db = [this](unsigned ch) {
        const double rms = std::sqrt(sum_sq_[ch] / static_cast<double>(frames_));
        return rms > 0 ? 20.0 * std::log10(rms / 32768.0) : -999.0;
      };
      std::fprintf(
        stderr,
        "audio: %6zu trames | HP G %6d %6.1f dB  HP D %6d %6.1f dB | haptique G %6d %6.1f dB (%5zu non nuls)  haptique D %6d %6.1f dB (%5zu non nuls)\n",
        frames_,
        peak_[0],
        db(0),
        peak_[1],
        db(1),
        peak_[2],
        db(2),
        nonzero_[2],
        peak_[3],
        db(3),
        nonzero_[3]
      );
      frames_ = 0;
      peak_ = {};
      sum_sq_ = {};
      nonzero_ = {};
    }

    std::FILE *dump_;
    std::mutex mutex_;
    std::size_t frames_ = 0;
    std::array<int, 4> peak_ {};
    std::array<double, 4> sum_sq_ {};
    std::array<std::size_t, 4> nonzero_ {};
  };

  // Detailed log: output report 0x02 printed when it changes, with the useful
  // decoded fields (offsets include the report id byte): 1-2 validity flags
  // (0x04 / 0x08 of the first: right / left trigger), 3-4 right / left rumble,
  // 11 right trigger mode, 22 left trigger mode.
  inline void log_output_report(std::span<const std::uint8_t> r) {
    static std::vector<std::uint8_t> last;
    static std::size_t same = 0;
    if (std::equal(r.begin(), r.end(), last.begin(), last.end())) {
      ++same;
      return;
    }
    last.assign(r.begin(), r.end());
    std::string hex;
    char b[4];
    for (const auto c : r) {
      std::snprintf(b, sizeof(b), "%02x", c);
      hex += b;
    }
    if (r.size() >= 23 && r[0] == 0x02) {
      std::fprintf(
        stderr,
        "hid: sortie (après %zu identiques) flags %02x %02x vib D %3u G %3u | gâchette D mode %02x G mode %02x | %s\n",
        same, r[1], r[2], r[3], r[4], r[11], r[22], hex.c_str()
      );
    } else {
      std::fprintf(stderr, "hid: sortie 0x%02x %zu octets | %s\n", r[0], r.size(), hex.c_str());
    }
    same = 0;
  }

}  // namespace ds5usbip::tools
