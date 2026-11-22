# ds5-usbip — plan

Haptiques HD de la DualSense dans SteamBox, pour tous les jeux, jusqu'à la
vraie manette du client Moonlight.

Rédigé le 22/11/2026, à partir des essais en direct de la session SteamBox
du 21-22/11. Démarrage prévu le 23/11.

## 1. Le problème

Une DualSense branchée en USB reçoit ses **haptiques HD comme un son** : sa
carte son USB a 4 canaux, les canaux 1-2 vont au haut-parleur et à la prise
casque, les canaux 3-4 aux deux actionneurs (voice coils). Les jeux récents
(RIDE 6, Stellar Blade, Spider-Man…) n'envoient leurs haptiques que par ce
chemin en USB.

Dans SteamBox, la manette du jeu est la DualSense virtuelle de Sunshine :

- créée par `uhid` (profil `dualsense_usb` de libvirtualhid, compilé dans
  l'image depuis le 02/10) ;
- **un périphérique HID seul, sans carte son** ;
- **sans parent USB réel** (elle vit sous `/devices/virtual/misc/uhid`).

Le protocole Moonlight transporte la vibration classique, la LED, le pavé,
le gyroscope et les gâchettes adaptatives (vérifié : RIDE 6, Star Wars,
F1 25), mais **aucun audio vers la manette**. Les haptiques HD n'ont donc
nulle part où aller.

Le contournement actuel de l'utilisateur (renvoyer tout le son de Sunshine
vers la manette côté client) fait vibrer la manette en permanence : il n'y a
pas de canal haptique dédié.

## 2. Ce que les essais du 22/11 ont établi

| Essai | Résultat |
|---|---|
| Sortie PipeWire virtuelle « DualSense Wireless Controller » 4 canaux (FL FR RL RR) dans le conteneur | Créée sans gêner Sunshine (sortie par défaut inchangée) |
| RIDE 6 sous proton-cachyos | N'ouvre que deux flux 6 canaux vers la sortie par défaut ; le second est du son de jeu (vérifié en le déplaçant : une partie du son disparaît) |
| RIDE 6 sous GE-Proton 11-12 (`--wine GE-Proton11-12-x86_64`) | Pareil : aucun flux 4 canaux |
| Sortie virtuelle avec `device.bus=usb`, `device.vendor.id=0x054c`, `device.product.id=0x0ce6` (créée par `pw-cli create-node`, le module PulseAudio tronque la description) | Toujours aucun flux 4 canaux |
| Journal GE-Proton | `err:hid:get_container_id_for_usb_udev_device Failed to get parent device.` |

**Cause** : les jeux choisissent la carte son de la manette par son
**ContainerId**, identique à celui de la manette HID. Wine (et GE-Proton, qui
lit `device.bus` / `device.vendor.id` / `device.product.id` côté PipeWire et
lie la sortie au « Sony controller container » actif) calcule ce conteneur à
partir du **périphérique USB parent**. La DualSense `uhid` n'en a pas : pas
de conteneur, donc aucune carte son liée, quelles que soient les propriétés
de la sortie.

**Conséquence** : il faut que la DualSense du jeu soit un **vrai
périphérique USB** (HID + carte son sous le même parent), comme une manette
branchée par câble.

## 3. Le noyau Unraid (vérifié le 22/11)

Sur l'hôte (`6.18.60-Unraid`) :

| Module | État |
|---|---|
| `vhci-hcd`, `usbip-core` | **présents** (`kernel/drivers/usb/usbip/`) |
| `snd-usb-audio` | **présent** |
| `dummy_hcd`, `raw_gadget`, `libcomposite` | absents |

Plugins déjà installés : `unraid.usbip`, `unraid.usbip-gui`,
`Usb_manager_usbip_addon`. Rien n'est chargé par défaut (seul `uhid` l'est).

