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

**État : phases 0 et 1 validées (22/11)** : RIDE 6 et F1 25 envoient leurs
haptiques HD à la DualSense émulée ; elles sont transmises en RTP au PC
client et ressenties dans la manette (DS5Dongle). Prochaine étape :
réglages durables, puis phase 2 (intégration dans Sunshine). Voir
[docs/plan.md](docs/plan.md).

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
