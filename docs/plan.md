# ds5-usbip — plan

Haptiques HD de la DualSense dans SteamBox, pour tous les jeux, jusqu'à la
vraie manette du client Moonlight.

Rédigé le 22/11/2026, à partir des essais en direct de la session SteamBox
du 21-22/11. Démarrage prévu le 23/11.

**Décision (22/11)** : la DualSense USB/IP sera **intégrée à Sunshine**
(nouveau backend de libvirtualhid, C++), pour supprimer une couche : une
seule manette, pas de pont entre la DualSense `uhid` et une DualSense USB, et
le cycle de vie de la session Moonlight gratuit. Le prototype de la phase 0
est écrit en C++ sur la base du code de vds, pour devenir ensuite le cœur de
ce backend. (Une version Go autonome avait d'abord été envisagée : écartée,
elle imposait le pont et ses deux manettes.)

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

### Ce qu'on reprend de vds (MIT)

[vds](https://github.com/hurryman2212/vds) émule déjà la DualSense USB
voulue — interface HID + carte son UAC1 **4 canaux, 48 kHz, S16_LE**
(canaux 0-1 haut-parleur, 2-3 haptiques, en numérotation à partir de 0) —
et l'exporte **par USB/IP sous Windows** (usbip-win2,
`usbip://127.0.0.1:3240/1-1`) : le modèle de périphérique est donc éprouvé à
travers USB/IP, isochrone compris. Sous Linux il utilise son propre HCD
uniquement parce que `dummy_hcd` ne gère pas l'isochrone ; `vhci-hcd`, lui,
le gère.

À reprendre en C++ (avec crédit, licence MIT) :

- descripteurs et profil : `module/dualsense.c`, `module/vds_controller.h`,
  `include/vds/ds5_protocol.h`, `include/uapi/vds.h` ;
- modèle de périphérique USB et requêtes de classe HID / UAC1 (muet,
  volume en 8.8 dB de -100 à 0 dB) : `module/vds_usb_core.c` ;
- intervalles des points de terminaison HID : 4 ms en entrée et en sortie
  (DualSense) ;
- côté Windows, son export USB/IP (`src/platform/`) : à lire pour la gestion
  de l'isochrone avant d'écrire la nôtre.

Ce qu'on ne reprend pas : la partie Bluetooth (vds pilote une vraie
DualSense par L2CAP et convertit les haptiques en rapports Bluetooth, code
dérivé de DS5Dongle d'awalol). Ici la source est la manette de Sunshine, et
les haptiques partent en PCM brut vers le DS5Dongle du client.

## 4. Architecture cible

