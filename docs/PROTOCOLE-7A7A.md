# Protocole série « 7A 7A » des clims Airton (module ACW02)

Notes de travail, état au 11 novembre 2026. Clim : Airton SMVH12B-2B2A3NM (unité intérieure réf. 409935).
Niveau de confiance indiqué pour chaque point : **[confirmé]** vu sur la clim réelle, **[forum]** vu dans les captures
ou scripts de la communauté (forum HACF, script Tasmota « Berryton »), **[hypothèse]** déduit, pas vérifié.

## 1. Liaison

- UART 9600 bauds, 8N1, niveau 5 V. **[confirmé]**
- Connecteur « WIFI connection cable » de l'unité intérieure : 4 fils (marron, rouge, bleu, vert). Chez moi, marron/rouge
  mesurent 5 V ; bleu et vert sont TX/RX de la clim. Adaptateur de niveau 5 V ↔ 3,3 V nécessaire côté ESP32.
- La clim parle d'elle-même, sans keepalive du module : un ping (type A4) toutes les 10 s, son état (A3) environ
  toutes les 60 s et à chaque changement fait à la télécommande. **[confirmé]**
- Certaines unités utilisent l'en-tête `BD BD` au lieu de `7A 7A` (même structure). **[forum]** Le firmware gère les deux.

## 2. Format de trame **[confirmé]**

