// Mode découverte : journal des événements, commandes texte et page web.
#include "discovery.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <sstream>

#include "ac_bridge.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tuya_mcu.h"

static const char *TAG = "disc";

struct Event {
  uint32_t seq;
  std::string ts;
  std::string kind;  // dp, note, tx, rx, sys
  std::string text;
};

static std::deque<Event> g_events;
static std::mutex g_mx;
static uint32_t g_seq = 0;
static const size_t MAX_EVENTS = 300;
static std::string g_token;
static volatile int64_t g_write_until = 0;
static volatile bool g_sweep_run = false, g_sweep_stop = false;

static int64_t now_ms() { return esp_timer_get_time() / 1000; }

static std::string stamp() {
  char b[24];
  time_t t = time(nullptr);
  if (t > 1700000000) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(b, sizeof(b), "%H:%M:%S", &tm);
  } else {
    snprintf(b, sizeof(b), "+%.1fs", now_ms() / 1000.0);
  }
  return b;
}

static void add_event(const char *kind, const std::string &text) {
  std::string ts = stamp();
  std::lock_guard<std::mutex> l(g_mx);
  g_events.push_back({++g_seq, ts, kind, text});
  while (g_events.size() > MAX_EVENTS) g_events.pop_front();
}

static const char *type_name(int t) {
  switch (t) {
    case tuya::DP_RAW: return "raw";
    case tuya::DP_BOOL: return "bool";
    case tuya::DP_VALUE: return "value";
    case tuya::DP_STRING: return "string";
    case tuya::DP_ENUM: return "enum";
    case tuya::DP_BITMAP: return "bitmap";
  }
  return "?";
}

static int type_from_name(const std::string &s) {
  if (s == "bool") return tuya::DP_BOOL;
  if (s == "value") return tuya::DP_VALUE;
  if (s == "enum") return tuya::DP_ENUM;
  return -1;
}

static std::string hex(const std::vector<uint8_t> &v) {
  std::string s;
  char b[4];
  for (uint8_t c : v) {
    snprintf(b, sizeof(b), "%02X ", c);
    s += b;
  }
  if (!s.empty()) s.pop_back();
  return s;
}

static std::string json_escape(const std::string &s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += (char)c;
    } else if (c < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", c);
      o += b;
    } else {
      o += (char)c;
    }
  }
  return o;
}

// ------------------------------------------------------------------ hooks
void disc_on_dp(uint8_t id, uint8_t type, bool had_before, const std::string &old_text,
                const std::string &new_text) {
  std::string t = "DP " + std::to_string(id) + " " + type_name(type);
  if (had_before)
    t += ": " + old_text + " -> " + new_text;
  else
    t += " = " + new_text + " (nouveau)";
  add_event("dp", t);
}

void disc_on_frame(bool tx, const std::vector<uint8_t> &frame) {
  add_event(tx ? "tx" : "rx", hex(frame));
}

void disc_sys(const std::string &msg) { add_event("sys", msg); }

// ------------------------------------------------------------------ commandes
struct SweepArgs {
  int id, min, max, delay_s;
};

static void sweep_task(void *a) {
  SweepArgs *s = (SweepArgs *)a;
  g_sweep_run = true;
  for (int v = s->min; v <= s->max && !g_sweep_stop; v++) {
    if (now_ms() >= g_write_until) {
      add_event("sys", "balayage arrete : ecriture verrouillee");
      break;
    }
    add_event("sys", "balayage DP " + std::to_string(s->id) + " enum = " + std::to_string(v));
    ac_send_dp((uint8_t)s->id, tuya::DP_ENUM, v);
    for (int i = 0; i < s->delay_s * 10 && !g_sweep_stop; i++) vTaskDelay(pdMS_TO_TICKS(100));
  }
  add_event("sys", g_sweep_stop ? "balayage interrompu" : "balayage termine");
  g_sweep_run = false;
  delete s;
  vTaskDelete(nullptr);
}

