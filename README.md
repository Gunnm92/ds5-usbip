# ds5-usbip

DualSense USB émulée par USB/IP, pour faire passer les **haptiques HD**
(audio, canaux 3-4) des jeux de SteamBox jusqu'à la vraie manette du client
Moonlight.

La DualSense virtuelle de Sunshine (`uhid`) n'a ni carte son ni parent USB :
les jeux ne lui trouvent aucune sortie haptique. Une DualSense émulée comme
un vrai périphérique USB (HID + carte son 4 canaux, branchée par
`vhci-hcd`) règle les deux ; ses canaux haptiques partent ensuite vers le PC
client et le DS5Dongle.

Cible : un backend USB/IP intégré à Sunshine (libvirtualhid, C++), à
partir du code de [vds](https://github.com/hurryman2212/vds) (MIT) ; la
phase 0 est un prototype C++ autonome.

**État (23/11)** : phases 0 à 2 validées. La DualSense USB/IP est créée par
Sunshine à chaque connexion Moonlight ; les haptiques HD et le haut-parleur
partent en RTP vers l'adresse du client (automatique), avec une page
« DualSense » dans l'interface web de Sunshine. Latence côté client ~25 ms.
Reste à faire : [docs/plan.md](docs/plan.md).

## Qui porte quoi

| Dépôt | Rôle |
|---|---|
| **ds5-usbip** (ici) | Bibliothèque USB/IP de la DualSense (`src/usbip/`), outils de mise au point, correctifs qui la posent dans libvirtualhid (`libvirtualhid/`), plan et journal des essais. |
| [sunshine-ds](https://github.com/Gunnm92/sunshine-ds) | Sunshine construit avec ce backend : clone ds5-usbip et lance `libvirtualhid/apply.sh`, ajoute ses propres correctifs Sunshine (destination automatique, page DualSense), règle WirePlumber côté serveur, release. |
| [SteamBox](https://github.com/Gunnm92/SteamBox) | Image du serveur de jeu : installe la release sunshine-ds, accès à vhci-hcd. |
| AIBox | PC client : réception RTP, latence et volume du DS5Dongle, service de démarrage. |
| Hôte Unraid | Modules `vhci-hcd` et `snd-usb-audio` au démarrage. |

## Organisation

- `src/usbip/` : **bibliothèque** (namespace `ds5usbip`), copiable telle quelle
  dans un autre projet : protocole USB/IP, modèle de DualSense, session URB,
  helpers vhci-hcd, envoi RTP des haptiques. Aucune sortie console : les
  messages passent par un `LogFn`.
- `src/tools/` : programmes en ligne de commande (serveur TCP, attache, local).
- `libvirtualhid/` : `apply.sh` copie `src/usbip/` dans libvirtualhid et
  applique les correctifs (manette USB/IP, destination, réglages, état).
- `tests/cpp/` : tests C++ de la bibliothèque ; `tests/*.py` : tests Python
  (client USB/IP, mire RTP, relecture d'un enregistrement).

## Compilation

```sh
make                              # build/ds5-usbip-proto, build/ds5-usbip-attach,
                                  # build/ds5-usbip-local (statiques)
build/ds5-usbip-proto --help      # serveur USB/IP sur TCP (SteamBox)
build/ds5-usbip-attach status     # hôte : attache au vhci-hcd (remplace « usbip attach »)
build/ds5-usbip-local --help      # attache locale par socketpair, sans réseau
make test                         # tests C++ de la bibliothèque (build/session_test)
make check-cxx23                  # la bibliothèque compile aussi en C++23
tests/usbip_client_test.py        # test du protocole, serveur lancé (voir l'en-tête)
```

Réglages de la chaîne haptique : [docs/parametres.md](docs/parametres.md).

Code repris de vds et de libvirtualhid (MIT) : voir
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
