#include "wifi.h"

static const char* TAG = "WIFI";
static TaskHandle_t xTaskToNotify = NULL;
static esp_netif_t* s_sta_netif = NULL;

#define CLOSER_IMPLEMENTATION
#include "closer.h"
static closer_handle_t s_closer = NULL;
#define DEFER(fn) CLOSER_DEFER(s_closer, (void*)fn)

static esp_err_t delete_default_wifi_driver_and_handlers() {
    if (unlikely(s_sta_netif == NULL)) {
        return ESP_OK;
    }

    return esp_wifi_clear_default_wifi_driver_and_handlers(s_sta_netif);
}

static void sta_netif_destroy() {
    if (unlikely(s_sta_netif == NULL)) {
        return;
    }

    esp_netif_destroy(s_sta_netif);
    s_sta_netif = NULL;
}

static esp_err_t wifi_init() {
    ESP_LOGI(TAG, "wifi_init");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    DEFER(esp_netif_deinit);

    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "esp_event_loop_create_default");
    DEFER(esp_event_loop_delete_default);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");
    DEFER(esp_wifi_deinit);

    esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    s_sta_netif = esp_netif_create_wifi(WIFI_IF_STA, &esp_netif_config);

    if (unlikely(s_sta_netif == NULL)) {
        ESP_LOGE(TAG, "esp_netif_create_wifi");
        return ESP_FAIL;
    }
    DEFER(sta_netif_destroy);

    ESP_RETURN_ON_ERROR(esp_wifi_set_default_wifi_sta_handlers(), TAG, "esp_wifi_set_default_wifi_sta_handlers");
    DEFER(delete_default_wifi_driver_and_handlers);

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode");

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
    DEFER(esp_wifi_stop);

    int8_t pwr;
    ESP_RETURN_ON_ERROR(esp_wifi_get_max_tx_power(&pwr), TAG, "esp_wifi_get_max_tx_power");
    ESP_LOGI(TAG, "WiFi TX power = %.2f dBm, pwr=%d", pwr * 0.25, pwr);

    return ESP_OK;
}

static void handler_on_sta_got_ip(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
    if (event->esp_netif != s_sta_netif) {
        ESP_LOGW(TAG, "Got IP event for unknown netif");
        return;
    }

    ESP_LOGI(TAG, "Got IPv4 event, address: " IPSTR, IP2STR(&event->ip_info.ip));

    TaskHandle_t to_notify = __atomic_load_n(&xTaskToNotify, __ATOMIC_SEQ_CST);
    if (to_notify) {
        if (xPortInIsrContext()) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xTaskNotifyFromISR(to_notify, 0, eSetValueWithOverwrite, &xHigherPriorityTaskWoken);
            if (xHigherPriorityTaskWoken) {
                portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
            }
        } else {
            xTaskNotify(to_notify, 0, eSetValueWithOverwrite);
        }
    }
}

esp_err_t wifi_connect() {
    ESP_ERROR_CHECK(closer_create(&s_closer));
    ESP_RETURN_ON_ERROR(wifi_init(), TAG, "wifi_init");
    wifi_config_t wifi_config = {
        .sta =
            {
                .ssid = CONFIG_ESPRTP_WIFI_SSID,
                .password = CONFIG_ESPRTP_WIFI_PASSWORD,
            },
    };

    ESP_LOGI(TAG, "Connecting to %s...", wifi_config.sta.ssid);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "esp_wifi_set_config");

    __atomic_store_n(&xTaskToNotify, xTaskGetCurrentTaskHandle(), __ATOMIC_SEQ_CST);

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &handler_on_sta_got_ip, NULL), TAG,
                        "esp_event_handler_register");

    esp_err_t err = esp_wifi_connect();

    if (err != ESP_OK) {
        goto cleanup;
    }

    ESP_LOGI(TAG, "Waiting for IP address...");

    if (xTaskNotifyWait(pdFALSE, ULONG_MAX, NULL, WAIT_STA_GOT_IP_MAX) != pdPASS) {
        err = ESP_ERR_TIMEOUT;
        ESP_LOGW(TAG, "No ip received within the timeout period");

        goto cleanup;
    }

    err = ESP_OK;

cleanup:

    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &handler_on_sta_got_ip); // TODO: спорно
    __atomic_store_n(&xTaskToNotify, NULL, __ATOMIC_SEQ_CST);

    if (err != ESP_OK) {
        closer_close(s_closer);
    }

    closer_destroy(s_closer);
    s_closer = NULL;

    return err;
}