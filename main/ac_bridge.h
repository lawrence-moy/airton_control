#pragma once
// Interface entre le module découverte (discovery.cpp) et le cœur du firmware (main.cpp).
#include <stdint.h>

#include <string>
#include <vector>

struct DpInfo {
  uint8_t id;
  uint8_t type;
  std::string text;     // valeur lisible (nombre, texte ou hexadécimal)
  int64_t changed_ms;   // date (uptime) du dernier changement
};

struct AcStatus {
  bool mcu_alive;
  uint32_t rx_frames;
  uint32_t bad_checksums;
  int64_t last_rx_ms;  // 0 = rien reçu
  const char *proto;   // "auto", "tuya" ou "7A7A"
};

// Implémentées dans main.cpp
void ac_send_raw_cmd(uint8_t cmd, const std::vector<uint8_t> &data);
void ac_send_dp(uint8_t id, uint8_t type, int32_t value);
// Demande l'état complet (Tuya : DP query ; 7A 7A : trame A2).
void ac_request_state();
// Vrai si les DP Tuya (set/sweep) sont utilisables.
bool ac_has_dp_write();
// Protocole 7A 7A : état décodé en clair ("" si le protocole n'est pas actif).
std::string ac_state_text();
// Même état en JSON pour la page ("null" si inconnu) : power, mode, fan, temp, room, swing, display.
std::string ac_state_json();
// Protocole 7A 7A : applique "power=on mode=heat fan=3 temp=20 swing=0x11 display=off" (champs facultatifs)
// à l'état courant et envoie la commande. Retourne un message lisible.
std::string ac_apply(const std::string &args);
std::vector<DpInfo> ac_dp_snapshot();
AcStatus ac_status();
