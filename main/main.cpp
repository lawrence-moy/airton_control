// Contrôleur ESP32 (ESP-IDF) pour clim Airton "non communicante" (UART 9600).
// L'ESP32 remplace le module WiFi ACW02. Deux protocoles : "7A 7A" (ACW02) et Tuya "55 AA",
// choisis dans menuconfig ou détectés automatiquement. Pilotage par la page web du module (WiFi).
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "ac_bridge.h"
#include "acw02.h"
#include "app_config.h"
#include "discovery.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ota.h"
#include "tuya_mcu.h"

static const char *TAG = "airton";
static const uart_port_t AC_UART = UART_NUM_1;

struct DpCache {
  bool known = false;
  uint8_t type = 0;
  int32_t value = 0;       // valeur numérique (bool, enum, value, bitmap)
  std::string text;        // valeur lisible, pour tous les types
  int64_t changed_ms = 0;  // uptime du dernier changement
};
static DpCache g_dp[256];
static std::mutex g_mx;     // protège g_dp
static std::mutex g_tx_mx;  // sérialise les écritures UART


static volatile bool g_mcu_alive = false;
static volatile bool g_handshake_sent = false;
static volatile int64_t g_last_rx_ms = 0;
static volatile uint32_t g_rx_frames = 0, g_bad_checksums = 0;

// Protocole actif : fixé par menuconfig ou détecté (premier protocole qui produit une trame valide).
enum Proto : int { P_AUTO = 0, P_TUYA = 1, P_ACW = 2 };
#if defined(CONFIG_AC_PROTO_ACW02)
static volatile int g_proto = P_ACW;
#elif defined(CONFIG_AC_PROTO_TUYA)
static volatile int g_proto = P_TUYA;
#else
static volatile int g_proto = P_AUTO;
#endif
static acw02::State g_ac;       // dernier état reçu (7A 7A), protégé par g_mx
static acw02::Info g_acinfo;    // dernière trame A6 (erreur, filtre), protégé par g_mx
static bool g_acinfo_known = false;
static volatile uint32_t g_acw_bad_crc = 0;
static volatile int64_t g_status_req_at = 0;  // demande d'état différée après une commande

static const char *proto_name() {
  return g_proto == P_ACW ? "7A7A" : g_proto == P_TUYA ? "tuya" : "auto";
}


static int64_t now_ms() { return esp_timer_get_time() / 1000; }

static std::string hex(const std::vector<uint8_t> &v) {
  std::string s;
  char b[4];
  for (uint8_t c : v) {
    snprintf(b, sizeof(b), "%02X ", c);
    s += b;
  }
  return s;
}

static DpCache get_dp(int id) {
  if (id < 0 || id > 255) return DpCache();
  std::lock_guard<std::mutex> l(g_mx);
  return g_dp[id];
}

// ------------------------------------------------------------------ série
static void send_frame(uint8_t cmd, const std::vector<uint8_t> &data = {}) {
  auto f = tuya::buildFrame(0x00, cmd, data.data(), data.size());
  {
    std::lock_guard<std::mutex> l(g_tx_mx);
    uart_write_bytes(AC_UART, f.data(), f.size());
  }
#ifdef CONFIG_AC_LOG_FRAMES
  ESP_LOGI(TAG, "> %s", hex(f).c_str());
#endif
  disc_on_frame(true, f);
}

static void send_acw(const std::vector<uint8_t> &f) {
  {
    std::lock_guard<std::mutex> l(g_tx_mx);
    uart_write_bytes(AC_UART, f.data(), f.size());
  }
#ifdef CONFIG_AC_LOG_FRAMES
  ESP_LOGI(TAG, "> %s", hex(f).c_str());
#endif
  disc_on_frame(true, f);
}

static void set_datapoint(uint8_t id, uint8_t type, int32_t value) {
  send_frame(tuya::CMD_DP_SET, tuya::buildDpPayload(id, type, value));
}

// ------------------------------------------------------------------ interface pour discovery.cpp
void ac_send_raw_cmd(uint8_t cmd, const std::vector<uint8_t> &data) {
  if (g_proto == P_ACW)
    send_acw(acw02::buildFrame(cmd, data));  // en-tête, longueur et CRC ajoutés
  else
    send_frame(cmd, data);
}

void ac_send_dp(uint8_t id, uint8_t type, int32_t value) {
  if (g_proto == P_ACW) {
    disc_sys("ecriture de DP indisponible en protocole 7A 7A");
    return;
  }
  set_datapoint(id, type, value);
}

