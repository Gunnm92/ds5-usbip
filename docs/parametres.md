# Paramètres réglables

Inventaire des réglages de la chaîne haptique, en vue d'une petite app
compagnon. Valeurs relevées le 22/11 (fin de la phase 1). « À chaud » :
modifiable sans couper la session ni rebrancher la manette.

```
jeu (Wine) ─► sortie DualSense ─► ds5-usbip ─RTP─► module-rtp-source ─► ds5_dongle_sink ─► dongle ─BT─► manette
  SteamBox      PipeWire           serveur          AIBox (PipeWire)      AIBox             firmware
```

## 1. Serveur ds5-usbip (SteamBox)

| Réglage | Valeur actuelle | Plage / choix | Comment | À chaud |
|---|---|---|---|---|
| **Gain des haptiques** | ×2 | 0 < g ≤ 64 ; écrêtage doux au-delà de la moitié de la pleine échelle | `--haptics-gain G`, ou fichier `--haptics-gain-file /tmp/ds5-haptics-gain` (un nombre, relu chaque seconde) | **oui** (fichier) |
| Destination RTP | `10.10.10.1:46100` | `IP:PORT` du PC client | `--haptics-to` | non (redémarrage = manette débranchée) |
| Journal détaillé | coupé | marche / arrêt | `-v`, ou signal `SIGUSR1` (bascule) | **oui** (signal) |
| Enregistrement PCM | `/tmp/ds5-audio.raw` | fichier S16_LE 4 canaux 48 kHz | `-d FICHIER` | non |
| Écoute USB/IP | `127.0.0.1:3240` | adresse, port | `-a`, `-p` | non |
| Recopie de la manette Sunshine (test, disparaît en phase 2) | `auto` + masquage | chemin hidraw, `auto` ; leurre `/dev/null` | `-m`, `--hide-mirror` | non |

Format RTP fixe : L16 stéréo big-endian, 48 kHz, type de charge 127, 1 ms
(48 trames) par paquet, marquage DSCP EF (`IP_TOS 0xb8`). Le marquage
n'a pas encore été testé seul : il reste à vérifier qu'aucun routeur ne le
pénalise.

**À afficher** : chaque seconde, le serveur journalise le pic et le niveau
RMS des 4 canaux (`audio: 48000 trames | HP G … | haptique G … (N non nuls)`) ;
état de la session (`périphérique importé`, `session terminée`) ; état de
la recopie (`miroir : …`).

## 2. PipeWire de SteamBox

| Réglage | Valeur attendue | Pourquoi | Comment | À chaud |
|---|---|---|---|---|
| **Volume de la sortie DualSense** | **100 %** | À 40 % (défaut), PipeWire atténuait les haptiques de 24 dB ; une vraie DualSense n'applique ce volume qu'au haut-parleur | `pactl set-sink-volume <sortie DualSense> 100%` (mémorisé par WirePlumber) | oui |
| **Sortie du flux haptique du jeu** | sortie DualSense | Si la manette est rebranchée pendant que le jeu tourne, le flux 4 canaux (« PipeWire ALSA [wine64-preloader] ») reste sur la sortie par défaut : haptiques dans le son du jeu | `pactl move-sink-input <flux> <sortie DualSense>` ; mieux : brancher la manette avant le jeu | oui |
| Sortie par défaut | celle de Sunshine | Hors session, la sortie DualSense devient celle par défaut | à fixer par règle WirePlumber (priorité basse), à faire | — |

## 3. PipeWire d'AIBox (client)

Fichier `~/.config/pipewire/pipewire.conf.d/ds5-usbip-haptics.conf`
([client/pipewire/](../client/pipewire/ds5-usbip-haptics.conf)).

