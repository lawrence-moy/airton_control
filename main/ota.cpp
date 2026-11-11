#include "ota.h"

#include <cstring>
#include <string>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";
static std::string g_token;

static esp_err_t update_post(httpd_req_t *req) {
  char tok[96] = {0};
  if (g_token.empty() || httpd_req_get_hdr_value_str(req, "X-Token", tok, sizeof(tok)) != ESP_OK ||
      g_token != tok) {
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "jeton invalide");
    return ESP_FAIL;
  }
  const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
  if (!part) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "pas de partition OTA");
    return ESP_FAIL;
  }
  esp_ota_handle_t handle = 0;
  if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin a echoue");
    return ESP_FAIL;
  }
  char buf[1024];
  int remaining = req->content_len;
  while (remaining > 0) {
    int want = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
    int r = httpd_req_recv(req, buf, want);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0 || esp_ota_write(handle, buf, r) != ESP_OK) {
      esp_ota_abort(handle);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reception/ecriture echouee");
      return ESP_FAIL;
    }
    remaining -= r;
  }
  if (esp_ota_end(handle) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image invalide");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "OK, redemarrage\n");
  ESP_LOGI(TAG, "mise a jour recue, redemarrage");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}

httpd_handle_t ota_start(const char *token) {
  g_token = token ? token : "";
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 8192;
  cfg.max_open_sockets = 4;
  cfg.lru_purge_enable = true;
  httpd_handle_t srv = nullptr;
  if (httpd_start(&srv, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "serveur HTTP non demarre");
    return nullptr;
  }
  httpd_uri_t uri = {};
  uri.uri = "/update";
  uri.method = HTTP_POST;
  uri.handler = update_post;
  httpd_register_uri_handler(srv, &uri);
  return srv;
}

void ota_mark_valid() {
  esp_ota_mark_app_valid_cancel_rollback();
}
