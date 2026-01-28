idf_component_register(SRCS "pdm_mic.c" "main.c" "wifi/wifi.c" "rtp/rtp.c" "rtp/jpeg.c" "pdm_mic.c"
                    INCLUDE_DIRS "."
                    INCLUDE_DIRS "rtp"
                    INCLUDE_DIRS "wifi"
                    PRIV_REQUIRES nvs_flash esp_psram esp_event esp_netif esp_wifi esp_timer esp_driver_i2s)
