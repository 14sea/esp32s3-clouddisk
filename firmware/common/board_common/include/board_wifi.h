#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    wifi_auth_mode_t authmode;
    bool is_target;      /* 是否是 Wifi.txt 裡配置的那個網絡 */
} board_ap_info_t;

/*
 * 掃描周邊 AP。可以在 connect 之前單獨調用——重連循環會打斷掃描，
 * 所以診斷時務必先掃再連。found 返回掃到的總數（可能大於 max_out）。
 */
esp_err_t board_wifi_scan(board_ap_info_t *out, int max_out, int *found);

/*
 * 設置主機名，必須在 board_wifi_connect() 之前調用。
 * 作用有兩處：
 *   - DHCP 主機名，路由器後台的設備列表裡會顯示它
 *   - mDNS 名字，board_mdns_start() 用同一個
 */
void board_wifi_set_hostname(const char *name);

/*
 * 按 "<prefix>-<MAC末兩字節>" 自動生成主機名，例如 esp32cam-a1b2。
 * 同型號多塊板子同時上線不會撞名，也不用每塊手工改代碼。
 * 同樣必須在 connect/scan 之前調用。
 */
void board_wifi_set_hostname_auto(const char *prefix);

/*
 * 限制最大發射功率，單位 0.25dBm（如 40 = 10dBm），0 表示用默認的 20dBm。
 * 必須在 connect 之前調用。用途：供電偏弱時滿功率發射會失敗，
 * 降功率若能讓鏈路變穩，就說明是供電問題而不是天線問題。
 */
void board_wifi_set_tx_power(int8_t quarter_dbm);

/*
 * 覆蓋 STA 的 MAC 地址，必須在 connect/scan 之前調用。
 * 只改運行時，不燒 efuse，斷電即恢復。首字節的最低位必須為 0（單播地址）。
 * 用途：診斷路由器是不是按 MAC 攔截了設備。
 */
void board_wifi_set_mac(const uint8_t mac[6]);

/*
 * 改連 WPA2-Enterprise 網絡（如 eduroam，PEAP/TTLS + MSCHAPv2，不驗證 CA），
 * 取代 wifi_secrets.h 裡的家用網絡。必須在 connect/scan 之前調用。
 */
void board_wifi_set_enterprise(const char *ssid, const char *identity,
                               const char *username, const char *password);

/*
 * 連上 Wifi.txt 裡配置的無線網絡，阻塞到拿到 IP 或超時。
 * 內部會初始化 NVS / netif / event loop，重複調用安全。
 * 斷線後自動重連。
 */
esp_err_t board_wifi_connect(int timeout_ms);

/*
 * 起 mDNS，之後可以用 <主機名>.local 訪問，不用記 IP。
 * 要在拿到 IP 之後調用。instance 是設備在網絡鄰居裡顯示的友好名稱。
 * http_port > 0 時順帶註冊 _http._tcp 服務，方便被發現。
 */
esp_err_t board_mdns_start(const char *instance, uint16_t http_port);

typedef struct {
    bool     connected;
    int8_t   rssi;          /* 當前信號強度 dBm，未連上為 0 */
    uint32_t uptime_s;      /* 本次連接已保持的秒數 */
    uint32_t disconnects;   /* 開機以來累計掉線次數 */
} board_wifi_stats_t;

/* 連接健康度，用來判斷鏈路穩不穩 */
void board_wifi_stats(board_wifi_stats_t *out);

/* 取當前 IP，形如 "192.168.1.23"；未連上時返回 "0.0.0.0" */
const char *board_wifi_ip(void);

/* 當前主機名，形如 "esp32cam01" */
const char *board_wifi_hostname(void);

/* SSID 的遮蔽形式，用於日誌，避免憑據出現在串口輸出裡 */
const char *board_wifi_ssid_masked(void);
