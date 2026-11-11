#pragma once
// Protocole série "7A 7A" des modules WiFi Airton ACW02 (9600 bauds 8N1).
//
//   octet 0-1 : en-tête 7A 7A (certaines clims : BD BD)
//   octet 2-3 : source / destination  (module -> clim : 21 D5 ; clim -> module : D5 21 ou D1 21)
//   octet 4   : longueur TOTALE de la trame (en-tête et CRC compris)
//   octet 5-6 : 00 00
//   octet 7   : type (A1 commande, A2 demande d'état, A3 état, A4 ping, AB keepalive, ...)
//   octet 8..n-3 : données
//   2 derniers: CRC-16/MODBUS (init FFFF, poly A001) sur TOUS les octets précédents, poids FORT d'abord (vérifié sur 12 captures).
//
// Aucune dépendance ESP-IDF : ce fichier est testé sur PC (test/test_acw02.cpp).
#include <stdint.h>

#include <string>
#include <vector>

namespace acw02 {

constexpr uint8_t T_COMMAND = 0xA1;     // module -> clim, 24 octets
constexpr uint8_t T_GET_STATUS = 0xA2;  // module -> clim, 12 octets
constexpr uint8_t T_STATUS = 0xA3;      // clim -> module, 34 octets
constexpr uint8_t T_PING = 0xA4;        // clim -> module, 13 octets (toutes les 10 s)
constexpr uint8_t T_PERIODIC = 0xA5;    // 27 octets
constexpr uint8_t T_INFO = 0xA6;        // clim -> module, 28 octets (filtre, code d'erreur)
constexpr uint8_t T_KEEPALIVE = 0xAB;   // module -> clim, 12 octets
constexpr uint8_t T_AC = 0xAC;          // clim -> module, 18 octets

constexpr size_t MIN_LEN = 12;
constexpr size_t MAX_LEN = 64;

uint16_t crc16(const uint8_t *data, size_t n);

// Octet d'en-tête utilisé pour construire les trames (0x7A par défaut, 0xBD sur certains modèles).
void setHeader(uint8_t h);
uint8_t header();

// Ajoute le CRC à la fin de la trame.
void seal(std::vector<uint8_t> &frame);

// Trame module -> clim : en-tête, longueur, type, données (à partir de l'octet 8), CRC.
std::vector<uint8_t> buildFrame(uint8_t type, const std::vector<uint8_t> &payload);
// Trame courte de 12 octets (octets 8-9 = 0A 0A) : A2, AB...
std::vector<uint8_t> buildSimple(uint8_t type);

enum Mode : uint8_t { MODE_AUTO = 0, MODE_COOL = 1, MODE_DRY = 2, MODE_FAN = 3, MODE_HEAT = 4 };
extern const char *const MODE_NAMES[5];   // auto cool dry fan_only heat
extern const char *const FAN_NAMES[8];    // auto low low_medium medium medium_high high stepless turbo
// Volets (octet 15 de l'état) : moitié basse = balayage vertical, moitié haute = balayage latéral (observé).
extern const char *const SWING_NAMES[4];  // off vertical horizontal both
extern const uint8_t SWING_VALUES[4];     // 0x00 0x01 0x10 0x11
const char *swingName(uint8_t v);         // nullptr si la valeur n'est pas l'une des quatre
int swingValue(const std::string &name);  // -1 si inconnu

struct State {
  bool valid = false;
  bool power = false;
  uint8_t mode = MODE_COOL;
  uint8_t fan = 0;        // 0..7
  uint8_t setpoint = 24;  // 16..31
  uint8_t swing = 0;      // octet brut : 0x00 fixe, 0x01 vertical, 0x10 latéral, 0x11 les deux
  bool silent = false;    // mode silence (bit 6 de l'octet 14 de l'état), ventilation auto
  float room_temp = 0;
  bool turbo_flag = false;
  std::string ref;        // texte ASCII des octets 25-31 de l'état (ex. "DB11510"), référence interne
  bool display = true;    // éclairage de l'afficheur (octet 16 de l'état, bit 7)
};

// Trame de commande A1 (24 octets) construite depuis l'état souhaité.
std::vector<uint8_t> buildCommand(const State &s);
// Décode une trame A3 (34 octets) valide. Faux si ce n'est pas un A3.
bool decodeStatus(const std::vector<uint8_t> &frame, State &out);

// Trame A6 : demande de nettoyage du filtre / code d'erreur.
struct Info {
  bool filter_clean = false;
  uint8_t error = 0;
};
bool decodeInfo(const std::vector<uint8_t> &frame, Info &out);

class Parser {
 public:
  // Retourne vrai quand une trame complète au CRC correct est dans `out`.
  bool feed(uint8_t b, std::vector<uint8_t> &out);
  uint32_t badCrc() const { return bad_crc_; }

 private:
  std::vector<uint8_t> buf_;
  uint32_t bad_crc_ = 0;
  bool step(uint8_t b, std::vector<uint8_t> &out);
  bool badHeader() const;
};

}  // namespace acw02
