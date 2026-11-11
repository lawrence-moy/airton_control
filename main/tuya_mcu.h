// Protocole série "Tuya MCU" côté module WiFi (notre ESP32 joue le rôle du module).
// Trame : 55 AA | version | cmd | len(2, big endian) | data... | checksum (somme mod 256)
// Indépendant d'Arduino pour pouvoir être testé sur PC.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace tuya {

enum Cmd : uint8_t {
  CMD_HEARTBEAT = 0x00,
  CMD_PRODUCT_INFO = 0x01,
  CMD_WORK_MODE = 0x02,
  CMD_NET_STATUS = 0x03,
  CMD_RESET_WIFI = 0x04,
  CMD_RESET_SELECT = 0x05,
  CMD_DP_SET = 0x06,     // module -> MCU : écrire un datapoint
  CMD_DP_REPORT = 0x07,  // MCU -> module : état d'un datapoint
  CMD_DP_QUERY = 0x08,   // module -> MCU : demander tous les états
  CMD_GET_TIME = 0x1C,   // MCU -> module : demande l'heure locale
};

enum DpType : uint8_t {
  DP_RAW = 0,
  DP_BOOL = 1,
  DP_VALUE = 2,  // entier 32 bits big endian
  DP_STRING = 3,
  DP_ENUM = 4,   // 1 octet
  DP_BITMAP = 5,
};

struct Frame {
  uint8_t version = 0;
  uint8_t cmd = 0;
  std::vector<uint8_t> data;
};

struct Datapoint {
  uint8_t id = 0;
  uint8_t type = 0;
  std::vector<uint8_t> value;
  int32_t asInt() const;  // BOOL / ENUM / VALUE / BITMAP
};

// Machine à états octet par octet. Gère le "double 0x55" observé sur certaines
// clims (55 55 AA ...) : un 0x55 supplémentaire avant 0xAA est ignoré.
class Parser {
 public:
  // Retourne true quand `out` contient une trame complète au checksum valide.
  bool feed(uint8_t b, Frame &out);
  uint32_t badChecksums() const { return bad_checksums_; }

 private:
  enum State : uint8_t { H1, H2, VER, CMD, LEN_H, LEN_L, DATA, SUM };
  static constexpr size_t MAX_LEN = 512;
  State state_ = H1;
  uint8_t sum_ = 0;
  uint16_t len_ = 0;
  Frame cur_;
  uint32_t bad_checksums_ = 0;
};

std::vector<uint8_t> buildFrame(uint8_t version, uint8_t cmd, const uint8_t *data, size_t len);
std::vector<Datapoint> parseDatapoints(const std::vector<uint8_t> &data);
// Charge utile d'un CMD_DP_SET pour un seul datapoint.
std::vector<uint8_t> buildDpPayload(uint8_t id, uint8_t type, int32_t value);

}  // namespace tuya