| Réglage | Valeur actuelle | Plage / choix | Comment | À chaud |
|---|---|---|---|---|
| **Tampon de réception** (`sess.latency.msec`) | 40 ms | doit dépasser le délai de la sortie du dongle : à 10 ms, silence ; minimum réel à mesurer | fichier + `systemctl --user restart pipewire` (coupe le son de Moonlight : quitter la session avant) ; pour essayer : `pw-cli -m load-module libpipewire-module-rtp-source '{ … }'` sur un autre port | non (fichier) ; oui (pw-cli) |
| Canaux de destination (`audio.position`) | `[ RL RR ]` | `[ RL RR ]` en profil « Direct » du dongle, `[ AUX2 AUX3 ]` en pro-audio | fichier | non |
| Port de réception (`source.port`) | 46100 | doit valoir celui de `--haptics-to` | fichier | non |
| **Volume de la sortie du dongle** (`ds5_dongle_sink`) | **100 %** | à 40 %, -24 dB de plus | `pactl set-sink-volume ds5_dongle_sink 100%` | oui |
| Volume du flux reçu (`ds5_usbip_haptics`) | 100 % | > 100 % = gain logiciel (échelle cubique : 160 % ≈ ×4, 200 % ≈ ×8), écrête | `pactl set-sink-input-volume <flux> N%` | oui |
| Renvoi du son du jeu vers la manette (`ds5-haptics-loopback`) | arrêté | marche = vibration « auto » tirée du son (canaux 1-2) ; en mode « Off », son du jeu dans le haut-parleur de la manette | `systemctl --user stop/start ds5-haptics-loopback` ; redémarre au rebranchement du dongle | oui |

**À afficher** : `pw-link -l` (flux relié à `playback_RL` / `playback_RR`),
`pw-top` (nœud `ds5_usbip_haptics` actif, erreurs), niveau reçu
(`pw-record --target ds5_dongle_sink -P '{ stream.capture.sink=true }'`).

## 4. Firmware du dongle (DS5Dongle-Studio, portail web)

| Réglage | Valeur actuelle | Plage / choix | Effet |
|---|---|---|---|
| **Mode** | Off | Off / Mix / Replace | Off : seules les haptiques natives (canaux 3-4) passent ; Replace : les ignore ; Mix : les mélange à la vibration tirée du son |
| **Haptics Gain** | à vérifier | 1.0 – 2.0 | Multiplie les canaux 3-4 avant la conversion 8 bits (3 kHz) vers le Bluetooth |
| Native Passthrough in Mix (%) | 8 | 0 – 100 | Part des haptiques natives en mode Mix seulement |
| Intensity, Smoothness, Noise Gate, LP Cutoff, Crossover, bandes | 169, 16, 20, 24 Hz, 72 Hz… | — | Vibration « auto » tirée du son (Mix / Replace), sans effet en Off |
| Speaker Effect Leak (volume, filtres…) | 100, … | — | Fuite des effets vers le haut-parleur de la manette |

## 5. Hôte Unraid

| Action | Commande | Remarque |
|---|---|---|
| Brancher la manette émulée | `ds5-usbip-attach attach 127.0.0.1` | après le démarrage de SteamBox, avant le jeu |
| Débrancher | `ds5-usbip-attach detach <port>` | à faire hors des sessions de test |
| État | `ds5-usbip-attach status` | ports vhci-hcd occupés |
| Modules | `modprobe vhci-hcd snd-usb-audio` | à charger à l'amorçage (phase 3) |

## Pour l'app compagnon

Réglages utiles au quotidien, par priorité : gain des haptiques (serveur,
à chaud), mode et Haptics Gain du dongle, marche/arrêt du renvoi
`ds5-haptics-loopback`, tampon de réception (latence). Contrôles d'état :
manette branchée, flux haptique du jeu sur la bonne sortie, volumes à
100 % des deux côtés, flux RTP reçu, niveaux par canal. Les réglages
« volume à 100 % » et « sortie du flux » sont des corrections
d'installation plutôt que des préférences : à automatiser (règles
WirePlumber) plutôt qu'à exposer.
