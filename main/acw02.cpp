#include "acw02.h"

namespace acw02 {

const char *const MODE_NAMES[5] = {"auto", "cool", "dry", "fan_only", "heat"};
const char *const FAN_NAMES[8] = {"auto", "low", "low_medium", "medium", "medium_high", "high", "stepless", "turbo"};
const char *const SWING_NAMES[4] = {"off", "vertical", "horizontal", "both"};
const uint8_t SWING_VALUES[4] = {0x00, 0x01, 0x10, 0x11};

const char *swingName(uint8_t v) {
  for (int i = 0; i < 4; i++)
    if (SWING_VALUES[i] == v) return SWING_NAMES[i];
  return nullptr;
}

int swingValue(const std::string &n) {
  for (int i = 0; i < 4; i++)
    if (n == SWING_NAMES[i]) return SWING_VALUES[i];
  return -1;
}

static uint8_t g_hdr = 0x7A;

void setHeader(uint8_t h) { g_hdr = h; }
uint8_t header() { return g_hdr; }

uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
  }
  return c;
}

void seal(std::vector<uint8_t> &f) {
  uint16_t c = crc16(f.data(), f.size());
  f.push_back(c >> 8);    // poids fort d'abord (vérifié sur les captures)
  f.push_back(c & 0xFF);
}

std::vector<uint8_t> buildFrame(uint8_t type, const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> f = {g_hdr, g_hdr, 0x21, 0xD5, 0, 0x00, 0x00, type};
  f.insert(f.end(), payload.begin(), payload.end());
  f[4] = (uint8_t)(f.size() + 2);
  seal(f);
  return f;
}

std::vector<uint8_t> buildSimple(uint8_t type) { return buildFrame(type, {0x0A, 0x0A}); }

std::vector<uint8_t> buildCommand(const State &s) {
  uint8_t sp = s.setpoint < 16 ? 16 : s.setpoint > 31 ? 31 : s.setpoint;
  uint8_t b12 = (uint8_t)((s.fan & 7) << 4);
  if (s.power) b12 |= 0x08 | (s.mode & 7);
  std::vector<uint8_t> p = {0x00, 0x00, 0x00, 0x00, b12, (uint8_t)((sp - 16) | (s.silent ? 0x40 : 0)), s.swing, (uint8_t)(0x18 | (s.display ? 0x80 : 0)),
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  return buildFrame(T_COMMAND, p);
}

bool decodeStatus(const std::vector<uint8_t> &f, State &o) {
  if (f.size() != 34 || f[7] != T_STATUS) return false;
  o.valid = true;
  o.room_temp = f[10] + f[11] / 10.0f;
  uint8_t b13 = f[13];
  o.mode = b13 & 7;
  o.power = (b13 & 0x08) != 0;
  o.fan = (b13 >> 4) & 7;
  o.turbo_flag = (b13 & 0x80) != 0;
  if (o.turbo_flag) o.fan = 7;  // observé : turbo = ventilation 5 + bit 7
  o.setpoint = (uint8_t)((f[14] & 0x0F) + 16);
  o.swing = f[15];
  o.silent = (f[14] & 0x40) != 0;
  o.display = (f[16] & 0x80) != 0;
  std::string r;
  for (size_t i = 25; i < 32; i++) {
    if (f[i] < 0x20 || f[i] > 0x7E) {
      r.clear();
      break;
    }
    r += (char)f[i];
  }
  o.ref = r;
  return true;
}

bool decodeInfo(const std::vector<uint8_t> &f, Info &o) {
  if (f.size() != 28 || f[7] != T_INFO) return false;
  o.filter_clean = (f[10] & 0x80) != 0;
  o.error = f[12];
  return true;
}

// Les octets 5-6 valent 00 00 et l'octet 7 est un type A0-AF dans toutes les trames connues.
bool Parser::badHeader() const {
  size_t n = buf_.size();
  return (n >= 6 && buf_[5] != 0) || (n >= 7 && buf_[6] != 0) || (n >= 8 && (buf_[7] & 0xF0) != 0xA0);
}

// Une étape de la machine à états : accepte un octet, peut retourner une trame complète.
bool Parser::step(uint8_t b, std::vector<uint8_t> &out) {
  buf_.push_back(b);
  size_t n = buf_.size();
  if (n == 1) {
    if (b != 0x7A && b != 0xBD) buf_.clear();
    return false;
  }
  if (n == 2) {
    if (b != buf_[0]) {
      buf_.clear();
      if (b == 0x7A || b == 0xBD) buf_.push_back(b);
    }
    return false;
  }
  if (badHeader()) {  // fausse synchro détectée dès les octets 5 à 7
    buf_.erase(buf_.begin());
    return false;
  }
  if (n >= 5) {
    size_t len = buf_[4];
    if (len < MIN_LEN || len > MAX_LEN) {  // longueur absurde : fausse synchro
      buf_.erase(buf_.begin());
      return false;  // l'appelant ré-examine le reste (voir feed)
    }
    if (n == len) {
      uint16_t c = crc16(buf_.data(), n - 2);
      if (buf_[n - 2] == (c >> 8) && buf_[n - 1] == (c & 0xFF)) {
        out = buf_;
        buf_.clear();
        return true;
      }
      bad_crc_++;
      buf_.erase(buf_.begin());
    }
  }
  return false;
}

bool Parser::feed(uint8_t b, std::vector<uint8_t> &out) {
  if (step(b, out)) return true;
  // Après un rejet, le tampon peut encore contenir un début de trame valide : on le rejoue.
  for (;;) {
    size_t n = buf_.size();
    bool bad_head = n >= 1 && buf_[0] != 0x7A && buf_[0] != 0xBD;
    bool bad_len = n >= 5 && (buf_[4] < MIN_LEN || buf_[4] > MAX_LEN);
    bool bad_pair = n >= 2 && buf_[1] != buf_[0];
    bool bad_hdr = badHeader();
    if (!(bad_head || bad_len || bad_pair || bad_hdr)) break;
    std::vector<uint8_t> rest(buf_.begin() + 1, buf_.end());
    buf_.clear();
    for (uint8_t x : rest) {
      if (step(x, out)) return true;
    }
  }
  return false;
}

}  // namespace acw02
