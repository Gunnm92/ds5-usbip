// SPDX-License-Identifier: MIT
// ds5-usbip-proto : prototype. Exports an emulated DualSense (HID + 4-channel
// sound card) over USB/IP, without Sunshine, and prints the level of the
// received channels every second (0-1 headphones, 2-3 haptics).
#include <getopt.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "tool_common.hpp"
#include "usbip/dualsense_device.hpp"
#include "usbip/haptics_rtp.hpp"
#include "usbip_server.hpp"

namespace {

  using namespace ds5usbip;

  void usage(const char *argv0) {
    std::printf(
      "Usage : %s [options]\n"
      "\n"
      "Exporte une DualSense USB émulée (HID + carte son 4 canaux 48 kHz) par\n"
      "USB/IP. Côté hôte : modprobe vhci-hcd snd-usb-audio, puis\n"
      "usbip attach -r <adresse> -b 1-1.\n"
      "\n"
      "  -a, --address ADR   adresse d'écoute (défaut 0.0.0.0)\n"
      "  -p, --port PORT     port TCP (défaut 3240)\n"
      "  -d, --dump FICHIER  enregistre le PCM reçu (S16_LE, 4 canaux, 48 kHz)\n"
      "  -t, --haptics-to IP:PORT\n"
      "                      envoie les canaux haptiques 3-4 en RTP (L16 stéréo\n"
      "                      48 kHz, PT 127) vers le PC client\n"
      "  -g, --haptics-gain G gain des haptiques envoyées (écrêtage doux), défaut 1\n"
      "      --rtp-channels N  2 = haptiques seules (défaut), 4 = haut-parleur + haptiques\n"
      "      --haptics-gain-file FICHIER\n"
      "                      gain relu chaque seconde dans ce fichier (réglage à chaud)\n"
      "  -v, --verbose       journalise les requêtes de contrôle et les UNLINK\n"
      "                      (SIGUSR1 bascule ce mode en cours de route)\n"
      "  -h, --help          cette aide\n",
      argv0
    );
  }

}  // namespace

int main(int argc, char **argv) {
  UsbipServer::Options options;
  std::string dump_path;
  std::string haptics_to;
  float haptics_gain = 1.0f;
  std::string haptics_gain_file;

  static const option long_options[] {
    {"address", required_argument, nullptr, 'a'},
    {"port", required_argument, nullptr, 'p'},
    {"dump", required_argument, nullptr, 'd'},
    {"haptics-to", required_argument, nullptr, 't'},
    {"haptics-gain", required_argument, nullptr, 'g'},
    {"haptics-gain-file", required_argument, nullptr, 'G'},
    {"rtp-channels", required_argument, nullptr, 'C'},
    {"verbose", no_argument, nullptr, 'v'},
    {"help", no_argument, nullptr, 'h'},
    {nullptr, 0, nullptr, 0},
  };
  unsigned rtp_channels = 2;
  int c;
  while ((c = getopt_long(argc, argv, "a:p:d:t:g:vh", long_options, nullptr)) != -1) {
    switch (c) {
      case 'a':
        options.bind_address = optarg;
        break;
      case 'p':
        options.port = static_cast<std::uint16_t>(std::atoi(optarg));
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
      case 'C':
        rtp_channels = static_cast<unsigned>(std::atoi(optarg));
        break;
      case 'G':
        haptics_gain_file = optarg;
        break;
      case 'v':
        options.verbose = true;
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
  tools::g_verbose.store(options.verbose);
  options.verbose_flag = &tools::g_verbose;

  tools::AudioMeter meter(dump);
  HapticsRtpSender haptics(tools::log_stderr);
  if (!haptics_to.empty()) {
    if (!haptics.set_destination(haptics_to)) {
      return 1;
    }
    haptics.set_gain(haptics_gain, haptics_gain_file);
    haptics.set_channels(rtp_channels);
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

  UsbipServer server(
    device,
    options,
    {
      .audio_out =
        [&meter, &haptics](std::span<const std::uint8_t> pcm) {
          haptics.send(pcm);
          meter.feed(pcm);
        },
      .session = {},
    }
  );

  const int rc = server.run(tools::g_stop);
  meter.flush();
  if (dump) {
    std::fclose(dump);
  }
  return rc;
}
