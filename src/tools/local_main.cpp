// SPDX-License-Identifier: MIT
// ds5-usbip-local : attaches the emulated DualSense to the local vhci-hcd
// without any network. A socketpair replaces the TCP connection: one end is
// served by a UsbipSession here, the other goes to the kernel through the
// vhci "attach" file. Runs until SIGINT / SIGTERM.
//
// Against a real /sys this attaches a device to the host kernel: use --sysfs
// with a fake directory for tests.
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <string>
#include <thread>

#include "tool_common.hpp"
#include "usbip/dualsense_device.hpp"
#include "usbip/haptics_rtp.hpp"
#include "usbip/usbip_session.hpp"
#include "usbip/vhci.hpp"

namespace {

  using namespace ds5usbip;

  // busid 1-1 as a USB device: bus 1, device 1. Speed 3 = high speed.
  constexpr std::uint32_t kDevId = (1u << 16) | 1u;
  constexpr int kSpeedHigh = 3;

  void usage(const char *argv0) {
    std::printf(
      "Usage : %s [options]\n"
      "\n"
      "Attache une DualSense émulée au vhci-hcd local, sans réseau (socketpair).\n"
      "Attention : avec le vrai /sys, le périphérique apparaît dans le noyau hôte.\n"
      "\n"
      "  --sysfs DIR         répertoire vhci (défaut %s)\n"
      "  -d, --dump FICHIER  enregistre le PCM reçu (S16_LE, 4 canaux, 48 kHz)\n"
      "  -t, --haptics-to IP:PORT\n"
      "                      envoie les canaux haptiques 3-4 en RTP (L16 stéréo, PT 127)\n"
      "  -g, --haptics-gain G gain des haptiques (défaut 1)\n"
      "      --haptics-gain-file FICHIER\n"
      "                      gain relu chaque seconde dans ce fichier\n"
      "  -v, --verbose       journalise les requêtes de contrôle et les UNLINK\n"
      "  -h, --help          cette aide\n",
      argv0,
      std::string(vhci::kDefaultSysfs).c_str()
    );
  }

}  // namespace

int main(int argc, char **argv) {
  std::string sysfs(vhci::kDefaultSysfs);
  std::string dump_path;
  std::string haptics_to;
  float haptics_gain = 1.0f;
  std::string haptics_gain_file;
  bool verbose = false;

  static const option long_options[] {
    {"sysfs", required_argument, nullptr, 's'},
    {"dump", required_argument, nullptr, 'd'},
    {"haptics-to", required_argument, nullptr, 't'},
    {"haptics-gain", required_argument, nullptr, 'g'},
    {"haptics-gain-file", required_argument, nullptr, 'G'},
    {"verbose", no_argument, nullptr, 'v'},
    {"help", no_argument, nullptr, 'h'},
    {nullptr, 0, nullptr, 0},
  };
  int c;
  while ((c = getopt_long(argc, argv, "d:t:g:vh", long_options, nullptr)) != -1) {
    switch (c) {
      case 's':
        sysfs = optarg;
        break;
      case 'd':
        dump_path = optarg;
        break;
      case 't':
        haptics_to = optarg;
        break;
      case 'g':
        haptics_gain = std::strtof(optarg, nullptr);
        break;
      case 'G':
        haptics_gain_file = optarg;
        break;
      case 'v':
        verbose = true;
        break;
      case 'h':
        usage(argv[0]);
        return 0;
      default:
        usage(argv[0]);
        return 2;
    }
  }

  std::FILE *dump = nullptr;
  if (!dump_path.empty()) {
    dump = std::fopen(dump_path.c_str(), "wb");
    if (!dump) {
      std::fprintf(stderr, "impossible d'ouvrir %s : %s\n", dump_path.c_str(), std::strerror(errno));
      return 1;
    }
  }

  tools::install_signal_handlers();
  tools::g_verbose.store(verbose);

  tools::AudioMeter meter(dump);
  HapticsRtpSender haptics(tools::log_stderr);
  if (!haptics_to.empty()) {
    if (!haptics.set_destination(haptics_to)) {
      return 1;
    }
    haptics.set_gain(haptics_gain, haptics_gain_file);
  }

  DualSenseDevice device(
    DeviceConfig {},
    DualSenseDevice::Callbacks {
      .output_report =
        [](std::span<const std::uint8_t> r) {
          if (tools::g_verbose.load()) {
            tools::log_output_report(r);
          }
        },
      .feature_set =
        [](std::span<const std::uint8_t> r) {
          std::fprintf(stderr, "hid: SET_REPORT(feature 0x%02x) %zu octets\n", r[0], r.size());
        },
      .interface_changed =
        [&meter](std::uint8_t intf, std::uint8_t alt) {
          const char *name = intf == dualsense::kAudioOutInterface ? "sortie (HP + haptiques)" : "entrée (micro)";
          std::fprintf(stderr, "audio: flux %s %s\n", name, alt ? "ouvert" : "fermé");
          if (!alt) {
            meter.flush();
          }
        },
    }
  );

  // Kernel side and session side of one stream socket.
  int pair[2] {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0) {
    std::fprintf(stderr, "socketpair : %s\n", std::strerror(errno));
    return 1;
  }

  UsbipSession::Callbacks session_cb;
  session_cb.audio_out = [&meter, &haptics](std::span<const std::uint8_t> pcm) {
    haptics.send(pcm);
    meter.feed(pcm);
  };
  session_cb.closed = [] {
    tools::log_stderr("ds5-usbip-local : session fermée par le noyau");
  };
  UsbipSession session(pair[0], device, std::move(session_cb), tools::log_stderr);
  session.set_verbose([] {
    return tools::g_verbose.load();
  });
  if (!session.start()) {  // closes pair[0]
    ::close(pair[1]);
    return 1;
  }

  int rc = 0;
  const auto ports = vhci::load_ports(sysfs);
  if (!ports) {
    std::fprintf(stderr, "%s/status illisible : vhci-hcd est-il chargé ?\n", sysfs.c_str());
    rc = 1;
  } else if (const auto port = vhci::pick_free_port(*ports); !port) {
    std::fprintf(stderr, "aucun port haute vitesse libre sur vhci-hcd\n");
    rc = 1;
  } else if (const int err = vhci::attach_device(sysfs, *port, pair[1], kDevId, kSpeedHigh); err < 0) {
    std::fprintf(stderr, "écriture de attach dans %s : %s\n", sysfs.c_str(), std::strerror(-err));
    rc = 1;
  } else {
    std::fprintf(stderr, "ds5-usbip-local : DualSense branchée sur le port %d (%s)\n", *port, sysfs.c_str());
  }
  // The kernel keeps its own reference after attach; close our copy.
  ::close(pair[1]);

  if (rc == 0) {
    while (!tools::g_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  session.stop();
  meter.flush();
  if (dump) {
    std::fclose(dump);
  }
  return rc;
}
