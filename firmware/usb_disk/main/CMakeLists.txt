idf_component_register(SRCS "main.c" "usb_msc.c" "blockdev_sd.c" "blockdev_ram.c"
                            "blockdev_http.c" "remote_log.c"
                       INCLUDE_DIRS "."
                       REQUIRES board_common
                                usb
                                esp_driver_sdmmc
                                sdmmc
                                esp_timer
                                esp_psram
                                esp_http_client
                                mbedtls)

# 把本目錄的 tusb_config.h 交給 TinyUSB 組件
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
