# Airton ESP32 : pilotage d'une clim Airton « non communicante » (protocole 7A 7A)

[English version](README.md)

Firmware **ESP-IDF 5.x** qui remplace le module WiFi Airton (réf. 409729, « ACW02 ») : l'ESP32 se branche
sur le connecteur WiFi de l'unité intérieure, lit son état par liaison série et la pilote depuis une page
web qu'il sert lui-même, sans cloud ni application. Développé et testé sur une **Airton SMVH12B-2B2A3NM**
(unité intérieure réf. 409935), ESP-IDF 5.3.1.

> ⚠️ Projet personnel, fourni sans garantie. Vous ouvrez une unité sous secteur et vous envoyez des
> ordres à une clim : lisez [Sécurité](#sécurité) et testez d'abord en lecture seule.

## État du projet

| Fonction | Statut sur SMVH12B |
|---|---|
| Lecture : température ambiante, marche/arrêt, mode, consigne, ventilation (niveaux, turbo, silence), volets, afficheur | validé sur la clim réelle |
| Commandes : marche/arrêt, consigne, mode, ventilation, silence, afficheur, volets | validées sur la clim réelle |
| Pourcentage de ventilation, éco, sleep, °C/°F, auto-nettoyage | non trouvés dans les trames |
| Codes d'erreur, demande de filtre | décodés (trame A6), jamais vus sur ma clim |
| Protocole Tuya `55 AA` (autres modèles Airton) | parseur testé sur PC uniquement |

Le détail du protocole, avec le niveau de confiance de chaque octet, est dans
[`docs/PROTOCOLE-7A7A.md`](docs/PROTOCOLE-7A7A.md).

## Ce que fait le firmware

- Détecte automatiquement le protocole de la clim (`7A 7A` ou Tuya `55 AA`), ou le fixe dans `menuconfig`.
- Sert une **page web** : carte de commande (température, marche/arrêt, consigne, mode, ventilation,
  volets, afficheur) calée sur l'état lu dans la clim, bannières d'erreur et de filtre.
- Un bloc replié « Analyse des trames (avancé) » : journal des changements d'octets, repères, export CSV,
  datapoints, écriture avancée. C'est l'outil pour cartographier les fonctions d'un autre modèle.
- **Mise à jour par WiFi** (depuis la page ou par `curl`), avec retour arrière automatique.
- WiFi en point d'accès (par défaut), en client de votre box, ou les deux. Processeur cadencé à 80 MHz.
- Commandes verrouillées par défaut : « Déverrouiller » les autorise 5 minutes.

Pas de MQTT ni de Home Assistant dans cette version.

## Matériel

- ESP32 DevKit classique (testé), module d'adaptation de niveau **5 V ↔ 3,3 V** bidirectionnel 4 canaux
  (BSS138) ou équivalent.
- Accès au connecteur « WIFI connection cable » de l'unité intérieure (4 fils, 5 V, 9600 bauds 8N1).
- Multimètre ; un isolateur USB si vous branchez un PC pendant que l'ESP32 est relié à la clim.

## Sécurité

- Coupez le disjoncteur avant d'ouvrir l'unité : la carte est sous secteur.
- La masse de la carte clim n'est probablement **pas isolée du secteur**. Ne reliez jamais un PC branché
  sur secteur à cette masse sans isolateur USB. Procédure sûre : flasher l'ESP32 hors du montage, puis
  ne faire que des mises à jour par WiFi.
- Mesurez le courant que le connecteur peut fournir avant d'y alimenter l'ESP32 (les pics WiFi montent à
  environ 300 mA). Un condensateur de 100 à 470 µF près de l'ESP32 évite les chutes de tension.
- Ouvrir l'unité peut affecter la garantie.

## Câblage

```
Clim TX (5 V) ──[adaptateur de niveau]──► GPIO35  (AC_UART_RX_PIN, entrée seule)
Clim RX       ◄──[adaptateur de niveau]──── GPIO33  (AC_UART_TX_PIN)
Clim GND ───────────────────────────────────── GND ESP32
Clim +5 V ──► VIN ESP32 (si le courant suffit)
```

- Le GPIO d'émission ne peut pas être un GPIO 34 à 39 (entrée seule) ; le code refuse de compiler dans ce cas.
- Le sens TX/RX des fils n'est pas garanti par leur couleur. Si rien ne répond, inversez les deux fils
  ou activez « Inverser TX et RX » dans `menuconfig`. Un test en boucle (relier les deux voies côté 5 V)
  valide l'adaptateur et le firmware sans la clim.
- Si le 5 V du connecteur n'est présent qu'avec le module officiel, alimentez l'ESP32 à part et ne reliez
  que GND, TX et RX.

## Compilation et flash

Prérequis : ESP-IDF 5.3 (terminal « ESP-IDF » sous Windows).

```bash
idf.py set-target esp32
idf.py menuconfig     # menu « Airton AC » : WiFi, broches, jeton OTA, protocole
idf.py build
idf.py -p COMx flash monitor
```

À régler dans `menuconfig` avant de flasher : **le mot de passe du point d'accès** (défaut `airton1234`),
**le jeton OTA** (défaut `changez-moi`), les broches UART, le mode WiFi.

`sdkconfig.defaults` fixe : flash 4 Mo, deux partitions OTA de 1,5 Mo, retour arrière activé, `-Os`,
processeur à 80 MHz. Si vous avez déjà un `sdkconfig`, supprimez-le ou réglez la fréquence dans
`menuconfig` (*Component config* → *ESP System Settings* → *CPU frequency*).

### Modes WiFi

| Mode | Effet |
|---|---|
| Point d'accès seul (défaut) | Réseau **Airton-AC**, page web sur `http://192.168.4.1/`. Pas d'heure réseau. |
| Box seulement | L'ESP32 rejoint votre WiFi 2,4 GHz. Page web sur l'adresse donnée par la box. Heure par NTP. |
| Point d'accès + box | Les deux à la fois. |

## Utilisation

1. Connectez-vous au réseau **Airton-AC**, ouvrez `http://192.168.4.1/` et saisissez le jeton OTA.
2. La carte se remplit dès que la clim a envoyé son état (au plus une minute).
3. Pour agir : « Déverrouiller les commandes » (5 min), puis les boutons.

Les commandes texte de la section avancée : `ac state`, `ac temp=20`,
`ac power=on mode=heat fan=3 swing=both display=on` (champs facultatifs), `raw <type> [octets]`.

### Mise à jour par WiFi

Depuis la page : section « Mise à jour du firmware », choisissez `build/airton_ac.bin`. Ou :

```bash
curl -H "X-Token: votre_jeton" --data-binary @build/airton_ac.bin http://192.168.4.1/update
```

Le nouveau firmware se déclare valide quand son serveur web démarre ; sinon le bootloader revient à
l'ancienne version au redémarrage suivant. L'API et l'OTA exigent le jeton (en-tête `X-Token`) mais le
trafic n'est pas chiffré : à réserver à un réseau de confiance.

## Aider à cartographier d'autres modèles

Avec la page avancée : cochez « afficher les trames brutes », ajoutez un repère (`note eco ON`), actionnez
**une seule** touche de la télécommande, exportez le CSV. Les octets qui changent apparaissent dans le journal.
Les captures (CSV) sont les bienvenues en *issue*.

## Tests

```bash
./test/run.sh          # parseurs Tuya et 7A 7A, sur PC (g++ suffit)
```

`test/test_acw02.cpp` utilise des trames capturées sur le forum HACF et sur ma clim (CRC vérifié).

## Structure

| Fichier | Rôle |
|---|---|
| `main/acw02.*` | Protocole 7A 7A : CRC, parseur, état, commandes |
| `main/tuya_mcu.*` | Protocole Tuya `55 AA` |
| `main/main.cpp` | UART, détection du protocole, état, commandes, WiFi, SNTP |
| `main/discovery.*` | Page web (carte de commande, analyse des trames, mise à jour) et API |
| `main/ota.*` | Mise à jour HTTP avec jeton et retour arrière |
| `main/ac_bridge.h` | Interface entre le cœur du firmware et la page |
| `main/Kconfig.projbuild` | Réglages `menuconfig` (menu « Airton AC ») |
| `docs/PROTOCOLE-7A7A.md` | Notes de protocole, octet par octet |
| `partitions.csv`, `sdkconfig.defaults` | Partitions OTA et réglages par défaut |
| `test/` | Tests unitaires sur PC |

## Limites connues

- Les lectures et les commandes ont été validées sur une seule clim (SMVH12B) ; d'autres modèles Airton peuvent différer.
- Le pourcentage de ventilation, l'éco, le sleep et les autres touches de la télécommande ne sont pas
  décodés. Des utilisateurs rapportent un bip à la première commande sur d'autres modèles.
- Le protocole Tuya n'a jamais été essayé sur une vraie clim.
- L'OTA et la page ne sont pas chiffrées.

## Sources

Forum HACF (« Module WIFI climatisation AIRTON »), script Tasmota
[`ouinouin/Berryton`](https://github.com/ouinouin/Berryton), et mes propres captures.

## Licence

[MIT](LICENSE). Le logiciel est fourni sans garantie, comme le rappelle l'avertissement en tête de ce fichier.
