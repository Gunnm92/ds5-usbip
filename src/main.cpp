// SPDX-License-Identifier: MIT
// ds5-usbip-proto : prototype de la phase 0. Exporte une DualSense USB
// (HID + carte son 4 canaux) par USB/IP, sans Sunshine, et affiche chaque
// seconde le niveau des canaux reçus (0-1 haut-parleur, 2-3 haptiques).
#include <fcntl.h>
#include <dirent.h>
#include <getopt.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <climits>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dualsense_device.hpp"
#include "haptics_rtp.hpp"
#include "usbip_server.hpp"

namespace {

  std::atomic<bool> g_stop {false};
  std::atomic<bool> g_verbose {false};

  void on_signal(int) {
    g_stop.store(true);
  }

  // SIGUSR1 : bascule le mode verbeux sans couper la session.
  void on_usr1(int) {
    g_verbose.store(!g_verbose.load());
  }

  // Statistiques par fenêtre d'une seconde (48 000 trames).
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

  // Test de la phase 0 uniquement : recopie les rapports d'entrée d'une
  // autre DualSense USB (celle de Sunshine, /dev/hidrawN) dans la manette
  // émulée, et renvoie vers elle les rapports de sortie du jeu (vibration,
  // gâchettes, LED). En phase 2, Sunshine alimente directement la manette.
  //
  // « auto » : cherche la DualSense uhid (celle de Sunshine) et la retrouve
  // quand Sunshine la recrée (reconnexion Moonlight). Avec hide, son nœud
  // /dev/hidrawN est supprimé pour que le jeu ne voie que la manette émulée.
  class Mirror {
  public:
    Mirror(std::string path, bool hide):
        path_(std::move(path)),
        hide_(hide) {}

    bool enabled() const {
      return !path_.empty();
    }

    void start(ds5usbip::DualSenseDevice &device, const std::atomic<bool> &stop) {
      writer_ = std::thread([this] {
        std::vector<std::uint8_t> report;
        while (true) {
          {
            std::unique_lock lock(out_mutex_);
            out_cv_.wait(lock, [this] {
              return !queue_.empty() || out_stop_;
            });
            if (out_stop_) {
              return;
            }
            report = std::move(queue_.front());
            queue_.pop_front();
            if (dropped_) {
              std::fprintf(stderr, "miroir : %zu rapports de sortie perdus (file pleine)\n", dropped_);
              dropped_ = 0;
            }
          }
          const int fd = fd_.load();
          if (fd >= 0 && ::write(fd, report.data(), report.size()) < 0 && errno != ENODEV) {
            std::fprintf(stderr, "miroir : écriture : %s\n", std::strerror(errno));
          }
        }
      });
      thread_ = std::thread([this, &device, &stop] {
        ds5usbip::DualSenseDevice::Report r {};
        while (!stop.load()) {
          if (fd_.load() < 0 && !reopen()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
          }
          pollfd p {fd_.load(), POLLIN, 0};
          if (::poll(&p, 1, 200) <= 0) {
            continue;
          }
          const ssize_t n = ::read(p.fd, r.data(), r.size());
          if (n < 0) {
            std::fprintf(stderr, "miroir : lecture : %s, recherche d'une nouvelle manette\n", std::strerror(errno));
            ::close(fd_.exchange(-1));
            continue;
          }
          if (n >= 10 && r[0] == ds5usbip::dualsense::kInputReportId) {
            device.set_input_report(r);
          }
        }
      });
    }

    // Appelé depuis le fil USB/IP : ne bloque jamais. Les rapports sont
    // mis en file pour le fil d'écriture (un write() direct sur hidraw
    // depuis le fil USB/IP a provoqué un interblocage : le noyau, verrou
    // hidraw pris, attendait de ce même fil l'annulation d'un URB). Aucun
    // rapport n'est fusionné : certains jeux (RIDE 6) n'envoient un effet
    // de gâchette qu'une fois ; seule une file pleine fait perdre le plus
    // ancien.
    void output(std::span<const std::uint8_t> report) {
      {
        std::lock_guard lock(out_mutex_);
        if (queue_.size() >= kMaxQueued) {
          queue_.pop_front();
          ++dropped_;
        }
        queue_.emplace_back(report.begin(), report.end());
      }
      out_cv_.notify_one();
    }