static const char *HELP =
    "Lecture : status, query, info, dps, note <texte>, clear\n"
    "Ecriture (apres 'write on', valable 5 min) :\n"
    "  set <id> <bool|enum|value> <n>\n"
    "  raw <cmd hex> [octets hex...]     Tuya : raw 08 ; 7A 7A : raw A2 0A 0A (en-tete, longueur et CRC ajoutes)\n"
    "  sweep <id> <min> <max> [delai_s]  balaye un enum, 'stop' pour arreter\n"
    "write on|off|status\n"
    "Protocole 7A 7A : ac state | ac power=on|off mode=auto|cool|dry|fan_only|heat fan=auto|low|...|stepless|turbo|silent\n"
    "  temp=16..31 swing=off|vertical|horizontal|both display=on|off   (champs facultatifs, necessite 'write on')";

std::string disc_run(const std::string &line) {
  std::istringstream in(line);
  std::string c;
  in >> c;
  if (c.empty()) return "";
  if (c == "help") return HELP;

  if (c == "status") {
    AcStatus s = ac_status();
    char b[200];
    snprintf(b, sizeof(b), "Protocole: %s | Clim: %s | trames recues: %u | CRC faux: %u | silence: %.1fs",
             s.proto, s.mcu_alive ? "oui" : "non", (unsigned)s.rx_frames,
             (unsigned)s.bad_checksums, s.last_rx_ms ? (now_ms() - s.last_rx_ms) / 1000.0 : -1.0);
    return b;
  }
  if (c == "query") {
    ac_request_state();
    return "etat complet demande";
  }
  if (c == "info") {
    if (!ac_has_dp_write()) {
      ac_request_state();
      return "protocole 7A 7A : etat demande (pas d'infos produit)";
    }
    ac_send_raw_cmd(tuya::CMD_PRODUCT_INFO, {});
    ac_send_raw_cmd(tuya::CMD_WORK_MODE, {});
    return "infos produit et mode de travail demandes";
  }
  if (c == "note") {
    std::string rest;
    std::getline(in, rest);
    size_t p = rest.find_first_not_of(' ');
    rest = p == std::string::npos ? "" : rest.substr(p);
    if (rest.empty()) return "usage: note <texte>";
    add_event("note", rest);
    return "ok";
  }
  if (c == "clear") {
    std::lock_guard<std::mutex> l(g_mx);
    g_events.clear();
    return "journal efface";
  }
  if (c == "dps") {
    std::string o;
    for (auto &d : ac_dp_snapshot())
      o += "DP " + std::to_string(d.id) + "  " + type_name(d.type) + "  " + d.text + "\n";
    return o.empty() ? "aucun DP recu pour l'instant" : o;
  }
  if (c == "stop") {
    g_sweep_stop = true;
    return "arret demande";
  }
  if (c == "write") {
    std::string a;
    in >> a;
    if (a == "on") g_write_until = now_ms() + 300000;
    else if (a == "off") g_write_until = 0;
    int64_t left = g_write_until - now_ms();
    return left > 0 ? "ecriture autorisee encore " + std::to_string(left / 1000) + " s" : "ecriture verrouillee";
  }

  if (c == "ac") {
    std::string rest;
    std::getline(in, rest);
    size_t p = rest.find_first_not_of(' ');
    rest = p == std::string::npos ? "" : rest.substr(p);
    if (rest.empty() || rest == "state") {
      std::string t = ac_state_text();
      return t.empty() ? "protocole 7A 7A non actif" : t;
    }
    if (now_ms() >= g_write_until) return "ecriture verrouillee : tapez 'write on' (valable 5 min)";
    return ac_apply(rest);
  }
  if (c == "set" || c == "raw" || c == "sweep") {
    if (now_ms() >= g_write_until) return "ecriture verrouillee : tapez 'write on' (valable 5 min)";

    if ((c == "set" || c == "sweep") && !ac_has_dp_write())
      return "indisponible en protocole 7A 7A : utilisez 'raw <type> <octets de donnees>' (voir help)";
    if (c == "set") {
      int id = -1;
      std::string ty;
      long n = 0;
      in >> id >> ty >> n;
      int t = type_from_name(ty);
      if (in.fail() || id < 0 || id > 255 || t < 0) return "usage: set <id 0-255> <bool|enum|value> <n>";
      ac_send_dp((uint8_t)id, (uint8_t)t, (int32_t)n);
      return "envoye";
    }
    if (c == "raw") {
      std::vector<uint8_t> bytes;
      std::string tok;
      while (in >> tok) {
        char *end = nullptr;
        long v = strtol(tok.c_str(), &end, 16);
        if (*end || v < 0 || v > 255) return "octet hexadecimal invalide : " + tok;
        bytes.push_back((uint8_t)v);
      }
      if (bytes.empty()) return "usage: raw <cmd hex> [octets hex...]";
      uint8_t cmd = bytes[0];
      bytes.erase(bytes.begin());
      ac_send_raw_cmd(cmd, bytes);
      return "envoye";
    }
    // sweep
    int id = -1, mn = 0, mx = 0, delay = 3;
    in >> id >> mn >> mx;
    if (in.fail() || id < 0 || id > 255 || mn < 0 || mx < mn || mx > 50) return "usage: sweep <id> <min> <max <= 50> [delai_s]";
    if (!(in >> delay)) delay = 3;
    if (delay < 1) delay = 1;
    if (delay > 60) delay = 60;
    if (g_sweep_run) return "un balayage est deja en cours ('stop' pour l'arreter)";
    g_sweep_stop = false;
    SweepArgs *a = new SweepArgs{id, mn, mx, delay};
    if (xTaskCreate(sweep_task, "sweep", 4096, a, 4, nullptr) != pdPASS) {
      delete a;
      return "impossible de demarrer le balayage";
    }
    return "balayage demarre";
  }
  return "commande inconnue. Tapez 'help'.";
}