void ac_request_state() {
  if (g_proto != P_TUYA) send_acw(acw02::buildSimple(acw02::T_GET_STATUS));
  if (g_proto != P_ACW) send_frame(tuya::CMD_DP_QUERY);
}

bool ac_has_dp_write() { return g_proto != P_ACW; }

std::vector<DpInfo> ac_dp_snapshot() {
  std::vector<DpInfo> out;
  std::lock_guard<std::mutex> l(g_mx);
  for (int i = 0; i < 256; i++)
    if (g_dp[i].known) out.push_back({(uint8_t)i, g_dp[i].type, g_dp[i].text, g_dp[i].changed_ms});
  return out;
}

AcStatus ac_status() {
  return {g_mcu_alive, g_rx_frames, g_bad_checksums + g_acw_bad_crc, g_last_rx_ms, proto_name()};
}

// ------------------------------------------------------------------ trames entrantes
static std::string dp_text(const tuya::Datapoint &dp) {
  if (dp.type == tuya::DP_STRING) return std::string(dp.value.begin(), dp.value.end());
  if (dp.type == tuya::DP_RAW || dp.value.size() > 4) {
    std::string s;
    char b[4];
    for (uint8_t c : dp.value) {
      snprintf(b, sizeof(b), "%02X", c);
      s += b;
    }
    return s;
  }
  return std::to_string(dp.asInt());
}

static void on_datapoint(const tuya::Datapoint &dp) {
  std::string text = dp_text(dp), old_text;
  bool had, changed;
  {
    std::lock_guard<std::mutex> l(g_mx);
    DpCache &c = g_dp[dp.id];
    had = c.known;
    old_text = c.text;
    changed = !had || c.text != text || c.type != dp.type;
    c.known = true;
    c.type = dp.type;
    c.text = text;
    c.value = (dp.type == tuya::DP_STRING || dp.type == tuya::DP_RAW) ? 0 : dp.asInt();
    if (changed) c.changed_ms = now_ms();
  }
  if (changed) disc_on_dp(dp.id, dp.type, had, old_text, text);
}

static void on_frame(const tuya::Frame &f) {
  g_last_rx_ms = now_ms();
  switch (f.cmd) {
    case tuya::CMD_HEARTBEAT:
      if (!g_mcu_alive) {
        g_mcu_alive = true;
        ESP_LOGI(TAG, "MCU detecte");
      }
      if (!g_handshake_sent) {
        g_handshake_sent = true;
        send_frame(tuya::CMD_PRODUCT_INFO);
        send_frame(tuya::CMD_WORK_MODE);
      }
      break;
    case tuya::CMD_PRODUCT_INFO: {
      std::string s(f.data.begin(), f.data.end());
      ESP_LOGI(TAG, "Produit: %s", s.c_str());
      break;
    }
    case tuya::CMD_WORK_MODE:
      // Pas de données => mode coopératif : c'est à nous d'annoncer l'état réseau.
      send_frame(tuya::CMD_NET_STATUS, {0x04});  // 04 = connecté au cloud (stoppe le clignotement WiFi)
      send_frame(tuya::CMD_DP_QUERY);
      break;
    case tuya::CMD_DP_REPORT:
      for (auto &dp : tuya::parseDatapoints(f.data)) on_datapoint(dp);
      break;
    case tuya::CMD_GET_TIME: {
      std::vector<uint8_t> d(8, 0);
      time_t now = time(nullptr);
      if (now > 1700000000) {
        struct tm t;
        localtime_r(&now, &t);
        d = {1, (uint8_t)(t.tm_year - 100), (uint8_t)(t.tm_mon + 1), (uint8_t)t.tm_mday,
             (uint8_t)t.tm_hour, (uint8_t)t.tm_min, (uint8_t)t.tm_sec,
             (uint8_t)(t.tm_wday == 0 ? 7 : t.tm_wday)};
      }
      send_frame(tuya::CMD_GET_TIME, d);
      break;
    }
    case tuya::CMD_RESET_WIFI:     // la carte demande un reset WiFi (ex. bouton de la télécommande)
    case tuya::CMD_RESET_SELECT: {  // idem avec choix du mode d'appairage
      char b[64];
      snprintf(b, sizeof(b), "la carte demande un reset WiFi (cmd 0x%02X), acquittement envoye", f.cmd);
      disc_sys(b);
      send_frame(f.cmd);
      break;
    }
    default: {
      char b[64];
      snprintf(b, sizeof(b), "commande non geree : 0x%02X (%u octets)", f.cmd, (unsigned)f.data.size());
      disc_sys(b);
      break;
    }
  }
}

