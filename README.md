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

**État : phase 0 validée (22/11)** : RIDE 6 et F1 25 envoient leurs
haptiques HD sur les canaux 3-4 de la carte son de la DualSense émulée.
Prochaine étape : phase 1, envoi des haptiques au PC client. Voir
[docs/plan.md](docs/plan.md).

```sh
make                              # build/ds5-usbip-proto (statique)
build/ds5-usbip-proto --help
tests/usbip_client_test.py        # test du protocole, serveur lancé
```

Code repris de vds et de libvirtualhid (MIT) : voir
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
