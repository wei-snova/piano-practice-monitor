/*
 * net.h - optional Wi-Fi: sets the clock via SNTP and serves the session log.
 *
 * If CONFIG_PL_WIFI_SSID is empty, Wi-Fi stays off and the logger still works,
 * but without a clock it cannot split totals into calendar days.
 *
 * When connected:  http://<board-ip>/            -> small summary page
 *                  http://<board-ip>/sessions.csv -> every logged session
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

void net_start(void);
bool net_connected(void);
/* "192.168.1.23" or "" */
const char *net_ip(void);
const char *net_ssid(void);
/* Saves credentials in NVS and connects/reconnects the station. */
esp_err_t net_set_wifi_credentials(const char *ssid, const char *password);