| Octet | Contenu |
|---|---|
| 0-1 | en-tête `7A 7A` |
| 2-3 | source, destination : module → clim `21 D5` ; clim → module `D5 21` ou `D1 21` |
| 4 | longueur totale de la trame (en-tête et CRC compris) |
| 5-6 | `00 00` |
| 7 | type |
| 8… | données (les octets 8-9 valent `0A 0A` dans les trames d'état et de requête) |
| n-2, n-1 | CRC |

**CRC** : CRC-16/MODBUS (init `FFFF`, polynôme `A001`, sans XOR final) calculé sur **tous** les octets précédents, en-tête
compris, stocké **octet de poids fort en premier**. Vérifié sur 12 trames capturées (voir § 7).

### Types de trames

| Type | Sens | Longueur | Rôle |
|---|---|---|---|
| `A1` | module → clim | 24 | commande (§ 4) |
| `A2` | module → clim | 12 | demande d'état ; réponse : A3 |
| `A3` | clim → module | 34 | état (§ 3) |
| `A4` | clim → module | 13 | ping, toutes les 10 s |
| `A5` | clim → module | 27 | périodique, rôle inconnu |
| `A6` | clim → module | 28 | octet 10 = `0x80` : nettoyage du filtre demandé ; octet 12 : code d'erreur (`04` = « mode non supporté ») **[forum]** |
| `AB` | module → clim | 12 | keepalive (inutile chez moi) |
| `AC` | clim → module | 18 | rôle inconnu |

Trames émises par le module (identiques aux captures) :
`7A 7A 21 D5 0C 00 00 A2 0A 0A FE 29` (demande d'état), `7A 7A 21 D5 0C 00 00 AB 0A 0A FC F9` (keepalive).

## 3. Trame d'état A3 (34 octets, clim → module)

| Octet | Contenu | Confiance |
|---|---|---|
| 10 | température ambiante, partie entière (°C) | confirmé |
| 11 | température ambiante, dixièmes (`05` = ,5) | confirmé |
| 13 | bits 0-2 : mode (0 auto, 1 froid, 2 déshumidification, 3 ventilation seule, 4 chauffage) ; bit 3 : marche ; bits 4-6 : ventilation (0 auto, 1 à 5 niveaux, 6 « stepless » = vitesse en pourcentage réglée à la télécommande) ; bit 7 : turbo | confirmé |
| 14 | bits 0-3 : consigne − 16 (`03` = 19 °C) ; bit 6 : mode silence (ventilation en auto) | consigne confirmée ; silence confirmé en lecture |
| 15 | volets : bits 0-3 balayage vertical, bits 4-7 balayage latéral. `00` fixe, `01` vertical, `10` latéral, `11` les deux | confirmé |
| 16 | bit 7 : éclairage de l'afficheur ; valeurs vues : `98` (chauffage, afficheur allumé), `88` (autres modes ou arrêt), `94`, `14` (afficheur éteint) | bit 7 confirmé ; bit 4 (`10`) = chauffe ? hypothèse |
| 21 | 1 froid, 2 déshumidification, 3 ventilation seule, 4 chauffage. En mode auto reste à la dernière valeur. Mode réellement exécuté ? | hypothèse |
| 22 | `64` constant | observé |
| 23-24 | valeur 16 bits qui monte lentement (`0E5D` → `0E5E` en environ 200 s) ; rôle inconnu | inconnu |
| 25-31 | texte ASCII `DB11510` (`DB11370` sur une autre unité du forum), référence de carte | observé |
| 32-33 | CRC | confirmé |

Remarques :
- Éteinte, la clim garde son mode et sa ventilation dans l'état : seul le bit 3 de l'octet 13 passe à 0.
- Le pourcentage de ventilation (« 62 % », « 64 % » à la télécommande) n'apparaît dans aucun octet qui change.
- Le bit 6 de l'octet 14 (silence) a d'abord été pris pour un état transitoire : il est à 1 tant que le silence est actif.
- Des valeurs passagères apparaissent pendant que la télécommande enchaîne des appuis rapides (octets 13, 14, 15) :
  ne pas conclure sur une valeur vue moins de 2 s.

### Ordre des touches de la télécommande observé
- Mode : chauffage → auto → froid → déshumidification → ventilation seule → chauffage.
- Ventilation : 1 → 2 → 3 → 4 → 5 → turbo → pourcentage → auto → silence → 1.
- Chaque mode mémorise sa consigne et ses volets (auto 25 °C, froid 26, déshumidification 27, ventilation 18, chauffage 19 chez moi).

## 4. Trame de commande A1 (24 octets, module → clim) **[validée sur la clim réelle]**

`7A 7A 21 D5 18 00 00 A1 00 00 00 00 b12 b13 b14 b15 00 00 00 00 00 00 CRC_hi CRC_lo`

Commandes validées sur la clim le 11 novembre 2026 : marche/arrêt, consigne, mode, ventilation, silence, afficheur, volets.
Un octet de commande correspond à l'octet suivant de l'état (décalage d'un octet) :

| Octet | Contenu |
|---|---|
| 12 | mode (bits 0-2) ; marche (bit 3, `08`) ; ventilation (bits 4-6). Arrêt : seulement la ventilation (mode et bit 3 à 0). Turbo : valeur 7 selon le forum, mais l'état renvoie niveau 5 + bit 7 |
| 13 | consigne − 16 (bits 0-3) ; silence : bit 6 |
| 14 | volets, valeur brute rapportée par la clim (`00`, `01`, `10`, `11`). La table du forum (0 à 13) ne correspond pas à cette clim |
| 15 | mot de configuration : le forum met toujours `98` ; le firmware envoie `18` + bit 7 si l'afficheur doit être allumé |
| 16-21 | `00` |

Exemple (construit par le firmware, test sur PC) : froid, ventilation 3, 24 °C, volets fixes, afficheur allumé :
octets 12 à 15 = `39 08 00 98`.

Le premier bip sonore survient sur la première commande reçue (rapporté sur le forum, non reproduit ici).

## 5. Ce qui reste à trouver

- Où est le pourcentage de ventilation (mode « stepless ») ?
- Octets 12, 17-20 de l'état (toujours à 0 jusqu'ici), octets 23-24.
- Touches non testées : éco, sleep, °C/°F, auto-nettoyage (la télécommande en a peut-être).
- Trames A5 et AC : rôle des octets.
- Pas encore testés séparément : turbo par commande (valeur 7) et pourcentage de ventilation ; codes d'erreur et filtre jamais vus non nuls ; autres modèles Airton.

## 6. Différences avec ce qu'on trouve en ligne

- Volets : deux moitiés d'octet (vertical / latéral) au lieu d'une table de 14 valeurs.
- Silence : bit 6 de l'octet de consigne, pas une vitesse de ventilation.
- Turbo (état) : niveau 5 + bit 7 ; la table de commande du forum donne la valeur 7.
- Mot de configuration (octet 16 de l'état) : `98` en chauffage, `88` ailleurs, alors que le forum le dit fixe.
- CRC : octet de poids fort en premier.

## 7. Trames de test (captures du forum, CRC vérifié)

```
7A 7A 21 D5 0C 00 00 AB 0A 0A FC F9
7A 7A 21 D5 0C 00 00 A2 0A 0A FE 29
7A 7A D1 21 0D 00 00 A4 0A 0A 00 25 25
7A 7A D5 21 12 00 00 AC 0A 0A 06 52 00 12 BF 00 BB 98
7A 7A D5 21 22 00 00 A3 0A 0A 19 00 00 6B 46 06 40 00 00 00 00 03 64 01 E8 44 42 31 31 33 37 30 09 56
7A 7A D5 21 22 00 00 A3 0A 0A 1A 00 00 29 0A 02 00 00 00 00 00 01 64 12 BF 44 42 31 31 33 37 30 8F 7A
```

Autres trames de test dans `test/test_acw02.cpp`.

## 8. Dans le firmware

- `main/acw02.h`, `main/acw02.cpp` : CRC, parseur, décodage de l'état, construction des commandes (tests : `test/test_acw02.cpp`).
- `main/main.cpp` : détection automatique du protocole (Tuya `55 AA` ou `7A 7A`), état, WiFi, serveur web.
- `main/discovery.cpp` : page de découverte (journal des changements d'octets, panneau « Commande de la clim »).

## Sources
- Forum HACF, fil sur le module ACW02 Airton (captures et scripts d'utilisateurs).
- Script Tasmota « Berryton » (GitHub, `ouinouin/Berryton`).
- Captures de ma clim (exports CSV de la page de découverte, 8 novembre 2026).
