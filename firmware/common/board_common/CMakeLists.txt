idf_component_register(SRCS "board_wifi.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi wpa_supplicant esp_netif nvs_flash esp_event esp_timer esp_hw_support)