    void join() {
      {
        std::lock_guard lock(out_mutex_);
        out_stop_ = true;
      }
      out_cv_.notify_one();
      if (thread_.joinable()) {
        thread_.join();
      }
      if (writer_.joinable()) {
        writer_.join();
      }
    }

  private:
    // DualSense créée par uhid (Sunshine) : /sys/class/hidraw/X/device
    // pointe sous /devices/virtual/misc/uhid/0003:054C:0CE6.*
    // Renvoie le nœud et, dans id, l'instance HID (0003:054C:0CE6.NNNN),
    // unique même si Sunshine recrée sa manette sous le même numéro hidraw.
    static std::string find_uhid_dualsense(std::string &id) {
      DIR *d = ::opendir("/sys/class/hidraw");
      if (!d) {
        return {};
      }
      std::string found;
      while (dirent *e = ::readdir(d)) {
        if (e->d_name[0] == '.') {
          continue;
        }
        // Lien relatif : realpath pour obtenir le chemin complet sous /sys.
        char target[PATH_MAX];
        const std::string link = std::string("/sys/class/hidraw/") + e->d_name + "/device";
        if (!::realpath(link.c_str(), target)) {
          continue;
        }
        if (std::strstr(target, "/uhid/") && std::strstr(target, "054C:0CE6")) {
          found = std::string("/dev/") + e->d_name;
          id = target;
        }
      }
      ::closedir(d);
      return found;
    }

    // Ouvre la manette d'après /sys/class/hidraw/N/dev (nœud privé).
    static int open_node(const std::string &path) {
      // Toujours par un nœud privé : /dev/hidrawN peut être absent ou déjà
      // remplacé par le leurre d'un lancement précédent.
      if (path.rfind("/dev/hidraw", 0) != 0) {
        return ::open(path.c_str(), O_RDWR | O_CLOEXEC);
      }
      int fd = -1;
      const std::string name = path.substr(5);
      std::FILE *f = std::fopen(("/sys/class/hidraw/" + name + "/dev").c_str(), "r");
      unsigned maj = 0, min = 0;
      const bool ok = f && std::fscanf(f, "%u:%u", &maj, &min) == 2;
      if (f) {
        std::fclose(f);
      }
      if (!ok) {
        return -1;
      }
      const std::string tmp = "/tmp/ds5-usbip-mirror-" + name;
      ::unlink(tmp.c_str());
      if (::mknod(tmp.c_str(), S_IFCHR | 0600, makedev(maj, min)) < 0) {
        std::fprintf(stderr, "miroir : mknod %s : %s\n", tmp.c_str(), std::strerror(errno));
        return -1;
      }
      fd = ::open(tmp.c_str(), O_RDWR | O_CLOEXEC);
      ::unlink(tmp.c_str());
      return fd;
    }

    bool reopen() {
      std::string id = path_;
      const std::string path = path_ == "auto" ? find_uhid_dualsense(id) : path_;
      if (path.empty() || id == last_id_) {
        return false;  // pas encore de manette, ou l'ancienne (supprimée)
      }
      const int fd = open_node(path);
      if (fd < 0) {
        return false;
      }
      last_id_ = id;
      fd_.store(fd);
      std::fprintf(stderr, "miroir : entrées lues sur %s\n", path.c_str());
      if (hide_) {
        // Leurre plutôt que suppression : le lanceur wsquashfs exige que
        // toute manette Sony de /sys/class/hidraw s'ouvre dans /dev, sinon
        // il prive GE-Proton de toutes les manettes Sony. Un nœud vers
        // /dev/null s'ouvre, mais Wine l'écarte (pas de descripteur HID).
        ::unlink(path.c_str());
        if (::mknod(path.c_str(), S_IFCHR | 0666, makedev(1, 3)) == 0) {
          ::chmod(path.c_str(), 0666);
          std::fprintf(stderr, "miroir : %s masqué (leurre /dev/null)\n", path.c_str());
        } else {
          std::fprintf(stderr, "miroir : masquage de %s impossible : %s\n", path.c_str(), std::strerror(errno));
        }
      }
      return true;
    }

