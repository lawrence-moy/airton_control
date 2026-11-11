#include "tuya_mcu.h"

namespace tuya {

int32_t Datapoint::asInt() const {
  if (value.empty()) return 0;
  if (type == DP_VALUE && value.size() == 4) {
    uint32_t v = (uint32_t(value[0]) << 24) | (uint32_t(value[1]) << 16) |
                 (uint32_t(value[2]) << 8) | uint32_t(value[3]);
    return int32_t(v);
  }
  return value[0];
}

bool Parser::feed(uint8_t b, Frame &out) {
  switch (state_) {
    case H1:
      if (b == 0x55) state_ = H2;
      break;
    case H2:
      if (b == 0xAA) {
        sum_ = 0x55 + 0xAA;
        cur_.data.clear();
        state_ = VER;
      } else if (b != 0x55) {  // 55 55 AA : on reste en attente du AA
        state_ = H1;
      }
      break;
    case VER:
      cur_.version = b;
      sum_ += b;
      state_ = CMD;
      break;
    case CMD:
      cur_.cmd = b;
      sum_ += b;
      state_ = LEN_H;
      break;
    case LEN_H:
      len_ = uint16_t(b) << 8;
      sum_ += b;
      state_ = LEN_L;
      break;
    case LEN_L:
      len_ |= b;
      sum_ += b;
      if (len_ > MAX_LEN) {
        state_ = H1;
      } else {
        cur_.data.reserve(len_);
        state_ = len_ ? DATA : SUM;
      }
      break;
    case DATA:
      cur_.data.push_back(b);
      sum_ += b;
      if (cur_.data.size() == len_) state_ = SUM;
      break;
    case SUM:
      state_ = H1;
      if (b == sum_) {
        out = cur_;
        return true;
      }
      bad_checksums_++;
      break;
  }
  return false;
}

std::vector<uint8_t> buildFrame(uint8_t version, uint8_t cmd, const uint8_t *data, size_t len) {
  std::vector<uint8_t> f;
  f.reserve(len + 7);
  f.push_back(0x55);
  f.push_back(0xAA);
  f.push_back(version);
  f.push_back(cmd);
  f.push_back(uint8_t(len >> 8));
  f.push_back(uint8_t(len & 0xFF));
  for (size_t i = 0; i < len; i++) f.push_back(data[i]);
  uint8_t s = 0;
  for (uint8_t b : f) s += b;
  f.push_back(s);
  return f;
}

std::vector<Datapoint> parseDatapoints(const std::vector<uint8_t> &d) {
  std::vector<Datapoint> out;
  size_t i = 0;
  while (i + 4 <= d.size()) {
    Datapoint dp;
    dp.id = d[i];
    dp.type = d[i + 1];
    size_t n = (size_t(d[i + 2]) << 8) | d[i + 3];
    i += 4;
    if (i + n > d.size()) break;  // trame tronquée
    dp.value.assign(d.begin() + i, d.begin() + i + n);
    i += n;
    out.push_back(dp);
  }
  return out;
}

std::vector<uint8_t> buildDpPayload(uint8_t id, uint8_t type, int32_t value) {
  std::vector<uint8_t> p{id, type};
  if (type == DP_VALUE) {
    p.push_back(0);
    p.push_back(4);
    uint32_t v = uint32_t(value);
    p.push_back(uint8_t(v >> 24));
    p.push_back(uint8_t(v >> 16));
    p.push_back(uint8_t(v >> 8));
    p.push_back(uint8_t(v));
  } else {  // BOOL, ENUM, BITMAP(1 octet)
    p.push_back(0);
    p.push_back(1);
    p.push_back(uint8_t(value));
  }
  return p;
}

}  // namespace tuya