// Octets des trames 7A 7A exposés comme pseudo-DP (id = base du type + position de l'octet) :
// la page de découverte montre ainsi chaque octet qui change quand on agit sur la télécommande.
static void store_pseudo(uint8_t id, uint8_t v) {
  char b[16];
  snprintf(b, sizeof(b), "%u (0x%02X)", v, v);
  std::string text = b, old_text;
  bool had, changed;
  {
    std::lock_guard<std::mutex> l(g_mx);
    DpCache &c = g_dp[id];
    had = c.known;
    old_text = c.text;
    changed = !had || c.text != text;
    c.known = true;
    c.type = tuya::DP_VALUE;
    c.text = text;
    c.value = v;
    if (changed) c.changed_ms = now_ms();
  }
  if (changed) {
    disc_on_dp(id, tuya::DP_VALUE, had, old_text, text);
  }
}

static void on_acw_frame(const std::vector<uint8_t> &f) {
  g_last_rx_ms = now_ms();
  if (!g_mcu_alive) {
    g_mcu_alive = true;
    ESP_LOGI(TAG, "Clim detectee (protocole 7A 7A)");
  }
  acw02::setHeader(f[0]);
  const uint8_t type = f[7];
  int base = -1;
  switch (type) {
    case acw02::T_STATUS: base = 0; break;     // ids 10..31
    case acw02::T_INFO: base = 64; break;      // ids 74..89
    case acw02::T_AC: base = 128; break;       // ids 138..143
    case acw02::T_PING: base = 160; break;     // id 170
    case acw02::T_PERIODIC: base = 192; break; // ids 202..216
    default: break;
  }
  if (base >= 0)
    for (size_t i = 10; i + 2 < f.size() && base + i < 256; i++) store_pseudo((uint8_t)(base + i), f[i]);

  if (type == acw02::T_STATUS) {
    acw02::State st;
    if (acw02::decodeStatus(f, st)) {
      {
        std::lock_guard<std::mutex> l(g_mx);
        g_ac = st;
      }
    }
  } else if (type == acw02::T_INFO) {
    static int last_err = -1, last_filter = -1;
    acw02::Info in;
    if (acw02::decodeInfo(f, in)) {
      {
        std::lock_guard<std::mutex> l(g_mx);
        g_acinfo = in;
        g_acinfo_known = true;
      }
      if (in.error != last_err) {
        last_err = in.error;
        if (in.error) disc_sys("code d'erreur de la clim : " + std::to_string(in.error));
      }
      if (in.filter_clean != last_filter) {
        last_filter = in.filter_clean;
        if (in.filter_clean) disc_sys("la clim demande un nettoyage du filtre");
      }
    }
  } else if (type != acw02::T_PING) {
    static bool seen[256];
    if (base < 0 && !seen[type]) {
      seen[type] = true;
      char b[64];
      snprintf(b, sizeof(b), "trame 7A 7A de type 0x%02X non geree (%u octets)", type, (unsigned)f.size());
      disc_sys(b);
    }
  }
}

