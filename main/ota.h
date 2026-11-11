#pragma once
#include "esp_http_server.h"

// Démarre un petit serveur HTTP : POST /update (binaire du firmware), en-tête X-Token requis.
// Retourne le serveur (réutilisé par la page de découverte), ou nullptr en cas d'échec.
httpd_handle_t ota_start(const char *token);
// À appeler quand le nouveau firmware est jugé sain (annule le retour arrière automatique).
void ota_mark_valid();
