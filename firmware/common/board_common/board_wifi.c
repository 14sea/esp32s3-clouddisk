/*
 * WiFi station 連接，供各個示例工程共用。
 * 憑據來自 wifi_secrets.h，由 scripts/gen_wifi_secrets.py 從 Wifi.txt 生成。
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "esp_timer.h"
#include "esp_mac.h"

#include "esp_eap_client.h"

#include "board_wifi.h"
#include "wifi_secrets.h"

static const char *TAG = "wifi";

#define BIT_GOT_IP   BIT0
#define BIT_FAILED   BIT1

static EventGroupHandle_t s_events;
static char s_ip[16] = "0.0.0.0";
static char s_hostname[33] = "esp32cam";
static int s_retries;
static bool s_started;       /* 底層 WiFi 是否已初始化並啟動 */
static bool s_want_connect;  /* 是否要自動連接/重連 */
static bool s_mdns_up;
static uint8_t s_mac_override[6];
static bool s_mac_set;
static int8_t s_tx_power_qdbm;   /* 單位 0.25dBm，0=不限制 */
static uint32_t s_disconnects;      /* 累計掉線次數 */
static int64_t s_connected_since;   /* 最近一次拿到 IP 的時間 */

/* WPA2-Enterprise（如 eduroam）：設了就取代 wifi_secrets.h 裡的 WPA2-Personal 網絡 */
static struct {
    bool on;
    char ssid[33];
    char identity[64];
    char username[64];
    char password[64];
} s_eap;

static const char *active_ssid(void)
{
    return s_eap.on ? s_eap.ssid : WIFI_SSID;
}

/* 快速重試多少次之後，board_wifi_connect 才報失敗返回 */
#define MAX_FAST_RETRY 6
/* 退避上限。固定短間隔猛重連會讓 AP 把設備當成異常客戶端，
 * 之前就是這樣把路由器逼到拒絕關聯的。 */
#define RETRY_BASE_MS  1000
#define RETRY_MAX_MS   30000

static esp_timer_handle_t s_retry_timer;

static const char *reason_str(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_AUTH_EXPIRE:            return "認證超時";
    case WIFI_REASON_AUTH_LEAVE:             return "AP 主動斷開";
    case WIFI_REASON_ASSOC_EXPIRE:           return "關聯過期";
    case WIFI_REASON_ASSOC_TOOMANY:          return "AP 客戶端已滿";
    case WIFI_REASON_NOT_AUTHED:             return "未認證";
    case WIFI_REASON_NOT_ASSOCED:            return "未關聯";
    case WIFI_REASON_BEACON_TIMEOUT:         return "信標超時(信號差/AP 消失)";
    case WIFI_REASON_NO_AP_FOUND:            return "找不到 AP";
    case WIFI_REASON_AUTH_FAIL:              return "認證失敗";
    case WIFI_REASON_ASSOC_FAIL:             return "關聯失敗";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "四次握手超時(多半是密碼錯)";
    case WIFI_REASON_CONNECTION_FAIL:        return "連接失敗";
    case WIFI_REASON_ROAMING:                return "漫遊切換";
    default:                                 return "其他";
    }
}

static void retry_cb(void *arg)
{
    if (s_want_connect) {
        esp_wifi_connect();
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        if (s_want_connect) {
            esp_wifi_connect();
        }
        return;
    }

    if (id != WIFI_EVENT_STA_DISCONNECTED || !s_want_connect) {
        return;      /* 掃描期間的斷開事件別觸發重連，會把掃描打斷 */
    }

    const wifi_event_sta_disconnected_t *d = data;
    strcpy(s_ip, "0.0.0.0");
    s_retries++;
    s_disconnects++;

    /* 指數退避，上限 RETRY_MAX_MS */
    int shift = s_retries - 1;
    if (shift > 5) {
        shift = 5;
    }
    int delay_ms = RETRY_BASE_MS << shift;
    if (delay_ms > RETRY_MAX_MS) {
        delay_ms = RETRY_MAX_MS;
    }

    if (s_retries <= MAX_FAST_RETRY) {
        ESP_LOGW(TAG, "斷開 (reason=%d %s)，%d ms 後第 %d 次重連",
                 d->reason, reason_str(d->reason), delay_ms, s_retries);
    } else if (s_retries == MAX_FAST_RETRY + 1) {
        ESP_LOGE(TAG, "斷開 (reason=%d %s)，已重試 %d 次，轉入 %d 秒間隔的後台重連",
                 d->reason, reason_str(d->reason), MAX_FAST_RETRY, RETRY_MAX_MS / 1000);
        xEventGroupSetBits(s_events, BIT_FAILED);
    }

    /* 絕不能在事件處理函數裡 vTaskDelay：那會把整個默認事件循環任務卡住，
     * 期間 GOT_IP 之類的事件都處理不了。用一次性定時器排程。 */
    esp_timer_stop(s_retry_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000));
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *e = data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "已連上，IP %s", s_ip);
    s_retries = 0;
    s_connected_since = esp_timer_get_time();
    xEventGroupClearBits(s_events, BIT_FAILED);
    xEventGroupSetBits(s_events, BIT_GOT_IP);
}