```
Moonlight (manette du client)
        │ entrées, gâchettes, LED…
        ▼
Sunshine ──► DualSense émulée (backend USB/IP de libvirtualhid, ce projet)
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

**État (22/11)** :

- [x] 1-2. Serveur USB/IP C++ et profil DualSense : `src/` (descripteurs
  extraits de vds, rapports feature 0x05/0x09/0x20 de libvirtualhid), binaire
  statique `build/ds5-usbip-proto` (`make`). Contrôle, HID IN toutes les
  4 ms, HID OUT, isochrone OUT et IN rythmés à 1 ms par paquet, UNLINK.
  Validé par `tests/usbip_client_test.py` (client qui imite `vhci-hcd`,
  28 vérifications). Le prototype affiche chaque seconde le niveau des
  4 canaux et peut enregistrer le PCM (`--dump`).
- [ ] 3. Attache sur l'hôte (serveur lancé dans le conteneur code-server).
  L'outil `usbip` n'est pas dans le PATH d'Unraid malgré le plugin :
  remplacé par `build/ds5-usbip-attach` (import USB/IP puis écriture dans
  `/sys/devices/platform/vhci_hcd.0/attach`), base de l'attache par
  Sunshine en phase 2.
  Énumération validée sur l'hôte : carte `Controller` (lecture 4 canaux
  48 kHz S16_LE FL FR RL RR, capture 2 canaux), hidraw sous le même parent
  USB `3-1` que la carte son, `hid-playstation` lié (rapports feature lus,
  LED reçue). Isochrone validé : `aplay -D hw:Controller -c 4 -r 48000
  -f S16_LE -d 5 /dev/urandom` (conteneur alpine jetable avec `/dev/snd`) →
  48 000 trames/s reçues, 5 s exactement, signal sur les 4 canaux, niveau
  RMS conforme (-4,8 dB) : ni perte ni décalage. **Étape 3 validée.**
- [ ] 4. SteamBox : accès à ALSA ajouté (règle cgroup `c 116:* rmw` dans
  le modèle Unraid, `docs/deploiement-unraid.md` et le compose ; règle udev
  `99-steambox-snd-fallback.rules` dans l'image). `/sys` étant en lecture
  seule dans le conteneur, la manette doit être attachée après son
  démarrage. Vérifié : `/dev/snd/*` (groupe audio), `/dev/hidraw5`
  (groupe input), sortie PipeWire 4 canaux FL FR RL RR avec
  `device.bus=usb`, `device.vendor.id=0x054c`, `device.product.id=0x0ce6`.
  Points ouverts : la sortie devient celle par défaut hors session ; deux
  DualSense pendant une session (celle de Sunshine et l'émulée). Reste :
  RIDE 6 sous GE-Proton.
  1er essai RIDE 6 : Wine ouvre les deux hidraw, le jeu prend la manette
  active (Sunshine, sans carte son) et n'ouvre que ses flux 6 canaux.
  Pour le test, option `--mirror /dev/hidrawN --hide-mirror` : le
  prototype, lancé dans SteamBox (127.0.0.1), recopie les entrées de la
  manette Sunshine, lui renvoie les rapports de sortie, et masque son nœud.
  L'API Docker refuse la copie de fichiers (403) : binaire passé par le
  partage Game (`/config/games/Batocera` dans SteamBox).
  2e essai (RIDE 6 puis F1 25, manette émulée seule visible) : Wine crée
  `HID\VID_054C&PID_0CE6&MI_03\…35:53:3A:D6:E7:74` avec un vrai
  **ContainerId** `{0CE6054C-0003-0004-DDAC-18C70B000000}` (objectif Wine
  atteint), mais les jeux ne détectent pas la manette. Piste : Steam Input
  la déclare dans `virtualgamepadinfo.txt` (handle = MAC), Proton la cache
  au jeu, et aucune manette virtuelle Steam n'existe. Steam Input était
  pourtant déjà désactivé pour le jeu. En fait, Sunshine avait recréé sa
  manette (reconnexion Moonlight, `hidraw7`) : recopie perdue, la nouvelle
  non masquée et reprise par le jeu. D'où `--mirror auto` : trouve la
  DualSense uhid, la suit à chaque recréation et la masque.
  Puis plus aucune requête du jeu vers la manette (journal verbeux vide) :
  cause = le masquage par suppression du nœud. Le lanceur wsquashfs
  (`hidrawUsable`) exige que toute manette Sony de `/sys/class/hidraw`
  s'ouvre dans `/dev`, sinon il laisse `DisableHidraw=1` sans
  `PROTON_SONY_HIDRAW_XINPUT` : GE-Proton ignore alors toutes les manettes
  Sony. Masquage refait par un leurre (nœud vers `/dev/null`, que Wine
  écarte faute de descripteur HID).
  **3e essai RIDE 6 : critère haptique atteint.** Le jeu lit les rapports
  feature 0x09/0x20/0x05, écrit 0x08, envoie des rapports de sortie 0x02,
  et ouvre un flux **4 canaux s16le** (« PipeWire ALSA [wine64-preloader] »)
  vers la carte de la manette. En roulant : signal sur les canaux 3-4
  seulement (pointes 500-1900, -35 à -56 dB RMS, variable), canaux 1-2
  muets. PCM enregistré dans `/tmp/ds5-audio.raw` (SteamBox). Reste :
  direction qui décroche par moments (recopie des entrées, à diagnostiquer).
  Analyse de l'enregistrement (220 s de flux, 109 s avec signal) : canaux
  1-2 toujours nuls ; canaux 3-4 indépendants (gauche seule, droite seule,
  les deux), pics jusqu'à 1 940, plages stables ~200 RMS : profil
  d'haptiques de conduite. **Critère de la phase 0 atteint** (ressenti à
  confirmer en phase 1).
  Décrochages de direction expliqués : interblocage. Le fil USB/IP écrivait
  lui-même les rapports de sortie vers la manette Sunshine (`write` sur
  hidraw) ; le noyau, verrou global hidraw pris pour une fermeture,
  attendait de ce fil l'annulation d'un URB. Tout hidraw figé (jeu, lanceur,
  PipeWire) jusqu'au détachement. Corrigé : fil d'écriture dédié, seul le
  dernier rapport est gardé. Leçon pour la phase 2 : ne jamais bloquer le
  fil USB/IP.
  Revérification avec F1 25 (correctif en place) : flux 4 canaux, 169 s
  d'haptiques sur 315 s (pic 2 097), canaux 1-2 muets ; gâchettes
  adaptatives fonctionnelles (40 000 rapports de sortie renvoyés à
  Sunshine) ; direction fiable, plus aucun blocage.
- [x] **Phase 0 validée (22/11)** sur RIDE 6 et F1 25.

Ce que la phase 0 apprend pour la suite :
- le lanceur wsquashfs exige que toute manette Sony de `/sys/class/hidraw`
  soit ouvrable dans `/dev` (sinon GE-Proton ignore les manettes Sony) ;
- la manette doit être branchée après le démarrage de SteamBox (`/sys` en
  lecture seule, pas de rejeu d'uevent) ;
- en phase 2, la DualSense `uhid` de Sunshine disparaît : plus de miroir ni
  de leurre, mais le fil USB/IP ne doit jamais bloquer ;
- hors session, la sortie DualSense devient la sortie par défaut (Sunshine
  impose la sienne pendant une session).

1. Écrire le serveur USB/IP **en C++**, sous forme d'un prototype autonome
   (sans Sunshine) dont le code passera tel quel dans libvirtualhid. Partir
   de vds : modèle de périphérique (`module/vds_usb_core.c`), profil
   DualSense, et son export USB/IP Windows (`src/platform/`). Le protocole
   est simple : `OP_REQ_DEVLIST`, `OP_REQ_IMPORT`, puis `USBIP_CMD_SUBMIT` /
   `USBIP_RET_SUBMIT` / `USBIP_CMD_UNLINK`, en-têtes big-endian sur TCP
   (port 3240). Gérer nous-mêmes les **transferts isochrones**
   (indispensables à l'audio USB) : tableau `iso_packet_descriptor`.
   Commencer par le contrôle (descripteurs, `SET_CONFIGURATION`,
   `SET_INTERFACE`), puis l'interruption (HID), puis l'isochrone (audio).
2. Prototype : descripteurs d'une DualSense USB (`054c:0ce6`) portés depuis
   vds (`module/dualsense.c`, voir section 3), à recouper si besoin avec une
   vraie manette (`lsusb -v` côté client) : interface HID + interfaces audio
   UAC1 (sortie 4 canaux 48 kHz S16_LE, entrée micro en option).
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

**État (22/11)** : émetteur écrit, `--haptics-to IP:PORT`
(`src/haptics_rtp.*`) : canaux 3-4 en RTP L16 stéréo big-endian 48 kHz,
PT 127 (défaut des modules RTP de PipeWire), un paquet par milliseconde,
DSCP EF. Vérifié par `tests/rtp_receiver_check.py` (séquence, horodatage,
contenu). Côté client prévu : `module-rtp-source` de PipeWire (≥ 0.3.60,
sans SAP : `source.port`, `audio.format S16BE`, `audio.position [ RL RR ]`,
`sess.latency.msec` ~10), dirigé vers la sortie 4 canaux du dongle
(firmware : 4 canaux reçus).
Client = AIBox (PipeWire 1.6.11). Sa config DS5Dongle-Studio : sortie
pro-audio `ds5_dongle_sink` (AUX0-AUX3), service `ds5-haptics-loopback`
qui renvoie le son du jeu sur AUX0/AUX1 (vibration « auto », la vibration
permanente). Haptiques natives = AUX2/AUX3. Config client :
`client/pipewire/ds5-usbip-haptics.conf` (port 46100). SteamBox → AIBox
(10.10.10.x) : 25 ms aller-retour, à surveiller pour la latence.
Mise au point (22/11) : profil « Direct » du dongle = canaux FL FR RL RR
(pas AUX), d'où `audio.position [ RL RR ]`. Paquets bien reçus (test UDP
3 000/3 000), mais rien joué : à `sess.latency.msec = 10`, tampon plus
court que le délai de la sortie du dongle, module-rtp-source le ramène à 0
et ne joue que du silence. À 40 ms : vibration de test
(`tests/rtp_test_pattern.py`) ressentie. Latence totale ~50 ms : à réduire
(mesurer le délai réel de la sortie). Attention : redémarrer PipeWire sur
le client coupe le son de Moonlight (quitter la session avant).

1. Le serveur extrait les canaux 3-4 et les envoie au PC client (RTP
   PipeWire `module-rtp-sink`/`-source`, ou UDP simple ; viser < 20 ms).
2. Côté client : réception et lecture sur les canaux 3-4 du DS5Dongle
   (mode « Off », transmission native, réglé dans le portail web
   `ds5-config-portal.html`) ; arrêter le renvoi de tout le son vers la
   manette.

**Critère** : haptiques ressenties dans RIDE 6, sans vibration permanente.
→ atteint le 22/11 (ressenti sur F1 25, voir plus haut).

**Point d'arrêt (22/11, 14 h)** — la chaîne fonctionne avec la vibration
de test (sinus 80 Hz, amplitude 20 000), mais **en jeu (RIDE 6, tampon
40 ms) l'utilisateur ne sent pas de différence**, alors que le jeu envoie
bien ses haptiques (21 s sur 40 avec signal, émises en RTP).

Pistes pour la reprise, dans l'ordre :
1. **Niveau du signal.** Haptiques du jeu faibles : pics 500-2 000,
   -35 à -56 dB RMS (test : 20 000). La sortie DualSense de SteamBox est à
   **40 % (-23,9 dB)**, volume matériel (`HARDWARE DECIBEL_VOLUME`, unité de
   fonction UAC du périphérique émulé, que le serveur ignore) : vérifier
   que PipeWire n'atténue pas en plus en logiciel (mesurer les pics à 40 %
   puis à 100 %). Une vraie DualSense applique ce volume au haut-parleur
   seulement (vds : gain sur les canaux 0-1). Si besoin, gain réglable à
   l'émission (`--haptics-gain`) et rejouer l'enregistrement RIDE 6 vers le
   client pour comparer au ressenti.
2. **Conversion du dongle.** Il ramène les haptiques à 3 kHz pour le
   Bluetooth (rapports 0x36, échantillons 8 bits dans vds/DS5Dongle) : un
   signal à -45 dB n'y laisse que quelques pas de quantification. Lire le
   code de DS5Dongle-Studio (mode « Off », gain éventuel).
3. **Latence.** ~50 ms (40 ms de tampon + ~12 ms de réseau) : mesurer le
   délai réel de la sortie du dongle (`pw-top`, propriétés de latence du
   nœud) et descendre le tampon au minimum qui joue encore.
4. Rejouer l'enregistrement RIDE 6 (`/config/workspace/ride6-haptics-20261122.raw`,
   hors dépôt, S16_LE 4 canaux 48 kHz, 220 s) en RTP vers le client, avec
   et sans gain, pour séparer « niveau » et « contenu ».

**Reprise (22/11, 16 h 45)** : piste 1 confirmée. Les pics de tout
l'enregistrement plafonnent à ~1 940 = -24,5 dB sous la pleine échelle,
soit le volume de 40 % de la sortie DualSense de SteamBox : PipeWire
atténue les haptiques (malgré `HARDWARE DECIBEL_VOLUME`). Relecture du
passage 95-110 s en RTP (`tests/rtp_replay.py`) : ×1 rien, ×16 ressenti
(léger). Corrigé à chaud : sortie à 100 % (`pactl set-sink-volume`,
mémorisé par WirePlumber). À rendre durable (règle WirePlumber dans
SteamBox : volume 1.0 pour la sortie 054c:0ce6, ou volume matériel seul).
Seconde atténuation trouvée par l'utilisateur : `ds5_dongle_sink` sur
AIBox était aussi à **40 % (-23,9 dB)** ; cumul -48 dB en jeu. Mise à
100 % (à rendre durable côté AIBox : règle WirePlumber, volume 1.0).
Relecture à volumes pleins : ressenti, mais faible. Firmware du dongle :
`haptics_gain` appliqué aux canaux 3-4, borné à [1.0, 2.0] (portail
« Haptics Gain », défaut 1.0), puis conversion int8 à 3 kHz. Au-delà :
volume du flux > 100 % sur le client pour doser, puis `--haptics-gain`
(écrêtage doux) dans le serveur.
Réglages DS5Dongle-Studio relevés (22/11) : Mode Off ; les autres
réglages visibles (Intensity, Noise Gate, Speaker Effect Leak…) ne
concernent que l'auto-haptique et la fuite vers le haut-parleur ;
« Haptics Gain » (1.0-2.0) à retrouver dans le portail. Relecture ×1 /
×16 / ×32 envoyée, ressenti non noté. Firmware officiel (awalol) : même
chemin natif (canaux 3-4 → 3 kHz → int8 × 127 × haptics_gain), donc pas
plus fort ; garder Studio. Prochaine étape : `--haptics-gain` côté serveur,
réglé au ressenti, puis rendre durables les volumes à 100 % (SteamBox et
AIBox) et la config client.
Le client avait aussi perdu sa config entre-temps (tampon resté à 10 ms,
`ds5-haptics-loopback` revenu après rebranchement du dongle) : à rendre
durable en phase 3 (paquet AIBox).

État des lieux au point d'arrêt :
- SteamBox : serveur `/tmp/ds5-usbip-proto -a 127.0.0.1 -m auto
  --hide-mirror -t 10.10.10.1:46100 -d /tmp/ds5-audio.raw` (lancé par exec,
  à relancer après un redémarrage du conteneur ; binaire copié par le
  partage Game). Manette émulée branchée (port 0) ; tout redémarrage du
  serveur la débranche (`ds5-usbip-attach attach 127.0.0.1` sur l'hôte).
- AIBox : `~/.config/pipewire/pipewire.conf.d/ds5-usbip-haptics.conf`
  (commit b431621, tampon 40 ms, RL/RR) ; `ds5-haptics-loopback` arrêté
  (reviendra au prochain branchement du dongle) ; dongle en mode « Off ».
- Hors session de test : débrancher la manette émulée (`ds5-usbip-attach
  detach 0`), sinon seconde DualSense et sortie son par défaut dans
  SteamBox.

**Phase 1 validée (22/11, 17 h 15)** : vibrations haptiques et gâchettes
ressenties en jeu (F1 25), sans vibration permanente. Dernier obstacle :
après un rebranchement de la manette émulée pendant que le jeu tournait,
son flux haptique 4 canaux (« PipeWire ALSA [wine64-preloader] ») était
resté sur la sortie par défaut (Sunshine 5.1 : canaux 3-4 sur FC/LFE,
« petits bruits » dans le son du jeu) ; remis sur la DualSense par
`pactl move-sink-input`. Gain à l'émission ramené à ×1 (`--haptics-gain-file
/tmp/ds5-haptics-gain`, réglable à chaud) : avec les volumes à 100 %, le
niveau d'origine du jeu est le bon ; plus fort = préférence (app compagnon).

Gâchettes adaptatives : OK dans F1 25. Dans RIDE 6, le journal décodé
montre que le jeu n'envoie qu'un « effet désactivé » (mode 0x05) : RIDE 6
n'a pas de gâchettes adaptatives (vérifié en direct sur AIBox). Le renvoi
des rapports de sortie passe par une file (fb94a00), sans fusion.

Recette qui marche aujourd'hui (à automatiser en phases 2-3) :
- SteamBox : serveur avec `-t <IP client>:46100 --haptics-gain-file …`,
  sortie DualSense à 100 %, manette branchée **avant** le lancement du jeu ;
- AIBox : `ds5-usbip-haptics.conf` (40 ms, RL/RR), `ds5_dongle_sink` à
  100 %, `ds5-haptics-loopback` arrêté, dongle en mode « Off ».

À rendre durable : volume 100 % et priorité basse de la sortie DualSense
(règle WirePlumber SteamBox : fait, `52-ds5-usbip.conf`, commit local
f958479, entrera au prochain `make push`) ; côté AIBox, volume du dongle, config RTP et
désactivation du loopback quand ds5-usbip est utilisé (dépôt AIBox) ;
réduction de la latence (~50 ms, par l'app compagnon).

Corrections finales avant la phase 2 (22/11, 18 h) :
- flux haptique du jeu : Wine le crée avec `target.object` = nom stable de
  la sortie DualSense ; rebranchée en plein jeu, WirePlumber l'y remet seul
  (vérifié sur F1 25). Il n'atterrit sur la sortie par défaut que si le jeu
  démarre **avant** la manette (Wine ne la désigne alors pas) : en phase 2,
  Sunshine crée la manette à l'ouverture de session, avant le jeu ;
- nœuds `/dev/hidraw` périmés : règle udev de suppression au « remove »
  (SteamBox, commit local) ; vérifié au rebranchement ;
- gain des haptiques ramené à ×1 (niveau d'origine du jeu).

### À faire dans le projet AIBox (client)

**Fait (23/11)** : scripts de démarrage de la manette ajoutés à AIBox
(persistance des réglages client au redémarrage), selon l'utilisateur.

Décidé le 22/11 : à traiter directement dans le dépôt AIBox, pas ici.

1. Sortie du dongle à 100 % dès son apparition : ajouter
   `device.routes.default-sink-volume = 1.0` pour la carte du dongle dans
   `51-ds5dongle.conf` (à 40 %, les haptiques perdaient 24 dB).
2. Réception RTP : intégrer `client/pipewire/ds5-usbip-haptics.conf`
   (tampon 40 ms, `[ RL RR ]`, port 46100) à l'image.
3. Le récepteur RTP ne doit jamais se rabattre sur une autre sortie quand
   le dongle manque ; `node.dont-reconnect` et `node.dont-fallback` cassent
   la liaison (22/11) : règle WirePlumber à écrire. AIBox n'est pas
   persistant (récepteur, volume du dongle, loopback perdus au redémarrage).
4. `ds5-haptics-loopback` (vibration tirée du son, dongle en « Replace »)
   et ds5-usbip (haptiques natives, dongle en « Off ») s'excluent : le
   service relancé par udev à chaque reconnexion du dongle renvoie le son
   du jeu dans le haut-parleur de la manette. Choisir : ds5-usbip seul,
   ou bascule entre les deux (mode du dongle compris, piste pour l'app
   compagnon, voir `docs/parametres.md`).

### Phase 2 — intégration dans Sunshine (choix du 22/11)

Nouveau backend USB/IP dans libvirtualhid : Sunshine crée directement la
DualSense USB/IP à la place de la DualSense `uhid` (profil `"ds5"`).
Sunshine est déjà compilé dans l'image SteamBox (étape `sunshine-usb` du
Dockerfile) : le correctif rejoint cette étape. Les rapports d'entrée
(Moonlight → jeu) et de sortie (vibration, gâchettes adaptatives, LED : jeu
→ Moonlight) passent par le même chemin qu'aujourd'hui, seul le transport
change. Les canaux haptiques de la carte son partent vers le client
(phase 1).

Points à prévoir : attacher la manette par `vhci-hcd` depuis le conteneur
(écriture dans `/sys/devices/platform/vhci_hcd.0/attach` ou `usbip attach`,
droits à vérifier) à chaque connexion, la détacher à la déconnexion ; repli
sur `uhid` si `vhci-hcd` est absent ; correctif assez lourd à réappliquer à
chaque version de Sunshine (à proposer à LizardByte une fois stable).

**Critère** : une seule DualSense vue par les jeux, entrées, gâchettes
adaptatives, LED et vibration classique toujours fonctionnelles, haptiques
HD en plus.

#### Étude de la phase 2 (22/11, soir)

**Point d'intégration.** libvirtualhid : `LinuxUhidBackend::create_gamepad()`
(`src/platform/linux/uhid_backend.cpp`). Pour le profil `dualsense_usb`,
essayer d'abord une nouvelle `UsbipGamepad`, repli sur `UhidGamepad` si
vhci-hcd est indisponible. L'interface `BackendGamepad` colle au modèle
du prototype :
- `submit(state, report)` → `DualSenseDevice::set_input_report(report)` ;
- rapports de sortie (interruption OUT, SET_REPORT) →
  `reports::parse_output_reports(profile, report)` → `OutputCallback`,
  comme `UhidGamepad::dispatch_output_report` (plus de miroir ni de leurre) ;
- rapports feature : tables `playstation_feature_reports`, MAC tirée de
  `metadata.stable_id` comme le backend uhid ;
- `close()` : fermer la socket ; vhci-hcd débranche alors la manette.

**Sans TCP.** vhci-hcd n'exige qu'une socket `SOCK_STREAM` : une
`socketpair(AF_UNIX)` suffit. Une extrémité est donnée au noyau par
`/sys/devices/platform/vhci_hcd.0/attach` (« port fd devid vitesse »),
l'autre porte la session USB/IP. Plus de port 3240, plus d'import.

**Compilation (décidé : hors de SteamBox).** Sunshine récupère
libvirtualhid par CPM (`package-lock.cmake`, `LIBVIRTUALHID_COMMIT`) ; on
peut le pointer vers une copie locale (`-DCPM_libvirtualhid_SOURCE=…`).
ds5-usbip compilera libvirtualhid + backend USB/IP et Sunshine, et
publiera le résultat en release ; SteamBox le récupérera comme ses autres
composants précompilés (`prebuilt-fetch`), à la place de son étape
`sunshine-usb`.

**Haptiques.** Sunshine connaît l'adresse du client Moonlight : la
destination RTP pourra être automatique (à prévoir dans le patch Sunshine),
sinon une variable de configuration.

**Point ouvert : l'attache depuis le conteneur.** Sunshine tourne dans
SteamBox, où `/sys` est en lecture seule. Options, à décider par
l'utilisateur (elles touchent l'hôte) :
1. monter `/sys/devices/platform/vhci_hcd.0` en écriture dans SteamBox
   (modèle Unraid) et donner l'accès à l'utilisateur de Sunshine ;
2. un petit service root sur l'hôte qui reçoit la socket (SCM_RIGHTS) et
   écrit dans `attach` ; SteamBox garde `/sys` en lecture seule ;
3. variante de 2 dans SteamBox (service s6 root) : exige quand même 1.
**Option 1 retenue et testée (22/11, autorisée par l'utilisateur)** :
conteneur jetable avec `-v /sys/devices/platform/vhci_hcd.0:/sys/devices/platform/vhci_hcd.0:rw`,
écriture invalide dans `attach` → « Invalid argument » (l'écriture atteint
le noyau). Contraintes :
- `attach`/`detach` sont en `0200 root` ; Sunshine tourne en `arcade` et
  abandonne ses capacités (`cap_sys_admin,cap_sys_nice` sur le binaire)
  après son initialisation. L'init de SteamBox (root) fera
  `chgrp input` + `chmod 0220` sur `attach` et `detach` (visible côté hôte
  jusqu'au rechargement du module : le groupe peut brancher des
  périphériques USB/IP).
- Le dossier n'existe qu'une fois `vhci-hcd` chargé : sans lui, le
  montage échoue et SteamBox ne démarre pas. Charger `vhci-hcd` et
  `snd-usb-audio` dans le fichier `go` d'Unraid, avant Docker (phase 3,
  par l'utilisateur).

#### Répartition entre projets (décidé le 22/11)

- **ds5-usbip** (ce dépôt) : source du backend USB/IP (modèle DualSense,
  session USB/IP, haptiques RTP) et du correctif pour libvirtualhid.
- **[sunshine-ds](https://github.com/Gunnm92/sunshine-ds)** (public, GPL de
  Sunshine oblige) : compile libvirtualhid modifié + Sunshine, publie en
  release une archive à décompresser : binaire, règle WirePlumber
  `52-ds5-usbip.conf` (déplacée depuis SteamBox), script d'ouverture de
  `attach`/`detach`.
- **SteamBox** : seulement le générique — `prebuilt-fetch` de la release à
  la place de l'étape `sunshine-usb`, règles udev ALSA/hidraw, cgroup
  `116`, montage de `vhci_hcd.0` (modèle Unraid et doc). Le commit local
  de la règle WirePlumber (f958479) sera annulé quand la release la
  fournira.

Avancement (22/11, soir) : bibliothèque `src/usbip/` + outils `src/tools/`
(dont `ds5-usbip-local`, branchement par socketpair sans TCP ; mode miroir
retiré) ; socle libvirtualhid dans `libvirtualhid/` (`apply.sh`,
`Dockerfile.test`, correctif 0001, `FACTS.md`), tests libvirtualhid verts
en Docker (175/175 hors 21 tests exigeant /dev/uhid).

Release publiée (22/11, 19 h 30) : sunshine-ds-v2026.1122.155624-20261122-1831
(Sunshine v2026.1122.155624, libvirtualhid dd8153d + correctifs 0001/0002,
ds5-usbip bc196b7) ; 186/186 tests, backend USB/IP vérifié dans le binaire.
**Phase 2 validée en direct (22/11, 20 h 30)** : SteamBox avec le Sunshine de
sunshine-ds ; à la connexion Moonlight, Sunshine crée seul la DualSense USB/IP
(port vhci occupé par sa socket, une seule DualSense, parent USB, carte son),
oneshot s6 : `attach` au groupe input. F1 25 : vraies vibrations haptiques,
nettement mieux que le prototype. Prochaine évolution : transmettre aussi les
canaux 1-2 (haut-parleur de la manette) en RTP 4 canaux ; bruits parasites à
éclaircir (piste : « Speaker Effect Leak » du dongle). AIBox non persistant :
récepteur RTP, volume du dongle et arrêt du loopback perdus au redémarrage
(projet AIBox).

Étapes : 1. backend `UsbipGamepad` (copie de libvirtualhid, testé seul
dans SteamBox) ; 2. compilation et release dans sunshine-ds ; 3.
récupération par SteamBox (commits locaux) ; 4. test en vrai (Moonlight,
F1 25, RIDE 6).

## Bilan au 22/11 (soir) et reste à faire

**Fonctionne** : Sunshine de sunshine-ds (release
`sunshine-ds-v2026.1122.180652-20261122-2016`) crée seul la DualSense USB/IP
à la connexion Moonlight ; haptiques HD de F1 25 et RIDE 6 ressenties sur le
client (pleine échelle) ; gâchettes, LED, vibration classique ; transport RTP
2 ou 4 canaux (`LIBVIRTUALHID_DUALSENSE_RTP_CHANNELS=4`). F1 25 n'envoie
rien au haut-parleur de la manette (mesuré : canaux 1-2 nuls) ; le
4 canaux sert aux jeux qui l'utilisent. Chaîne 4 canaux vérifiée de bout en
bout le 22/11 (`RTP_CHANNELS=4 tests/rtp_test_pattern.py` depuis SteamBox :
bips gauche/droite dans le haut-parleur, vibrations gauche/droite reçus).

**Reprise (23/11)** : page « DualSense » et destination automatique en
cours. Côté driver : fait et poussé (correctif 0003, ds5-usbip 7da3607).
Côté Sunshine : correctifs de l'agent dans sunshine-ds (commits locaux,
patches/sunshine/) à relire, puis `make build REQUIRE_USBIP=1`, release,
`make push` SteamBox, test de la page sur http://<SteamBox>:47990.

**23/11, 00 h 30 : page « DualSense » validée en direct.** Image SteamBox
reconstruite (Sunshine 745f95a, correctifs sunshine-ds 0001-0002 sur la
branche locale `dualsense-web`, f63eb4e), déployée et testée par
l'utilisateur : tout fonctionne. Attention : ArcadeBox et SteamBox sont en
réseau hôte et partagent les ports Sunshine (47984-48010), noVNC (6080) et
le socket X ; ArcadeBox lancé, le Sunshine de SteamBox boucle sur « Couldn't
bind RTSP server to port 48010 ». Un seul des deux à la fois.

**Reste à faire :**
1. SteamBox : commits poussés (22/11).
2. Registre : réglé le 22/11 (`--insecure-registry=10.1.1.1:5003` dans
   `/boot/config/docker.cfg`, modèle sur `10.1.1.1:5003/steambox:latest`).
3. Fichier `go` d'Unraid : `modprobe vhci-hcd snd-usb-audio` avant Docker
   (sans vhci-hcd, le montage manque et SteamBox ne démarre pas).
4. AIBox : intégration faite dans son projet (22/11), d'après
   `docs/aibox.md`.
5. Grésillements à pleine échelle : plus constatés (23/11, ressenti de
   l'utilisateur) ; rien à faire.
6. Destination RTP automatique : faite (correctif sunshine-ds 0001, page
   « DualSense », validée le 23/11). sunshine-ds poussé (main f63eb4e,
   release 20261122-2309).
7. Latence : ~50 ms (40 ms de tampon client + réseau). Mesurer le délai de
   la sortie du dongle sur AIBox, puis descendre `sess.latency.msec` au
   minimum qui joue encore (10 ms = silence le 22/11).
8. Page « DualSense » de Sunshine : la rendre plus attrayante (mise en
   page, état en direct de la manette, retour visuel des réglages).
9. App compagnon (`docs/parametres.md`).

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