// ------------------------------------------------------------------ HTTP
static bool auth(httpd_req_t *r) {
  char tok[96] = {0};
  return !g_token.empty() && httpd_req_get_hdr_value_str(r, "X-Token", tok, sizeof(tok)) == ESP_OK &&
         g_token == tok;
}

static esp_err_t poll_get(httpd_req_t *r) {
  if (!auth(r)) {
    httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED, "jeton invalide");
    return ESP_FAIL;
  }
  uint32_t since = 0;
  char q[64];
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
    char v[16];
    if (httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) since = strtoul(v, nullptr, 10);
  }
  AcStatus st = ac_status();
  int64_t now = now_ms();
  int64_t wleft = g_write_until - now;

  std::string j;
  j.reserve(4096);
  char b[256];
  snprintf(b, sizeof(b),
           "{\"status\":{\"mcu\":%d,\"rx\":%u,\"bad\":%u,\"silence\":%.1f,\"write\":%d,\"ac\":\"",
           st.mcu_alive ? 1 : 0, (unsigned)st.rx_frames, (unsigned)st.bad_checksums,
           st.last_rx_ms ? (now - st.last_rx_ms) / 1000.0 : -1.0, wleft > 0 ? (int)(wleft / 1000) : 0);
  j += b;
  j += json_escape(ac_state_text()) + "\",\"acs\":" + ac_state_json() + "},\"dps\":[";
  bool first = true;
  for (auto &d : ac_dp_snapshot()) {
    snprintf(b, sizeof(b), "%s{\"id\":%u,\"type\":\"%s\",\"age\":%.1f,\"v\":\"", first ? "" : ",", d.id,
             type_name(d.type), (now - d.changed_ms) / 1000.0);
    first = false;
    j += b;
    j += json_escape(d.text) + "\"}";
  }
  j += "],\"events\":[";
  uint32_t last;
  {
    std::lock_guard<std::mutex> l(g_mx);
    last = g_seq;
    first = true;
    for (auto &e : g_events) {
      if (e.seq <= since) continue;
      snprintf(b, sizeof(b), "%s{\"s\":%u,\"t\":\"", first ? "" : ",", (unsigned)e.seq);
      first = false;
      j += b;
      j += json_escape(e.ts) + "\",\"k\":\"" + e.kind + "\",\"x\":\"" + json_escape(e.text) + "\"}";
    }
  }
  snprintf(b, sizeof(b), "],\"last\":%u}", (unsigned)last);
  j += b;
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, j.c_str(), j.size());
}