/* 底層初始化並啟動 WiFi，但不發起連接。掃描和連接都先走這裡。 */
static esp_err_t wifi_low_init(void)
{
    if (!s_started) {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        ESP_ERROR_CHECK(err);

        s_events = xEventGroupCreate();

        const esp_timer_create_args_t targs = {
            .callback = retry_cb,
            .name = "wifi_retry",
        };
        ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_t *sta = esp_netif_create_default_wifi_sta();
        /* 必須在 esp_wifi_start() 之前設，DHCP 請求才會帶上它 */
        esp_netif_set_hostname(sta, s_hostname);

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        /* 診斷用：換 MAC 可以區分「這個 MAC 被路由器攔了」和「AP 拒絕所有人」。
         * 只改運行時，不動 efuse，斷電即恢復出廠 MAC。 */
        if (s_mac_set) {
            ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, s_mac_override));
            ESP_LOGW(TAG, "已覆蓋 STA MAC 為 %02x:%02x:%02x:%02x:%02x:%02x",
                     s_mac_override[0], s_mac_override[1], s_mac_override[2],
                     s_mac_override[3], s_mac_override[4], s_mac_override[5]);
        }

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL));

        wifi_config_t wc = {
            .sta = {
                .ssid = WIFI_SSID,
                .password = WIFI_PASSWORD,
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,

                /* 用指定初始化器時未列出的字段會歸零，pmf_cfg.capable 也會變成 false。
                 * 遇到啟用了 PMF(802.11w) required 的 AP，會在關聯階段直接被拒。
                 * capable=true 對不用 PMF 的 AP 無副作用，應當作默認值。 */
                .pmf_cfg = {
                    .capable = true,
                    .required = false,
                },

                /* 同一 SSID 有多個 BSSID（Mesh/擴展器）時，掃全信道並挑信號最好的，
                 * 默認的 FAST_SCAN 會連上第一個掃到的，未必可用 */
                .scan_method = WIFI_ALL_CHANNEL_SCAN,
                .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            },
        };
        if (s_eap.on) {
            memset(wc.sta.password, 0, sizeof(wc.sta.password));
            /* sta.ssid 是 32 字節、不要求 \0 結尾，SSID 最長正好 32。
             * 必須先清零：wc 是按家用網絡初始化的，只拷企業網的短 SSID 會留下
             * 家用 SSID 的尾巴，拼成一個不存在的名字（實測 reason=201 找不到 AP） */
            memset(wc.sta.ssid, 0, sizeof(wc.sta.ssid));
            memcpy(wc.sta.ssid, s_eap.ssid, strnlen(s_eap.ssid, sizeof(wc.sta.ssid)));
            wc.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
        }
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        if (s_eap.on) {
            /* PEAP/TTLS + MSCHAPv2，eduroam 等校園網常見的配置。
             * 沒裝 CA 證書：不驗證 RADIUS 服務器身份，換來不用維護學校的根證書 */
            esp_eap_client_set_identity((uint8_t *)s_eap.identity, strlen(s_eap.identity));
            esp_eap_client_set_username((uint8_t *)s_eap.username, strlen(s_eap.username));
            esp_eap_client_set_password((uint8_t *)s_eap.password, strlen(s_eap.password));
            esp_eap_client_set_ttls_phase2_method(ESP_EAP_TTLS_PHASE2_MSCHAPV2);
            ESP_ERROR_CHECK(esp_wifi_sta_enterprise_enable());
        }
        ESP_ERROR_CHECK(esp_wifi_start());

        /* 默認的 modem sleep 會讓 RTT 從幾毫秒漲到近百毫秒，推流必須關掉 */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

        /* 供電偏弱時，滿功率發射的電流突發會讓發射失敗（表現為收得到、發不出）。
         * 降功率能區分「供電不足」和「天線/射頻硬件問題」。0 表示用默認的 20dBm。 */
        if (s_tx_power_qdbm > 0) {
            ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(s_tx_power_qdbm));
            int8_t got = 0;
            esp_wifi_get_max_tx_power(&got);
            ESP_LOGW(TAG, "發射功率限制為 %.2f dBm", got / 4.0f);
        }
        s_started = true;

    }
    return ESP_OK;
}

