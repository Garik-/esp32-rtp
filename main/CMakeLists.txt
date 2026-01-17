idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES nvs_flash esp_psram esp_event esp_netif esp_wifi)
