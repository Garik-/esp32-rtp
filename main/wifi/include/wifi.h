#pragma once

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WAIT_STA_GOT_IP_MAX pdMS_TO_TICKS(10000) // TODO: make configurable

esp_err_t wifi_connect();
uint32_t wifi_get_broadcast_addr();

#ifdef __cplusplus
}
#endif
