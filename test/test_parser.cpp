// Test hôte (PC) : g++ -std=c++17 -I src test/test_parser.cpp src/tuya_mcu.cpp -o /tmp/t && /tmp/t
#include <cassert>
#include <cstdio>
#include "tuya_mcu.h"

using namespace tuya;

static int feedAll(Parser &p, const std::vector<uint8_t> &bytes, std::vector<Frame> &out) {
  int n = 0;
  Frame f;
  for (uint8_t b : bytes)
    if (p.feed(b, f)) { out.push_back(f); n++; }
  return n;
}

int main() {
  // Heartbeat module -> MCU : 55 AA 00 00 00 00 FF
  auto hb = buildFrame(0, CMD_HEARTBEAT, nullptr, 0);
  assert((hb == std::vector<uint8_t>{0x55, 0xAA, 0x00, 0x00, 0x00, 0x00, 0xFF}));

  // Réponse MCU avec double 0x55 (observée sur le forum) : 55 55 AA 03 04 00 00 06
  Parser p;
  std::vector<Frame> fr;
  assert(feedAll(p, {0x55, 0x55, 0xAA, 0x03, 0x04, 0x00, 0x00, 0x06}, fr) == 1);
  assert(fr[0].version == 3 && fr[0].cmd == 4 && fr[0].data.empty());

  // Mauvais checksum rejeté, puis trame valide acceptée juste après
  fr.clear();
  assert(feedAll(p, {0x55, 0xAA, 0x03, 0x04, 0x00, 0x00, 0x07}, fr) == 0);
  assert(p.badChecksums() == 1);
  assert(feedAll(p, {0x55, 0xAA, 0x03, 0x04, 0x00, 0x00, 0x06}, fr) == 1);

  // Bruit avant la trame
  fr.clear();
  assert(feedAll(p, {0x00, 0xFF, 0x13, 0x55, 0xAA, 0x03, 0x04, 0x00, 0x00, 0x06}, fr) == 1);

  // Aller-retour DP : bool, enum, value (24 °C)
  std::vector<uint8_t> payload;
  for (auto dp : {buildDpPayload(1, DP_BOOL, 1), buildDpPayload(4, DP_ENUM, 2), buildDpPayload(2, DP_VALUE, 24)})
    payload.insert(payload.end(), dp.begin(), dp.end());
  auto frame = buildFrame(0, CMD_DP_REPORT, payload.data(), payload.size());
  fr.clear();
  assert(feedAll(p, frame, fr) == 1);
  auto dps = parseDatapoints(fr[0].data);
  assert(dps.size() == 3);
  assert(dps[0].id == 1 && dps[0].asInt() == 1);
  assert(dps[1].id == 4 && dps[1].asInt() == 2);
  assert(dps[2].id == 2 && dps[2].asInt() == 24);

  // DP négatif (température -5)
  auto neg = buildDpPayload(3, DP_VALUE, -5);
  assert(parseDatapoints(neg)[0].asInt() == -5);

  // Trame tronquée dans parseDatapoints : pas de plantage
  std::vector<uint8_t> trunc{1, 2, 0, 4, 0, 0};
  assert(parseDatapoints(trunc).empty());

  puts("OK : tous les tests passent");
  return 0;
}