→ **Voie retenue : USB/IP.** Un programme en espace utilisateur joue le rôle
de serveur USB/IP et exporte une DualSense USB complète ; `vhci-hcd` la
branche en local (`usbip attach -r 127.0.0.1`) ; le noyau la voit comme une
vraie manette USB : `hid-playstation`/hidraw pour le HID, `snd-usb-audio`
pour la carte son, sous le même parent USB.

La voie gadget USB (`dummy_hcd` + `libcomposite`) et le module dédié
[vds](https://github.com/hurryman2212/vds) (`vds_hcd`) demanderaient de
compiler un module pour le noyau Unraid : écartés tant que USB/IP suffit.

## 4. Architecture cible

```
Moonlight (manette du client)
        │ entrées, gâchettes, LED…
        ▼
Sunshine ──► DualSense émulée (serveur USB/IP, ce projet)
                 │  vhci-hcd (hôte) : vrai périphérique USB
                 ├─ interface HID  ─► jeux / Wine (manette)
                 └─ interface audio (UAC, 4 canaux 48 kHz)
                        ├─ canaux 1-2 : haut-parleur (option : renvoyés au client)
                        └─ canaux 3-4 : haptiques
                               │ flux réseau dédié (RTP PipeWire ou UDP simple)
                               ▼
                         PC client Linux ─► DS5Dongle-Studio, canaux 3-4,
                                            mode « Off » (transmission native)
```

Côté jeu, rien de spécial : Wine trouve le parent USB, lie la carte son à la
manette, le jeu y joue ses haptiques comme sur une DualSense filaire.

## 5. Phases

Chaque phase a un critère de validation ; on n'attaque la suivante qu'une
fois le critère atteint.

### Phase 0 — faisabilité (décisive)

But : prouver qu'une DualSense USB émulée en USB/IP obtient une carte son
et un conteneur Wine.

1. Écrire le serveur USB/IP **en Go** (choix du 22/11 : binaire unique,
   cohérent avec wsquashfs-launcher). Le protocole est simple et se code
   sans bibliothèque : `OP_REQ_DEVLIST`, `OP_REQ_IMPORT`, puis
   `USBIP_CMD_SUBMIT` / `USBIP_RET_SUBMIT` / `USBIP_CMD_UNLINK`, en-têtes
   big-endian sur TCP (port 3240). L'écrire nous-mêmes règle le risque des
   **transferts isochrones** (indispensables à l'audio USB) : tableau
   `iso_packet_descriptor` géré directement. Commencer par le contrôle
   (descripteurs, `SET_CONFIGURATION`, `SET_INTERFACE`), puis l'interruption
   (HID), puis l'isochrone (audio).
2. Prototype : descripteurs d'une DualSense USB (`054c:0ce6`) recopiés d'une
   vraie manette (`lsusb -v` côté client, ou descripteurs de libvirtualhid
   pour le HID) : interface HID + interfaces audio UAC1 (sortie 4 canaux
   48 kHz 16 bits, entrée micro 2 canaux en option).
3. Sur l'hôte : `modprobe vhci-hcd snd-usb-audio` (à la main, par
   l'utilisateur), `usbip attach -r 127.0.0.1 -b <busid>`.
4. Dans le conteneur : vérifier la carte son (`aplay -l`, sortie PipeWire
   avec `device.vendor.id=0x054c`), le hidraw, et lancer RIDE 6 sous
   GE-Proton.

**Critère** : RIDE 6 ouvre un flux **4 canaux** vers la carte son de la
DualSense émulée, et le serveur reçoit des échantillons non nuls sur les
canaux 3-4 qui suivent la moto (vibreurs, freinage).

Si l'isochrone pose problème en USB/IP, ou si le conteneur ne peut pas voir
les nouveaux périphériques, on s'arrête là et on réévalue (vds, gadget).

### Phase 1 — haptiques jusqu'au client

1. Le serveur extrait les canaux 3-4 et les envoie au PC client (RTP
   PipeWire `module-rtp-sink`/`-source`, ou UDP simple ; viser < 20 ms).
2. Côté client : réception et lecture sur les canaux 3-4 du DS5Dongle
   (mode « Off », transmission native, réglé dans le portail web
   `ds5-config-portal.html`) ; arrêter le renvoi de tout le son vers la
   manette.

**Critère** : haptiques ressenties dans RIDE 6, sans vibration permanente.

### Phase 2 — intégration avec Sunshine

Deux options, à trancher après la phase 0 :

- **A. Sunshine crée la DualSense USB/IP** : nouveau profil/backend dans
  libvirtualhid (Sunshine est déjà compilé dans l'image, étape
  `sunshine-usb` du Dockerfile). Propre, une seule manette.
- **B. Pont** : la DualSense `uhid` de Sunshine reste, ce projet recopie ses
  rapports d'entrée vers la DualSense USB et renvoie les rapports de sortie
  du jeu (vibration, gâchettes, LED) vers elle pour que Sunshine les
  transmette. Il faut alors cacher la manette `uhid` aux jeux (sinon deux
  DualSense). Modèle existant : `dualsense-usb-bridge` de
  wsquashfs-launcher.

**Critère** : une seule DualSense vue par les jeux, entrées, gâchettes
adaptatives, LED et vibration classique toujours fonctionnelles, haptiques
HD en plus.

### Phase 3 — mise en production

- Service s6 dans SteamBox (démarrage avec une session Moonlight, arrêt à la
  déconnexion), accès du conteneur aux nouveaux périphériques (hidraw,
  `/dev/snd`) dans le compose et les profils.
- Chargement des modules à l'amorçage d'Unraid (fichier `go` ou plugin) :
  documenté, fait par l'utilisateur.
- Composant client empaqueté (service systemd utilisateur).
- README, `--help`, documentation SteamBox à jour.

## 6. Risques et questions ouvertes

- **Isochrone en USB/IP** : le point le plus incertain (phase 0).
- **Accès du conteneur** aux périphériques créés après son démarrage
  (règles de cgroup pour hidraw et ALSA, `/dev` dynamique).
- **Sunshine** recrée sa manette à chaque connexion : la DualSense USB/IP
  doit suivre ce cycle de vie.
- **Jeux natifs Linux** (Hookshot) : cherchent une sortie SDL nommée
  « DualSense » ou « Wireless Controller » avec au moins 4 canaux ; la carte
  son émulée devrait leur convenir aussi.
- **Latence** des haptiques sur le réseau local, et synchronisation avec
  l'image.
- **Steam Input** : vérifier qu'il ne masque pas la DualSense USB émulée
  aux jeux lancés depuis Steam.
- `hid-playstation` sur l'hôte prendra la manette émulée (vibration force
  feedback) : sans gêne attendue, à confirmer.

## 7. Références

- Essais et journaux : session SteamBox du 21-22/11/2026 ; TODO SteamBox,
  entrées « DualSense » et « Haptiques audio de la DualSense ».
- DS5Dongle-Studio (dongle du client) : <https://github.com/artzox/DS5Dongle-Studio>
  — 4 canaux reçus, canaux 3-4 = haptiques natives, mode « Off » =
  transmission native. Sources awalol et loteran du dongle :
  `/config/workspace/DS5Dongle/`.
- Wine, ContainerId des cartes son DualSense :
  <https://www.winehq.org/pipermail/wine-devel/2022-July/222153.html>,
  <https://list.winehq.org/archives/list/wine-gitlab@list.winehq.org/thread/YZJHGUGKMFHFEOB537L3YR6G2MUVGJDK>
- GE-Proton 11-2 (haptiques DualSense en USB filaire) :
  <https://www.linuxcompatible.org/story/geproton112-released-adds-wired-ps5-haptics-and-marvel-rivals-fix/>
- vds (alternative par module noyau) : <https://github.com/hurryman2212/vds>
- Protocole USB/IP : documentation du noyau Linux,
  `Documentation/usb/usbip_protocol.rst`.
