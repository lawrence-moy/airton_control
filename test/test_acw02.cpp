// Test hôte : g++ -std=c++17 -I main test/test_acw02.cpp main/acw02.cpp -o /tmp/t2 && /tmp/t2
// Les trames viennent de captures publiées sur le forum HACF (clim Airton + module ACW02).
#include <cassert>
#include <cstdio>
#include <sstream>
#include <string>

#include "acw02.h"

using namespace acw02;
typedef std::vector<uint8_t> Bytes;

static Bytes H(const char *s) {
  Bytes v;
  std::istringstream in(s);
  std::string t;
  while (in >> t) v.push_back((uint8_t)strtol(t.c_str(), nullptr, 16));
  return v;
}

static const char *CAPTURES[] = {
    "7A 7A 21 D5 0C 00 00 AB 0A 0A FC F9",
    "7A 7A 21 D5 0C 00 00 A2 0A 0A FE 29",
    "7A 7A D1 21 0D 00 00 A4 0A 0A 00 25 25",
    "7A 7A D5 21 12 00 00 AC 0A 0A 06 52 00 12 BF 00 BB 98",
    "7A 7A D5 21 22 00 00 A3 0A 0A 19 00 00 6B 46 06 40 00 00 00 00 03 64 01 E8 44 42 31 31 33 37 30 09 56",
    "7A 7A D5 21 22 00 00 A3 0A 0A 1A 00 00 29 0A 02 00 00 00 00 00 01 64 12 BF 44 42 31 31 33 37 30 8F 7A",
    "7A 7A D5 21 22 00 00 A3 0A 0A 1A 05 00 39 0A 02 80 00 00 00 00 01 64 12 BF 44 42 31 31 33 37 30 B9 8A",
    "7A 7A D5 21 22 00 00 A3 0A 0A 1A 05 00 39 0A 02 00 00 00 00 00 01 64 12 BF 44 42 31 31 33 37 30 19 EB",
    "7A 7A D5 21 22 00 00 A3 0A 0A 1A 00 00 39 0A 02 00 00 00 00 00 01 64 12 BF 44 42 31 31 33 37 30 4C BE",
    "7A 7A D5 21 1C 00 00 A6 0A 0A 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 52 0B",
    "7A 7A 21 D1 1B 00 00 A5 0A 0A 01 03 00 00 00 00 00 00 02 D8 1F 12 92 5D 95 33 BC",
    "7A 7A 21 D1 1B 00 00 A5 0A 0A 01 03 00 00 00 00 01 00 02 D8 1F 12 92 5D 95 A3 B1",
};