    std::string path_;
    bool hide_;
    std::string last_id_;
    std::atomic<int> fd_ {-1};
    std::thread thread_;
    std::thread writer_;
    std::mutex out_mutex_;
    std::condition_variable out_cv_;
    static constexpr std::size_t kMaxQueued = 64;
    std::deque<std::vector<std::uint8_t>> queue_;
    std::size_t dropped_ = 0;
    bool out_stop_ = false;
  };

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
      "      --haptics-gain-file FICHIER\n"
      "                      gain relu chaque seconde dans ce fichier (réglage à chaud)\n"
      "  -m, --mirror HIDRAW recopie les entrées de cette DualSense (test) ;\n"
      "                      « auto » : la DualSense uhid de Sunshine, suivie\n"
      "      --hide-mirror   remplace ensuite le nœud HIDRAW par un leurre /dev/null\n"
      "                      (conteneur seulement)\n"
      "  -v, --verbose       journalise les requêtes de contrôle et les UNLINK\n"
      "                      (SIGUSR1 bascule ce mode en cours de route)\n"
      "  -h, --help          cette aide\n",
      argv0
    );
  }

}  // namespace

int main(int argc, char **argv) {
  ds5usbip::UsbipServer::Options options;
  std::string dump_path;
  std::string mirror_path;
  std::string haptics_to;
  float haptics_gain = 1.0f;
  std::string haptics_gain_file;
  bool hide_mirror = false;

  static const option long_options[] {
    {"address", required_argument, nullptr, 'a'},
    {"port", required_argument, nullptr, 'p'},
    {"dump", required_argument, nullptr, 'd'},
    {"haptics-to", required_argument, nullptr, 't'},
    {"haptics-gain", required_argument, nullptr, 'g'},
    {"haptics-gain-file", required_argument, nullptr, 'G'},
    {"mirror", required_argument, nullptr, 'm'},
    {"hide-mirror", no_argument, nullptr, 'H'},
    {"verbose", no_argument, nullptr, 'v'},
    {"help", no_argument, nullptr, 'h'},
    {nullptr, 0, nullptr, 0},
  };
  int c;
  while ((c = getopt_long(argc, argv, "a:p:d:t:g:m:vh", long_options, nullptr)) != -1) {
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
      case 'G':
        haptics_gain_file = optarg;
        break;
      case 'm':
        mirror_path = optarg;
        break;
      case 'H':
        hide_mirror = true;
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

  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  struct sigaction su {};
  su.sa_handler = on_usr1;
  su.sa_flags = SA_RESTART;
  sigaction(SIGUSR1, &su, nullptr);
  g_verbose.store(options.verbose);
  options.verbose_flag = &g_verbose;

  AudioMeter meter(dump);
  ds5usbip::HapticsRtpSender haptics;
  if (!haptics_to.empty()) {
    if (!haptics.open(haptics_to)) {
      return 1;
    }
    haptics.set_gain(haptics_gain, haptics_gain_file);
  }
  Mirror mirror(mirror_path, hide_mirror);

  ds5usbip::DualSenseDevice device({
    .output_report =
      [&mirror](std::span<const std::uint8_t> r) {
        if (g_verbose.load()) {
          std::fprintf(stderr, "hid: rapport de sortie 0x%02x, %zu octets\n", r[0], r.size());
        }
        mirror.output(r);
      },
    .feature_set =
      [](std::span<const std::uint8_t> r) {
        std::fprintf(stderr, "hid: SET_REPORT(feature 0x%02x) %zu octets\n", r[0], r.size());
      },
    .interface_changed =
      [&meter](std::uint8_t intf, std::uint8_t alt) {
        const char *name = intf == ds5usbip::dualsense::kAudioOutInterface ? "sortie (HP + haptiques)" : "entrée (micro)";
        std::fprintf(stderr, "audio: flux %s %s\n", name, alt ? "ouvert" : "fermé");
        if (!alt) {
          meter.flush();
        }
      },
  });

  ds5usbip::UsbipServer server(
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

  if (mirror.enabled()) {
    mirror.start(device, g_stop);
  }
  const int rc = server.run(g_stop);
  mirror.join();
  meter.flush();
  if (dump) {
    std::fclose(dump);
  }
  return rc;
}
