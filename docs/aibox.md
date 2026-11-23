# Intégration côté client : AIBox

Ce qu'il faut ajouter à l'image AIBox (dépôt
[Gunnm92/AIBox](https://github.com/Gunnm92/AIBox), `archiso/airootfs/`) pour
recevoir le son de la DualSense envoyé par SteamBox et le jouer sur la
manette, via le DS5Dongle. Tout a été validé à la main le 22/11/2026 ; AIBox
n'étant pas persistant, ces réglages se perdent au redémarrage tant qu'ils ne
sont pas dans l'image.

## Ce que reçoit AIBox

Le Sunshine de SteamBox (projet sunshine-ds) envoie en RTP, vers
`<IP d'AIBox>:46100` :

- L16 big-endian, 48 kHz, type de charge 127, un paquet par milliseconde ;
- 4 canaux si SteamBox a `LIBVIRTUALHID_DUALSENSE_RTP_CHANNELS=4` :
  haut-parleur de la manette (1-2) puis haptiques HD (3-4) ; sinon 2 canaux
  (haptiques seules).

Le DS5Dongle (firmware DS5Dongle-Studio) reçoit 4 canaux USB : 1-2 vont au
haut-parleur de la manette, 3-4 aux actionneurs.

## 1. Récepteur RTP

Copier [client/pipewire/ds5-usbip-haptics.conf](../client/pipewire/ds5-usbip-haptics.conf)
vers `archiso/airootfs/etc/pipewire/pipewire.conf.d/ds5-usbip-haptics.conf`
(niveau système, pour tous les utilisateurs).

Points de ce fichier qui ont été établis à l'essai :

- `sess.latency.msec = 20` (validé le 23/11 sur plusieurs jeux), **avec**
  la règle [client/wireplumber/52-ds5dongle-latency.conf](../client/wireplumber/52-ds5dongle-latency.conf)
  à copier vers `archiso/airootfs/etc/wireplumber/wireplumber.conf.d/` :
  blocs de 128 échantillons au dongle au lieu de 512. Sans cette règle,
  rester à 40 : à 10 ms, le tampon est plus court que le délai de la sortie
  du dongle ; `module-rtp-source` le ramène alors à zéro et ne joue que du
  silence. Sur l'AIBox actuelle, la valeur vit dans
  `/etc/aibox/ds5-usbip-rtp.conf` (chargé par `ds5-usbip.service`).
- `audio.position = [ FL FR RL RR ]` : en profil « Direct », les canaux du
  dongle s'appellent FL FR RL RR (pas AUX). En profil pro-audio, mettre
  `[ AUX0 AUX1 AUX2 AUX3 ]`. Avec seulement 2 canaux reçus :
  `audio.channels = 2` et `[ RL RR ]`.
- `target.object = "ds5_dongle_sink"` (nom donné par la règle
  `51-ds5dongle.conf` existante).
- **Ni `node.dont-reconnect` ni `node.dont-fallback`** : le premier détruit
  le flux si la sortie du dongle n'existe pas encore au démarrage de PipeWire
  (elle arrive après), le second empêche toute liaison. Voir le point 3 pour
  empêcher le repli.

## 2. Volume du dongle à 100 %

WirePlumber ouvre une nouvelle sortie à 40 % : les haptiques perdent alors
24 dB et ne se sentent presque plus. Dans
`archiso/airootfs/etc/wireplumber/wireplumber.conf.d/51-ds5dongle.conf`,
ajouter une règle sur la **carte** du dongle :

```
monitor.alsa.rules = [
  # (règles existantes…)
  {
    matches = [
      { device.name = "~alsa_card.usb-Sony_Interactive_Entertainment_DualSense_Wireless_Controller.*" }
    ]
    actions = {
      update-props = {
        device.routes.default-sink-volume = 1.0
      }
    }
  }
]
```

(WirePlumber 0.5 : ne joue qu'à la première apparition, ensuite le volume
mémorisé est restauré ; sur une image non persistante, c'est à chaque
démarrage.)

## 3. Ne jamais jouer le son de la manette ailleurs

Sans `dont-fallback`, si le dongle est absent, le flux RTP peut se rabattre
sur la sortie par défaut (TV) : haptiques et haut-parleur deviendraient des
grésillements audibles. À verrouiller par une règle WirePlumber qui
n'autorise le nœud `ds5_usbip_haptics` qu'à se lier à `ds5_dongle_sink`
(par exemple un script de liaison, ou `node.passive`/`target.object` combinés
à une règle `linking`), **à tester** : c'est le seul point non résolu côté
client.

## 4. Choisir entre haptiques natives et vibration tirée du son

`ds5-haptics-loopback.service` (relancé par udev à chaque branchement du
dongle) renvoie tout le son du jeu sur les canaux 1-2 du dongle, pour le mode
« Replace » du dongle (vibration calculée à partir du son). Il est
incompatible avec ds5-usbip :

- dongle en mode **« Off »** obligatoire pour les haptiques natives (sinon
  les canaux 3-4 sont ignorés) ;
- en mode « Off », les canaux 1-2 vont au haut-parleur de la manette : le
  loopback y met tout le son du PC.

Options : désactiver le loopback dans l'image (ds5-usbip seul), ou une
bascule entre les deux modes (service + mode du dongle), piste pour l'app
compagnon ([parametres.md](parametres.md)).

## 5. Vérifications après démarrage

```fish
ss -ulpn | grep 46100                       # PipeWire écoute
pw-link -l | grep -A1 "ds5_usbip_haptics:"  # 4 liaisons vers ds5_dongle_sink
pactl get-sink-volume ds5_dongle_sink       # 100 %
systemctl --user is-active ds5-haptics-loopback   # inactive (si ds5-usbip seul)
```

PipeWire n'ouvre le port 46100 qu'une fois le flux relié : pas de port =
flux non relié. Mesure du niveau reçu, en jeu :

```fish
timeout 8 pw-record --target ds5_dongle_sink -P '{ stream.capture.sink=true }' --raw --rate 48000 --channels 4 --channel-map FL,FR,RL,RR --format s16 - > /tmp/cap.raw
python3 -c "import array; d=array.array('h'); d.frombytes(open('/tmp/cap.raw','rb').read()); [print('canal',c+1,'pic',max(map(abs,d[c::4])) if d else 0) for c in range(4)]"
```

Redémarrer PipeWire coupe le son de Moonlight : quitter la session avant.
