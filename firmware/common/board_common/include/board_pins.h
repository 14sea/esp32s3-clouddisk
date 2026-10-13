/*
 * Freenove ESP32-S3-WROOM CAM 板載外設引腳
 * 來源：廠商資料 camera_pins.h 中的 CAMERA_MODEL_ESP32S3_EYE 一組
 */
#pragma once

/* --- OV2640/OV5640 攝像頭 (DVP 8-bit) --- */
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD     4      /* SCCB SDA */
#define CAM_PIN_SIOC     5      /* SCCB SCL */
#define CAM_PIN_D0      11
#define CAM_PIN_D1       9
#define CAM_PIN_D2       8
#define CAM_PIN_D3      10
#define CAM_PIN_D4      12
#define CAM_PIN_D5      18
#define CAM_PIN_D6      17
#define CAM_PIN_D7      16
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13

/* --- 板載 WS2812 RGB LED --- */
#define WS2812_PIN      48

/* --- microSD (SDMMC, 1-bit) --- */
#define SD_PIN_CLK      39
#define SD_PIN_CMD      38
#define SD_PIN_D0       40