static void uart_task(void *) {
  uint8_t buf[128];
  tuya::Parser parser;
  tuya::Frame f;
  acw02::Parser aparser;
  std::vector<uint8_t> af;
  std::vector<uint8_t> chunk;  // octets reçus depuis le dernier silence, pour diagnostiquer un mauvais débit/protocole
  bool chunk_framed = false;
  int64_t last_hb = 0, last_q = 0;
  for (;;) {
    int n = uart_read_bytes(AC_UART, buf, sizeof(buf), pdMS_TO_TICKS(20));
    for (int i = 0; i < n; i++) {
      if (chunk.size() < 64) chunk.push_back(buf[i]);
      if (g_proto != P_ACW && parser.feed(buf[i], f)) {
        if (g_proto == P_AUTO) {
          g_proto = P_TUYA;
          disc_sys("protocole detecte : Tuya 55 AA");
          ESP_LOGI(TAG, "Protocole detecte : Tuya");
        }
        chunk_framed = true;
        auto raw = tuya::buildFrame(f.version, f.cmd, f.data.data(), f.data.size());
#ifdef CONFIG_AC_LOG_FRAMES
        ESP_LOGI(TAG, "< %s", hex(raw).c_str());
#endif
        g_rx_frames = g_rx_frames + 1;
        disc_on_frame(false, raw);
        on_frame(f);
      }
      if (g_proto != P_TUYA && aparser.feed(buf[i], af)) {
        if (g_proto == P_AUTO) {
          g_proto = P_ACW;
          disc_sys("protocole detecte : Airton 7A 7A (CRC valide)");
          ESP_LOGI(TAG, "Protocole detecte : 7A 7A");
        }
        chunk_framed = true;
#ifdef CONFIG_AC_LOG_FRAMES
        ESP_LOGI(TAG, "< %s", hex(af).c_str());
#endif
        g_rx_frames = g_rx_frames + 1;
        disc_on_frame(false, af);
        on_acw_frame(af);
      }
    }
    if (n <= 0 && !chunk.empty()) {
      if (!chunk_framed) {
        std::string m = "octets recus mais non reconnus (" + std::to_string(chunk.size()) + ") : " + hex(chunk);
        disc_sys(m);
      }
      chunk.clear();
      chunk_framed = false;
    }
    g_bad_checksums = parser.badChecksums();
    g_acw_bad_crc = aparser.badCrc();
    int64_t now = now_ms();
    // Tuya : heartbeat 1 s tant que la clim ne répond pas, ensuite toutes les 15 s.
    if (g_proto != P_ACW && now - last_hb > (g_mcu_alive ? 15000 : 1000)) {
      last_hb = now;
      send_frame(tuya::CMD_HEARTBEAT);
    }
    // 7A 7A : la clim parle d'elle-même ; on demande l'état au début (3 s) puis toutes les 30 s.
    if (g_proto != P_TUYA) {
      bool due = now - last_q > ((g_proto == P_ACW && g_mcu_alive) ? 30000 : 3000);
      if (g_status_req_at && now >= g_status_req_at) {
        g_status_req_at = 0;
        due = true;
      }
      if (due) {
        last_q = now;
        send_acw(acw02::buildSimple(acw02::T_GET_STATUS));
      }
    }
    // Silence prolongé : on refait le handshake (Tuya) / on déclare la clim muette (7A 7A).
    if (g_mcu_alive && now - g_last_rx_ms > (g_proto == P_ACW ? 90000 : 45000)) {
      g_mcu_alive = false;
      g_handshake_sent = false;
      ESP_LOGW(TAG, "Clim silencieuse");
    }
  }
}

// Broches effectives (l'option d'inversion échange les deux réglages).
#ifdef CONFIG_AC_SWAP_TX_RX
#define AC_TX_GPIO CONFIG_AC_UART_RX_PIN
#define AC_RX_GPIO CONFIG_AC_UART_TX_PIN
#else
#define AC_TX_GPIO CONFIG_AC_UART_TX_PIN
#define AC_RX_GPIO CONFIG_AC_UART_RX_PIN
#endif
// Les GPIO 34 à 39 de l'ESP32 sont en entrée seulement : ils ne peuvent pas émettre.
#if AC_TX_GPIO >= 34 && AC_TX_GPIO <= 39
#error "Le GPIO d'emission (TX) ne peut pas etre 34-39 (entree seule). Mettez le GPIO35 en reception (RX) et un GPIO comme 33 en emission (TX)."
#endif

