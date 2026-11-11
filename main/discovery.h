#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "esp_http_server.h"

// Enregistre la page web et l'API de découverte sur le serveur HTTP existant.
void disc_init(httpd_handle_t server, const char *token);

// Appelés par main.cpp
void disc_on_dp(uint8_t id, uint8_t type, bool had_before, const std::string &old_text,
                const std::string &new_text);
void disc_on_frame(bool tx, const std::vector<uint8_t> &frame);
void disc_sys(const std::string &msg);

// Interpréteur de commandes texte (utilisé par l'API POST /api/cmd).
std::string disc_run(const std::string &line);