int main() {
  // CRC et parseur sur toutes les captures
  for (const char *c : CAPTURES) {
    Bytes f = H(c);
    assert(f[4] == f.size());
    uint16_t crc = crc16(f.data(), f.size() - 2);
    assert(f[f.size() - 2] == (crc >> 8) && f[f.size() - 1] == (crc & 0xFF));
    Parser p;
    Bytes out;
    int n = 0;
    for (uint8_t b : f)
      if (p.feed(b, out)) n++;
    assert(n == 1 && out == f);
  }

  // Trames émises par le module : identiques aux captures
  assert(buildSimple(T_KEEPALIVE) == H("7A 7A 21 D5 0C 00 00 AB 0A 0A FC F9"));
  assert(buildSimple(T_GET_STATUS) == H("7A 7A 21 D5 0C 00 00 A2 0A 0A FE 29"));

  // Décodage de l'état : capture (b) = froid, ventilation 2, consigne 26, 26.0 C
  State s;
  assert(decodeStatus(H(CAPTURES[5]), s));
  assert(s.power && s.mode == MODE_COOL && s.fan == 2 && s.setpoint == 26 && s.swing == 2);
  assert(s.room_temp > 25.99f && s.room_temp < 26.01f);
  // capture (a) : ventilation seule, consigne 22, 25.0 C
  assert(decodeStatus(H(CAPTURES[4]), s));
  assert(s.power && s.mode == MODE_FAN && s.setpoint == 22 && s.room_temp > 24.99f && s.room_temp < 25.01f);
  // capture (c) : 26.5 C
  assert(decodeStatus(H(CAPTURES[6]), s));
  assert(s.room_temp > 26.49f && s.room_temp < 26.51f);
  assert(!decodeStatus(H(CAPTURES[0]), s));

  // Ventilation observée sur la clim réelle (octet 13) : 1C..5C = niveaux 1-5, DC = turbo, 6C = pourcentage, 0C = auto
  Bytes st = H(CAPTURES[5]);
  const uint8_t b13[] = {0x1C, 0x5C, 0xDC, 0x6C, 0x0C};
  const uint8_t fan[] = {1, 5, 7, 6, 0};
  for (int i = 0; i < 5; i++) {
    st[13] = b13[i];
    st.resize(32);
    seal(st);
    assert(decodeStatus(st, s) && s.fan == fan[i] && s.mode == MODE_HEAT && s.power);
    st.resize(32);
  }

  // Appui sur la touche « light » (capture réelle) : octet 16 = 0x94 (afficheur allumé) puis 0x14 (éteint)
  st = H(CAPTURES[5]);
  st[16] = 0x94;
  st.resize(32);
  seal(st);
  assert(decodeStatus(st, s) && s.display);
  st.resize(32);
  st[16] = 0x14;
  seal(st);
  assert(decodeStatus(st, s) && !s.display);
  State d;
  d.display = false;
  assert(buildCommand(d)[15] == 0x18);

  // Volets (capture réelle, octet 15) : 0x00 fixe, 0x01 vertical, 0x10 latéral, 0x11 les deux
  assert(swingName(0x00) && swingValue("vertical") == 0x01 && swingValue("horizontal") == 0x10);
  assert(swingName(0x11) == std::string("both") && swingName(0x01) == std::string("vertical"));
  assert(swingName(0x05) == nullptr && swingValue("x") == -1);

  // Mode silence : octet 14 = 0x43 (bit 6) avec ventilation auto (capture réelle)
  st = H(CAPTURES[5]);
  st[13] = 0x0C;
  st[14] = 0x43;
  st.resize(32);
  seal(st);
  assert(decodeStatus(st, s) && s.silent && s.fan == 0 && s.setpoint == 19);
  State sl;
  sl.power = true;
  sl.mode = MODE_HEAT;
  sl.setpoint = 19;
  sl.silent = true;
  assert(buildCommand(sl)[13] == 0x43);

  // Référence interne (octets 25-31, texte ASCII)
  assert(decodeStatus(H(CAPTURES[4]), s) && s.ref == "DB11370");

  // Trame A6
  Info inf;
  assert(decodeInfo(H(CAPTURES[9]), inf) && !inf.filter_clean && inf.error == 0);
  Bytes a6 = H(CAPTURES[9]);
  a6[10] = 0x80;
  a6[12] = 0x04;
  a6.resize(26);
  seal(a6);
  assert(decodeInfo(a6, inf) && inf.filter_clean && inf.error == 4);

  // Commande : froid / ventilation moyenne / 24 C / pas d'oscillation
  State w;
  w.power = true;
  w.mode = MODE_COOL;
  w.fan = 3;
  w.setpoint = 24;
  w.swing = 0;
  Bytes cmd = buildCommand(w);
  assert(cmd.size() == 24 && cmd[4] == 24 && cmd[7] == T_COMMAND);
  assert(cmd[12] == 0x39 && cmd[13] == 0x08 && cmd[14] == 0x00 && cmd[15] == 0x98);
  uint16_t cc = crc16(cmd.data(), 22);
  assert(cmd[22] == (cc >> 8) && cmd[23] == (cc & 0xFF));
  w.power = false;
  assert(buildCommand(w)[12] == 0x30);

  // Resynchronisation : bruit, trame tronquée puis trame valide
  Parser p;
  Bytes out, stream = {0x00, 0xFF, 0x7A, 0x13, 0x7A, 0x7A, 0x7A};
  Bytes good = H(CAPTURES[2]);
  stream.insert(stream.end(), good.begin(), good.end());
  int n = 0;
  for (uint8_t b : stream)
    if (p.feed(b, out)) n++;
  assert(n == 1 && out == good);

  // Trame valide précédée d'une trame au CRC faux : la deuxième est quand même reconnue
  Bytes bad = H(CAPTURES[2]);
  bad[11] ^= 0x01;
  Bytes two = bad;
  two.insert(two.end(), good.begin(), good.end());
  Parser q;
  n = 0;
  for (uint8_t b : two)
    if (q.feed(b, out)) n++;
  assert(n == 1 && out == good && q.badCrc() >= 1);

  // Deux trames collées
  Parser r;
  Bytes ab = H(CAPTURES[1]), ac = H(CAPTURES[0]);
  ab.insert(ab.end(), ac.begin(), ac.end());
  n = 0;
  for (uint8_t b : ab)
    if (r.feed(b, out)) n++;
  assert(n == 2);

  // En-tête BD BD
  setHeader(0xBD);
  Bytes bd = buildSimple(T_GET_STATUS);
  assert(bd[0] == 0xBD && bd[1] == 0xBD);
  Parser t;
  n = 0;
  for (uint8_t b : bd)
    if (t.feed(b, out)) n++;
  assert(n == 1);
  setHeader(0x7A);

  puts("acw02 : tous les tests passent");
  return 0;
}