static void uart_setup() {
  uart_config_t uc = {};
  uc.baud_rate = CONFIG_AC_UART_BAUD;
  uc.data_bits = UART_DATA_8_BITS;
  uc.parity = UART_PARITY_DISABLE;
  uc.stop_bits = UART_STOP_BITS_1;
  uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uc.source_clk = UART_SCLK_DEFAULT;
  ESP_ERROR_CHECK(uart_driver_install(AC_UART, 1024, 0, 0, nullptr, 0));
  ESP_ERROR_CHECK(uart_param_config(AC_UART, &uc));
  const int tx = AC_TX_GPIO, rx = AC_RX_GPIO;
  ESP_ERROR_CHECK(uart_set_pin(AC_UART, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

// ------------------------------------------------------------------ commandes (page de découverte)
static int index_of(const char *const *names, size_t n, const std::string &v) {
  for (size_t i = 0; i < n; i++)
    if (v == names[i]) return (int)i;
  return -1;
}

std::string ac_state_text() {
  if (g_proto != P_ACW) return "";
  acw02::State s;
  {
    std::lock_guard<std::mutex> l(g_mx);
    s = g_ac;
  }
  if (!s.valid) return "etat de la clim pas encore recu";
  char b[220], hx[8];
  snprintf(hx, sizeof(hx), "0x%02X", s.swing);
  const char *sw = acw02::swingName(s.swing);
  snprintf(b, sizeof(b), "%s | mode %s | ventilation %s | consigne %u C | ambiante %.1f C | volets %s | afficheur %s",
           s.power ? "MARCHE" : "ARRET", acw02::MODE_NAMES[s.mode < 5 ? s.mode : 0],
           s.silent ? "silent" : acw02::FAN_NAMES[s.fan & 7], s.setpoint, s.room_temp, sw ? sw : hx,
           s.display ? "allume" : "eteint");
  return b;
}

std::string ac_state_json() {
  if (g_proto != P_ACW) return "null";
  acw02::State s;
  {
    std::lock_guard<std::mutex> l(g_mx);
    s = g_ac;
  }
  if (!s.valid) return "null";
  const char *sw = acw02::swingName(s.swing);
  char hx[8];
  snprintf(hx, sizeof(hx), "0x%02X", s.swing);
  acw02::Info info;
  bool info_known;
  {
    std::lock_guard<std::mutex> l(g_mx);
    info = g_acinfo;
    info_known = g_acinfo_known;
  }
  std::string ref;
  for (char c : s.ref) ref += (isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_' || c == '.') ? c : '?';
  char b[384];
  snprintf(b, sizeof(b),
           "{\"power\":\"%s\",\"mode\":\"%s\",\"fan\":\"%s\",\"temp\":%u,\"room\":%.1f,\"swing\":\"%s\",\"display\":\"%s\",\"err\":%d,\"filter\":%d,\"ref\":\"%s\"}",
           s.power ? "on" : "off", acw02::MODE_NAMES[s.mode < 5 ? s.mode : 0],
           s.silent ? "silent" : acw02::FAN_NAMES[s.fan & 7], s.setpoint, s.room_temp, sw ? sw : hx,
           s.display ? "on" : "off", info_known ? (int)info.error : -1, info_known ? (info.filter_clean ? 1 : 0) : -1,
           ref.c_str());
  return b;
}

std::string ac_apply(const std::string &args) {
  if (g_proto != P_ACW) return "indisponible : protocole 7A 7A non actif";
  acw02::State s;
  {
    std::lock_guard<std::mutex> l(g_mx);
    s = g_ac;
  }
  if (!s.valid) return "refuse : aucun etat recu de la clim (attendez ou tapez 'query')";
  std::istringstream in(args);
  std::string tok;
  while (in >> tok) {
    size_t eq = tok.find('=');
    if (eq == std::string::npos) return "syntaxe : champ=valeur (ex: temp=20), recu : " + tok;
    std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
    if (k == "power") {
      if (v != "on" && v != "off") return "power=on|off";
      s.power = v == "on";
    } else if (k == "mode") {
      int i = index_of(acw02::MODE_NAMES, 5, v);
      if (i < 0) return "mode inconnu : " + v + " (auto cool dry fan_only heat)";
      s.mode = (uint8_t)i;
    } else if (k == "fan") {
      if (v == "silent") {
        s.silent = true;
        s.fan = 0;
      } else {
        int i = index_of(acw02::FAN_NAMES, 8, v);
        if (i < 0 && v.size() == 1 && v[0] >= '0' && v[0] <= '7') i = v[0] - '0';
        if (i < 0) return "ventilation inconnue : " + v;
        s.fan = (uint8_t)i;
        s.silent = false;
      }
    } else if (k == "temp") {
      int t = atoi(v.c_str());
      if (t < 16 || t > 31) return "temp : 16 a 31";
      s.setpoint = (uint8_t)t;
    } else if (k == "swing") {
      int i = acw02::swingValue(v);
      if (i < 0) {
        char *end = nullptr;
        long n = strtol(v.c_str(), &end, 0);
        if (v.empty() || *end || n < 0 || n > 255) return "volets : nom, ou nombre 0-255 (ex: 0x11)";
        i = (int)n;
      }
      s.swing = (uint8_t)i;
    } else if (k == "display") {
      if (v != "on" && v != "off") return "display=on|off";
      s.display = v == "on";
    } else {
      return "champ inconnu : " + k + " (power mode fan temp swing display)";
    }
  }
  {
    std::lock_guard<std::mutex> l(g_mx);
    g_ac = s;
  }
  auto f = acw02::buildCommand(s);
  send_acw(f);
  g_status_req_at = now_ms() + 1500;  // relit l'etat pour voir si la clim a obei
  return "commande envoyee : " + hex(f) + "\nL'etat sera relu dans 1,5 s.";
}

// ------------------------------------------------------------------ réseau
// Heure (SNTP) : seulement quand le module est connecté à votre box.
static void start_net_services() {
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();
}

// Serveur web (page de découverte + mise à jour OTA) : dès que le WiFi est prêt, point d'accès ou box.
static void start_http() {
  httpd_handle_t srv = ota_start(CONFIG_AC_OTA_TOKEN);
  if (srv) {
    disc_init(srv, CONFIG_AC_OTA_TOKEN);
    ota_mark_valid();  // le firmware tourne et le réseau est prêt : on annule le retour arrière
  }
}

// Le démarrage des services se fait dans une tâche dédiée : la tâche d'événements WiFi a une pile trop petite.
enum { SVC_HTTP = 1, SVC_NET = 2 };

static void svc_task(void *arg) {
  int kind = (int)(intptr_t)arg;
  if (kind & SVC_NET) start_net_services();
  if (kind & SVC_HTTP) start_http();
  vTaskDelete(nullptr);
}

static void spawn_services(int kind) {
  xTaskCreate(svc_task, "svc", 6144, (void *)(intptr_t)kind, 5, nullptr);
}

static void net_event(void *, esp_event_base_t base, int32_t id, void *) {
  static bool http_started = false, net_started = false;
  if (base == WIFI_EVENT) {
#if !defined(CONFIG_AC_WIFI_MODE_AP)
    if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
#endif
    if (id == WIFI_EVENT_AP_START && !http_started) {
      http_started = true;
      spawn_services(SVC_HTTP);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    int kind = 0;
    if (!net_started) { net_started = true; kind |= SVC_NET; }
    if (!http_started) { http_started = true; kind |= SVC_HTTP; }
    if (kind) spawn_services(kind);
  }
}

static void wifi_setup() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
#if !defined(CONFIG_AC_WIFI_MODE_AP)
  esp_netif_t *sta = esp_netif_create_default_wifi_sta();
  esp_netif_set_hostname(sta, DEVICE_ID);
#endif
#if !defined(CONFIG_AC_WIFI_MODE_STA)
  esp_netif_create_default_wifi_ap();  // adresse 192.168.4.1, serveur DHCP inclus
#endif
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &net_event, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &net_event, nullptr));

#if defined(CONFIG_AC_WIFI_MODE_AP)
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
#elif defined(CONFIG_AC_WIFI_MODE_APSTA)
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
#else
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#endif

#if !defined(CONFIG_AC_WIFI_MODE_AP)
  wifi_config_t wc = {};
  strncpy((char *)wc.sta.ssid, CONFIG_AC_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
  strncpy((char *)wc.sta.password, CONFIG_AC_WIFI_PASS, sizeof(wc.sta.password) - 1);
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
#endif
#if !defined(CONFIG_AC_WIFI_MODE_STA)
  wifi_config_t ap = {};
  strncpy((char *)ap.ap.ssid, CONFIG_AC_AP_SSID, sizeof(ap.ap.ssid) - 1);
  ap.ap.ssid_len = strlen((const char *)ap.ap.ssid);
  strncpy((char *)ap.ap.password, CONFIG_AC_AP_PASS, sizeof(ap.ap.password) - 1);
  ap.ap.channel = CONFIG_AC_AP_CHANNEL;
  ap.ap.max_connection = 4;
  ap.ap.authmode = strlen(CONFIG_AC_AP_PASS) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
#endif
  ESP_ERROR_CHECK(esp_wifi_start());
}

static const char *reset_reason_name() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "mise sous tension";
    case ESP_RST_SW: return "redemarrage logiciel";
    case ESP_RST_PANIC: return "plantage du programme (panic)";
    case ESP_RST_INT_WDT: return "watchdog d'interruptions";
    case ESP_RST_TASK_WDT: return "watchdog de tache";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "CHUTE DE TENSION (brownout) : alimentation insuffisante";
    default: return "autre";
  }
}

extern "C" void app_main(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  ESP_LOGW(TAG, "Dernier redemarrage : %s", reset_reason_name());
  ESP_LOGI(TAG, "Airton ESP32 - demarrage (protocole : %s)", proto_name());
  setenv("TZ", CONFIG_AC_TIMEZONE, 1);
  tzset();
  uart_setup();
  xTaskCreate(uart_task, "uart_task", 6144, nullptr, 5, nullptr);
  wifi_setup();
}