esp_err_t board_wifi_scan(board_ap_info_t *out, int max_out, int *found)
{
    ESP_ERROR_CHECK(wifi_low_init());

    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "掃描失敗: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    *found = n;
    if (n == 0) {
        return ESP_OK;
    }

    uint16_t want = n > max_out ? max_out : n;
    wifi_ap_record_t *recs = calloc(want, sizeof(wifi_ap_record_t));
    if (!recs) {
        return ESP_ERR_NO_MEM;
    }
    esp_wifi_scan_get_ap_records(&want, recs);

    for (int i = 0; i < want; i++) {
        snprintf(out[i].ssid, sizeof(out[i].ssid), "%s", (char *)recs[i].ssid);
        out[i].rssi = recs[i].rssi;
        out[i].channel = recs[i].primary;
        out[i].authmode = recs[i].authmode;
        out[i].is_target = strcmp((char *)recs[i].ssid, active_ssid()) == 0;
    }
    free(recs);
    return ESP_OK;
}

esp_err_t board_wifi_connect(int timeout_ms)
{
    ESP_ERROR_CHECK(wifi_low_init());

    if (!s_want_connect) {
        s_want_connect = true;
        s_retries = 0;
        ESP_LOGI(TAG, "連接 SSID %s ...", board_wifi_ssid_masked());
        esp_wifi_connect();
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_FAILED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_GOT_IP) {
        return ESP_OK;
    }
    return (bits & BIT_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

void board_wifi_set_mac(const uint8_t mac[6])
{
    if (s_started) {
        ESP_LOGW(TAG, "WiFi 已啟動，MAC 覆蓋被忽略");
        return;
    }
    memcpy(s_mac_override, mac, 6);
    s_mac_set = true;
}

void board_wifi_set_tx_power(int8_t quarter_dbm)
{
    s_tx_power_qdbm = quarter_dbm;
}

void board_wifi_set_hostname_auto(const char *prefix)
{
    uint8_t mac[6];
    if (s_mac_set) {
        memcpy(mac, s_mac_override, 6);
    } else {
        /* esp_read_mac 直接讀 efuse，不需要先起 WiFi */
        ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    }
    /* 取 MAC 末兩字節做後綴，同型號多塊板子同時上線也不會撞名 */
    char name[33];
    snprintf(name, sizeof(name), "%s-%02x%02x", prefix, mac[4], mac[5]);
    board_wifi_set_hostname(name);
}

void board_wifi_set_hostname(const char *name)
{
    if (s_started) {
        ESP_LOGW(TAG, "WiFi 已啟動，主機名設置被忽略");
        return;
    }
    snprintf(s_hostname, sizeof(s_hostname), "%s", name);
}

esp_err_t board_mdns_start(const char *instance, uint16_t http_port)
{
    if (s_mdns_up) {
        return ESP_OK;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init 失敗: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(mdns_hostname_set(s_hostname));
    if (instance) {
        ESP_ERROR_CHECK(mdns_instance_name_set(instance));
    }
    if (http_port > 0) {
        /* 註冊成 _http._tcp，網絡發現工具和系統的網絡鄰居能直接看到 */
        ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", http_port, NULL, 0));
    }

    s_mdns_up = true;
    ESP_LOGI(TAG, "mDNS 已啟動: %s.local", s_hostname);
    return ESP_OK;
}

void board_wifi_stats(board_wifi_stats_t *out)
{
    out->disconnects = s_disconnects;
    out->connected = (strcmp(s_ip, "0.0.0.0") != 0);
    out->uptime_s = out->connected && s_connected_since
                        ? (uint32_t)((esp_timer_get_time() - s_connected_since) / 1000000)
                        : 0;

    wifi_ap_record_t ap;
    out->rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

const char *board_wifi_hostname(void)
{
    return s_hostname;
}

const char *board_wifi_ip(void)
{
    return s_ip;
}

const char *board_wifi_ssid_masked(void)
{
    static char masked[33];
    if (masked[0] == '\0') {
        const char *ssid = active_ssid();
        size_t n = strlen(ssid);
        if (n <= 2) {
            snprintf(masked, sizeof(masked), "**");
        } else {
            snprintf(masked, sizeof(masked), "%.2s***%c (%u 字符)",
                     ssid, ssid[n - 1], (unsigned)n);
        }
    }
    return masked;
}

void board_wifi_set_enterprise(const char *ssid, const char *identity,
                               const char *username, const char *password)
{
    if (s_started) {
        ESP_LOGW(TAG, "WiFi 已啟動，企業網配置被忽略");
        return;
    }
    snprintf(s_eap.ssid, sizeof(s_eap.ssid), "%s", ssid);
    snprintf(s_eap.identity, sizeof(s_eap.identity), "%s", identity);
    snprintf(s_eap.username, sizeof(s_eap.username), "%s", username);
    snprintf(s_eap.password, sizeof(s_eap.password), "%s", password);
    s_eap.on = true;
}