static esp_err_t cmd_post(httpd_req_t *r) {
  if (!auth(r)) {
    httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED, "jeton invalide");
    return ESP_FAIL;
  }
  if (r->content_len == 0 || r->content_len > 300) {
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "commande vide ou trop longue");
    return ESP_FAIL;
  }
  char buf[304];
  size_t got = 0;
  while (got < r->content_len) {
    int n = httpd_req_recv(r, buf + got, r->content_len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) return ESP_FAIL;
    got += n;
  }
  buf[got] = 0;
  std::string out = disc_run(buf);
  httpd_resp_set_type(r, "text/plain; charset=utf-8");
  return httpd_resp_send(r, out.c_str(), out.size());
}

static const char PAGE[] = R"HTML(<!doctype html>
<html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Airton - decouverte</title>
<style>
body{font-family:system-ui,sans-serif;margin:12px;max-width:900px}
h2{margin:18px 0 6px;font-size:1.05em}
table{border-collapse:collapse;width:100%}td,th{border:1px solid #bbb;padding:3px 6px;text-align:left}
#log{height:260px;overflow:auto;border:1px solid #bbb;padding:4px;font:12px monospace;background:#fafafa}
#log div.dp{color:#064}#log div.note{color:#a30;font-weight:bold}#log div.sys{color:#559}
#log div.tx,#log div.rx{color:#777}#log.hideraw div.tx,#log.hideraw div.rx{display:none}
input,select,button{margin:2px;padding:4px}
button.sel{background:#bcd;font-weight:bold;border:2px solid #357}input.n{width:70px}
#st span{margin-right:12px}.ok{color:#060}.ko{color:#a00}
pre{background:#f0f0f0;padding:6px;white-space:pre-wrap}

:root{--bg:#f1f4f8;--card:#fff;--ink:#1c2430;--mut:#6b7686;--line:#dde3ea;--acc:#2f6fed}
body{background:var(--bg);color:var(--ink)}
.card{background:var(--card);border:1px solid var(--line);border-radius:18px;padding:16px 18px;max-width:560px;box-shadow:0 2px 8px rgba(20,30,50,.07);margin:12px 0}
.top{display:flex;justify-content:space-between;align-items:center}
.room{font-size:2.6em;font-weight:650;line-height:1}
.mut{color:var(--mut);font-size:.85em}
.lab{font-size:.75em;text-transform:uppercase;letter-spacing:.06em;color:var(--mut);margin-top:10px}
.pw{width:68px;height:68px;border-radius:50%;border:3px solid #b9c2cf;background:#fff;color:#6b7686;cursor:pointer;margin:0;padding:0;display:inline-flex;align-items:center;justify-content:center}
.pw.on{background:#2fa36b;border-color:#2fa36b;color:#fff}
details>summary{margin:6px 0}
.sp{display:flex;align-items:center;justify-content:center;gap:20px;margin:12px 0 4px}
.sp b{font-size:3em;min-width:2.3em;text-align:center;font-weight:650}
.sp button{width:56px;height:56px;border-radius:50%;font-size:1.9em;line-height:1;border:1px solid var(--line);background:#fff;cursor:pointer;margin:0;padding:0}
.seg{display:flex;flex-wrap:wrap;gap:6px;margin:6px 0 8px}
.seg button{flex:1 1 auto;min-width:62px;padding:11px 6px;border:1px solid var(--line);background:#fff;border-radius:11px;cursor:pointer;margin:0;font-size:.95em}
.seg button.sel{background:var(--acc);border-color:var(--acc);color:#fff;font-weight:600}
.seg button[data-ac="mode=cool"].sel{background:#2b82e6;border-color:#2b82e6}
.seg button[data-ac="mode=heat"].sel{background:#e8742a;border-color:#e8742a}
.seg button[data-ac="mode=dry"].sel{background:#1f9d9d;border-color:#1f9d9d}
.seg button[data-ac="mode=fan_only"].sel{background:#6d7a8c;border-color:#6d7a8c}
.seg button[data-ac="mode=auto"].sel{background:#7a5bd0;border-color:#7a5bd0}
.seg.modes button{display:flex;flex-direction:column;align-items:center;gap:3px;padding:9px 4px}
.card.locked .seg button,.card.locked .pw,.card.locked .sp button{opacity:.45}
.card.nostate .ctl{opacity:.4;pointer-events:none}
.alert{padding:10px 12px;border-radius:11px;margin:10px 0;font-weight:600}
.alert.err{background:#fde8e8;color:#a11;border:1px solid #e6a5a5}
.alert.warn{background:#fff4e0;color:#8a5a00;border:1px solid #e8b04a}
#lock{width:100%;margin:12px 0 4px;padding:11px;border-radius:11px;border:1px solid var(--line);background:#fff;cursor:pointer;font-size:.95em}
#lock.open{background:#fff4e0;border-color:#e8b04a}
#cmsg{min-height:1.2em;font-size:.85em;color:var(--mut);white-space:pre-wrap}
</style></head><body>
<h1>Airton - mode decouverte</h1>
<div>Jeton : <input id="tok" type="password"> <button onclick="saveTok()">OK</button></div>
<div class="card nostate locked" id="card">
 <div class="top">
  <div><div class="room" id="room">--</div><div class="mut">temperature ambiante</div><div class="mut" id="conn"></div></div>
  <div style="text-align:center"><button class="pw" id="pwb" onclick="pw()" title="Marche / arret"><svg viewBox="0 0 24 24" width="32" height="32" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><path d="M12 3v9"/><path d="M6.4 6.6a8 8 0 1 0 11.2 0"/></svg></button><div class="mut" id="ptxt">en attente</div></div>
 </div>
 <div class="alert" id="alert" style="display:none"></div>
 <div class="ctl">
  <div class="lab">Consigne</div>
  <div class="sp"><button onclick="tstep(-1)">&minus;</button><b id="sp">--</b><button onclick="tstep(1)">+</button></div>
  <div class="lab">Mode</div>
  <div class="seg modes">
   <button data-ac="mode=auto"><svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M8.5 16 12 8l3.5 8M9.8 13.5h4.4"/></svg><span>Auto</span></button><button data-ac="mode=cool"><svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 3v18M4.2 7.5l15.6 9M4.2 16.5l15.6-9M9.6 4.6 12 6.8l2.4-2.2M9.6 19.4 12 17.2l2.4 2.2"/></svg><span>Froid</span></button>
   <button data-ac="mode=dry"><svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 3.5S6 10 6 14a6 6 0 0 0 12 0c0-4-6-10.5-6-10.5z"/></svg><span>Deshum.</span></button><button data-ac="mode=fan_only"><svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="1.7"/><g id="bl"><path d="M12 10.3C11.6 6.5 13.4 4 16 4.6c.6 2.6-1.2 4.9-3.4 5.8"/></g><use href="#bl" transform="rotate(120 12 12)"/><use href="#bl" transform="rotate(240 12 12)"/></svg><span>Ventil.</span></button>
   <button data-ac="mode=heat"><svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 3c1 3.5 5 5.5 5 10a5 5 0 0 1-10 0c0-2 1-3.400 2.200-4.600.2 1.400.8 2 1.600 2.400C10.400 7.600 10.800 5.400 12 3z"/></svg><span>Chaud</span></button>
  </div>
  <div class="lab">Ventilation</div>
  <div class="seg">
   <button data-ac="fan=auto">Auto</button><button data-ac="fan=low">1</button><button data-ac="fan=low_medium">2</button>
   <button data-ac="fan=medium">3</button><button data-ac="fan=medium_high">4</button><button data-ac="fan=high">5</button>
   <button data-ac="fan=turbo">Turbo</button><button data-ac="fan=stepless" title="vitesse en pourcentage reglee a la telecommande">%</button>
   <button data-ac="fan=silent">Silence</button>
  </div>
  <div class="lab">Volets</div>
  <div class="seg">
   <button data-ac="swing=off">Fixes</button><button data-ac="swing=vertical">Vertical</button>
   <button data-ac="swing=horizontal">Lateral</button><button data-ac="swing=both">Les deux</button>
  </div>
  <div class="lab">Afficheur</div>
  <div class="seg">
   <button data-ac="display=on">Allume</button><button data-ac="display=off">Eteint</button>
  </div>
 </div>
 <button id="lock" onclick="run(locked?'write on':'write off')">Deverrouiller les commandes (5 min)</button>
 <div id="cmsg"></div>
 <div class="mut" id="ref"></div>
 <div class="mut" id="acst"></div>
</div>

<h2>Mise a jour du firmware</h2>
<input type="file" id="fw" accept=".bin">
<button onclick="upload()">Envoyer</button>
<progress id="fwp" value="0" max="100" style="width:160px"></progress> <span id="fwt"></span><br>
<small>Fichier build/airton_ac.bin (1,5 Mo maximum). Le module redemarre apres l'envoi ; si le nouveau firmware ne demarre pas, l'ancien revient tout seul.</small>

<details id="adv" style="margin-top:22px">
<summary style="cursor:pointer;font-weight:bold">Analyse des trames (avance) - afficher / masquer</summary>
<div id="st">...</div>
<h2>Journal</h2>
<label><input type="checkbox" id="fraw" onchange="rawToggle()"> afficher les trames brutes</label>
<button onclick="exportCsv()">Exporter CSV</button>
<button onclick="run('clear');evs.length=0;$('log').textContent=''">Effacer</button>
<div id="log" class="hideraw"></div>

<h2>Reperes et lecture</h2>
<input id="note" placeholder="ex: eco ON" size="24"><button onclick="run('note '+$('note').value);$('note').value=''">Ajouter un repere</button><br>
<button onclick="run('query')">Interroger tous les etats</button>
<button onclick="run('info')">Infos produit</button>
<button onclick="run('dps')">Lister les DP</button>

<h2>Datapoints (etat courant)</h2>
<table><thead><tr><th>DP</th><th>Type</th><th>Valeur</th><th>Depuis (s)</th></tr></thead><tbody id="dps"></tbody></table>

<h2>Ecriture (modifie la clim)</h2>
<button onclick="run('write on')">Autoriser 5 min</button>
<button onclick="run('write off')">Verrouiller</button><br>
set DP <input class="n" id="sid" type="number" min="0" max="255">
<select id="sty"><option>bool</option><option>enum</option><option>value</option></select>
<input class="n" id="sval" type="number"> <button onclick="run('set '+$('sid').value+' '+$('sty').value+' '+$('sval').value)">Envoyer</button><br>
balayage enum DP <input class="n" id="wid" type="number"> de <input class="n" id="wmin" type="number" value="0">
a <input class="n" id="wmax" type="number" value="4"> delai <input class="n" id="wdel" type="number" value="3">s
<button onclick="run('sweep '+$('wid').value+' '+$('wmin').value+' '+$('wmax').value+' '+$('wdel').value)">Lancer</button>
<button onclick="run('stop')">Stop</button><br>
commande libre <input id="free" size="30" placeholder="help"><button onclick="run($('free').value)">Executer</button>
<pre id="out">Tapez help pour la liste des commandes.</pre>

</details>

<script>
const $=id=>document.getElementById(id);
let tok='',last=0;const evs=[];
try{tok=localStorage.getItem('tok')||''}catch(e){}
$('tok').value=tok;
function saveTok(){tok=$('tok').value;try{localStorage.setItem('tok',tok)}catch(e){}}
function rawToggle(){$('log').classList.toggle('hideraw',!$('fraw').checked)}
async function api(p,o){o=o||{};o.headers={'X-Token':tok};const r=await fetch(p,o);if(!r.ok)throw new Error('HTTP '+r.status);return r}
async function run(line){try{const r=await api('/api/cmd',{method:'POST',body:line});const t=await r.text();$('out').textContent=t;$('cmsg').textContent=t.split('\n')[0]}catch(e){$('out').textContent='Erreur : '+e.message;$('cmsg').textContent='Erreur : '+e.message}}
function addEv(e){evs.push(e);if(evs.length>1500)evs.shift();
 const d=document.createElement('div');d.className=e.k;d.textContent=e.t+'  '+e.x;
 const L=$('log'),bottom=L.scrollTop+L.clientHeight>=L.scrollHeight-8;L.appendChild(d);
 while(L.childNodes.length>1500)L.removeChild(L.firstChild);if(bottom)L.scrollTop=L.scrollHeight}
let AC=null,locked=true,pend=null,pt=null;
function cmd(a){
 if(locked){$('cmsg').textContent='Commandes verrouillees : appuyez sur Deverrouiller';return}
 run('ac '+a)}
function pw(){cmd('power='+(AC&&AC.power==='on'?'off':'on'))}
function tstep(d){
 if(!AC)return;let v=(pend===null?AC.temp:pend)+d;v=Math.max(16,Math.min(31,v));pend=v;$('sp').textContent=v;
 clearTimeout(pt);pt=setTimeout(()=>{cmd('temp='+v);setTimeout(()=>{pend=null},4000)},900)}
document.addEventListener('click',e=>{const b=e.target.closest('button[data-ac]');if(!b)return;
 const a=b.dataset.ac;cmd(a.startsWith('mode=')?'power=on '+a:a)});
function syncAc(a){
 AC=a;const c=$('card');c.classList.toggle('nostate',!a);
 document.querySelectorAll('button[data-ac]').forEach(b=>{
  const kv=b.dataset.ac.split('=');b.classList.toggle('sel',!!a&&String(a[kv[0]])===kv[1])});
 $('room').textContent=a?a.room.toFixed(1)+' \u00b0C':'--';
 $('sp').textContent=pend!==null?pend:(a?a.temp:'--');
 $('pwb').classList.toggle('on',!!a&&a.power==='on');
$('ptxt').textContent=a?(a.power==='on'?'en marche':'arretee'):'en attente de la clim';
 const al=$('alert'),p=[];
 if(a&&a.err>0)p.push("Code d'erreur "+a.err+(a.err===4?' (mode non supporte, selon le forum)':''));
 if(a&&a.filter===1)p.push('Nettoyer le filtre');
 al.style.display=p.length?'block':'none';al.textContent=p.join(' - ');al.className='alert '+(a&&a.err>0?'err':'warn');
 $('ref').textContent=a&&a.ref?'Reference interne : '+a.ref:''}
function syncLock(w){
 locked=!(w>0);const c=$('card'),l=$('lock');c.classList.toggle('locked',locked);l.classList.toggle('open',!locked);
 l.textContent=locked?'Deverrouiller les commandes (5 min)':'Commandes deverrouillees - encore '+Math.floor(w/60)+':'+String(w%60).padStart(2,'0')+' (verrouiller)'}
function render(d){const s=d.status;
 $('acst').textContent=s.ac||'';
 $('conn').textContent=s.mcu?'clim connectee':'clim sans reponse';
 syncAc(s.acs);
 syncLock(s.write);
 $('st').innerHTML='<span class="'+(s.mcu?'ok':'ko')+'">MCU : '+(s.mcu?'detecte':'absent')+'</span><span>trames : '+s.rx+'</span><span>checksums faux : '+s.bad+'</span><span>silence : '+s.silence+' s</span><span>ecriture : '+(s.write>0?s.write+' s':'verrouillee')+'</span>';
 const tb=$('dps');tb.textContent='';
 d.dps.forEach(p=>{const tr=tb.insertRow();[p.id,p.type,p.v,p.age].forEach(v=>{tr.insertCell().textContent=v})})}
let busy=false;
function upload(){
 const f=$('fw').files[0],t=$('fwt');
 if(!f){t.textContent='choisissez un fichier .bin';return}
 if(f.size>1572864){t.textContent='fichier trop gros ('+f.size+' octets, maximum 1572864)';return}
 if(!confirm('Envoyer '+f.name+' ('+Math.round(f.size/1024)+' Ko) et redemarrer le module ?'))return;
 busy=true;let full=false;
 const x=new XMLHttpRequest();x.open('POST','/update');x.setRequestHeader('X-Token',tok);
 x.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(100*e.loaded/e.total);$('fwp').value=p;t.textContent=p+' %';if(p>=100)full=true}};
 const done=m=>{t.textContent=m;setTimeout(()=>{busy=false},12000)};
 x.onload=()=>{if(x.status==200)done('envoye, le module redemarre...');else{busy=false;t.textContent='Erreur '+x.status+' : '+x.responseText}};
 x.onerror=()=>{if(full)done('envoi termine, connexion coupee par le redemarrage du module');else{busy=false;t.textContent='connexion perdue pendant l\'envoi'}};
 t.textContent='envoi...';x.send(f);
}
async function poll(){
 if(busy){setTimeout(poll,1000);return}
 try{const r=await api('/api/poll?since='+last);const d=await r.json();
  if(d.last<last){evs.length=0;$('log').textContent='';last=0}
  render(d);d.events.forEach(addEv);last=d.last}
 catch(e){$('st').textContent='Hors ligne ou jeton invalide ('+e.message+')'}
 setTimeout(poll,1000)}
function q(s){return '"'+String(s).replace(/"/g,'""')+'"'}
function exportCsv(){
 const rows=['heure,type,texte'];evs.filter(e=>e.k=='dp'||e.k=='note').forEach(e=>rows.push([e.t,e.k,e.x].map(q).join(',')));
 const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([rows.join('\n')],{type:'text/csv'}));
 a.download='airton-decouverte.csv';a.click()}
poll();
</script></body></html>)HTML";

static esp_err_t index_get(httpd_req_t *r) {
  httpd_resp_set_type(r, "text/html; charset=utf-8");
  return httpd_resp_send(r, PAGE, HTTPD_RESP_USE_STRLEN);
}

void disc_init(httpd_handle_t server, const char *token) {
  g_token = token ? token : "";
  httpd_uri_t idx = {};
  idx.uri = "/";
  idx.method = HTTP_GET;
  idx.handler = index_get;
  httpd_register_uri_handler(server, &idx);
  httpd_uri_t poll = {};
  poll.uri = "/api/poll";
  poll.method = HTTP_GET;
  poll.handler = poll_get;
  httpd_register_uri_handler(server, &poll);
  httpd_uri_t cmd = {};
  cmd.uri = "/api/cmd";
  cmd.method = HTTP_POST;
  cmd.handler = cmd_post;
  httpd_register_uri_handler(server, &cmd);
  ESP_LOGI(TAG, "page de decouverte disponible sur /");
}
