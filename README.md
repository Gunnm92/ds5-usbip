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

**État : phases 0, 1 et 2 validées (22/11)** : la DualSense USB/IP est
intégrée à Sunshine (projet [sunshine-ds](https://github.com/Gunnm92/sunshine-ds),
backend libvirtualhid), créée à chaque connexion Moonlight ; les haptiques HD
des jeux sont ressenties dans la manette du client. Reste à faire : voir
[docs/plan.md](docs/plan.md), « Bilan au 22/11 ».

## Organisation

- `src/usbip/` : **bibliothèque** (namespace `ds5usbip`), copiable telle quelle
  dans un autre projet : protocole USB/IP, modèle de DualSense, session URB,
  helpers vhci-hcd, envoi RTP des haptiques. Aucune sortie console : les
  messages passent par un `LogFn`.
- `src/tools/` : programmes en ligne de commande (serveur TCP, attache, local).
- `tests/cpp/` : tests C++ de la bibliothèque ; `tests/*.py` : tests Python.

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
