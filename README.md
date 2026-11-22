# ds5-usbip

DualSense USB émulée par USB/IP, pour faire passer les **haptiques HD**
(audio, canaux 3-4) des jeux de SteamBox jusqu'à la vraie manette du client
Moonlight.

La DualSense virtuelle de Sunshine (`uhid`) n'a ni carte son ni parent USB :
les jeux ne lui trouvent aucune sortie haptique. Une DualSense émulée comme
un vrai périphérique USB (HID + carte son 4 canaux, branchée par
`vhci-hcd`) règle les deux ; ses canaux haptiques partent ensuite vers le PC
client et le DS5Dongle.

Écrit en Go (serveur USB/IP fait maison, transferts isochrones compris).

**État : plan, pas encore de code.** Voir [docs/plan.md](docs/plan.md).
