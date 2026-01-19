idf_component_register(SRCS "main.c" "wifi.c" "rtp/rtp.c"
                    INCLUDE_DIRS "."
                    INCLUDE_DIRS "rtp"
                    PRIV_REQUIRES nvs_flash esp_psram esp_event esp_netif esp_wifi esp_timer)
