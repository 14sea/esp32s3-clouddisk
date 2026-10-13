/*
 * 網絡後端：U 盤扇區經 HTTPS 轉發到服務器上的鏡像文件。
 * 路徑：ESP32 → WiFi → 公網 → HTTPS 反向代理（如 Tailscale Funnel）→ 127.0.0.1:8090 clouddisk_server.py
 *
 * 每次 USB 回調只有 4KB（CFG_TUD_MSC_EP_BUFSIZE），逐個發 HTTP 會被往返延遲拖死，所以：
 *   讀：PSRAM 裡 48 塊 × 64KB 的 LRU 緩存，未命中就整塊拉 64KB（預讀）
 *   寫：連續的寫攢到 64KB 再一次 PUT（寫回）；同時更新已緩存的塊，讀到的永遠是新數據
 *       落盤時機：攢滿 / 寫不連續 / 空閒 300ms / 主機發 SYNCHRONIZE CACHE / 彈出
 * 寫回意味著回調返回成功時數據可能還在 ESP32 內存裡：這 300ms 內斷電或斷網會丟。
 * 落盤失敗會計數並記日誌（flush_err），驗證階段先接受這個取捨。
 */
#include "blockdev.h"

/* 沒有 clouddisk_secrets.h（本地模式）時整個文件不參與編譯 */
#if __has_include("clouddisk_secrets.h")

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_wifi.h"
#include "clouddisk_secrets.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bd_http";

#define CHUNK_SECTORS   128                             /* 預讀單位 64KB */
#define CHUNK_BYTES     (CHUNK_SECTORS * BLOCKDEV_SECTOR)
#define CACHE_CHUNKS    48                              /* 3MB PSRAM */
#define WBUF_SECTORS    128                             /* 寫合併上限 64KB */
#define FLUSH_IDLE_US   (300 * 1000)
#define RETRIES         3

typedef struct {
    bool     valid;
    uint32_t chunk;     /* 塊號 = lba / CHUNK_SECTORS */
    uint32_t stamp;     /* LRU 時間戳 */
    uint8_t *data;
} cache_ent_t;

static esp_http_client_handle_t s_cli;
static SemaphoreHandle_t s_lock;        /* 串行化 USB 回調和後台落盤任務 */
static char s_auth[128];

static cache_ent_t s_cache[CACHE_CHUNKS];
static uint32_t s_tick;

static uint8_t *s_wbuf;
static uint32_t s_wlba, s_wcount;
static int64_t s_wlast_us;

static struct {
    uint32_t reqs, retries, errors;
    uint32_t hits, misses;
    uint32_t flushes, flush_err;
    uint32_t max_req_ms;
} s_st;

static blockdev_t s_dev;

/* ---------------- HTTP ---------------- */

/*
 * 發一個請求。body 非空時作為請求體；out 非空時要求響應體恰好 out_len 字節。
 * 連接保持複用（keep-alive），出錯就關掉連接、退避後重試。
 */
static esp_err_t http_req(esp_http_client_method_t method, const char *path,
                          const void *body, int body_len, void *out, int out_len)
{
    char url[192];
    snprintf(url, sizeof(url), "%s%s", CLOUDDISK_URL, path);

    for (int attempt = 0; attempt < RETRIES; attempt++) {
        int64_t t0 = esp_timer_get_time();
        s_st.reqs++;
        esp_http_client_set_url(s_cli, url);
        esp_http_client_set_method(s_cli, method);

        esp_err_t err = esp_http_client_open(s_cli, body_len);
        if (err == ESP_OK && body_len > 0 &&
            esp_http_client_write(s_cli, body, body_len) != body_len) {
            err = ESP_FAIL;
        }
        int status = 0;
        if (err == ESP_OK) {
            int64_t clen = esp_http_client_fetch_headers(s_cli);
            status = esp_http_client_get_status_code(s_cli);
            if (clen < 0) {
                err = ESP_FAIL;
            } else if (status / 100 != 2) {
                err = ESP_FAIL;
            } else if (out) {
                int got = 0;
                while (got < out_len) {
                    int r = esp_http_client_read(s_cli, (char *)out + got, out_len - got);
                    if (r <= 0) break;
                    got += r;
                }
                if (got != out_len) err = ESP_FAIL;
            }
            if (err == ESP_OK && !out) {
                esp_http_client_flush_response(s_cli, NULL);
            }
        }

        uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        if (ms > s_st.max_req_ms) s_st.max_req_ms = ms;
        if (err == ESP_OK) return ESP_OK;

        esp_http_client_close(s_cli);
        if (status == 400 || status == 401) {
            ESP_LOGE(TAG, "%s → HTTP %d，不重試", path, status);
            break;
        }
        s_st.retries++;
        ESP_LOGW(TAG, "%s 失敗（HTTP %d，第 %d 次），重連重試", path, status, attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(200 << attempt));
    }
    s_st.errors++;
    return ESP_FAIL;
}

/* ---------------- 寫合併 ---------------- */

static esp_err_t wbuf_flush(void)
{
    if (s_wcount == 0) return ESP_OK;
    char path[48];
    snprintf(path, sizeof(path), "/blk/%" PRIu32, s_wlba);
    esp_err_t err = http_req(HTTP_METHOD_PUT, path, s_wbuf, s_wcount * BLOCKDEV_SECTOR, NULL, 0);
    s_st.flushes++;
    if (err != ESP_OK) {
        s_st.flush_err++;
        ESP_LOGE(TAG, "落盤失敗，丟失 lba=%" PRIu32 " 起 %" PRIu32 " 扇區", s_wlba, s_wcount);
    }
    s_wcount = 0;
    return err;
}

static bool wbuf_overlaps(uint32_t lba, uint32_t count)
{
    return s_wcount && lba < s_wlba + s_wcount && s_wlba < lba + count;
}

/* ---------------- 讀緩存 ---------------- */

static cache_ent_t *cache_get(uint32_t chunk)
{
    cache_ent_t *victim = &s_cache[0];
    for (int i = 0; i < CACHE_CHUNKS; i++) {
        cache_ent_t *e = &s_cache[i];
        if (e->valid && e->chunk == chunk) {
            e->stamp = ++s_tick;
            s_st.hits++;
            return e;
        }
        if (!e->valid || (victim->valid && e->stamp < victim->stamp)) victim = e;
    }

    /* 未命中：服務器上的數據可能比寫緩衝裡的舊，先落盤再拉 */
    s_st.misses++;
    uint32_t lba = chunk * CHUNK_SECTORS;
    uint32_t n = CHUNK_SECTORS;
    if (lba + n > s_dev.sector_count) n = s_dev.sector_count - lba;
    if (wbuf_overlaps(lba, n) && wbuf_flush() != ESP_OK) return NULL;

    char path[48];
    snprintf(path, sizeof(path), "/blk/%" PRIu32 "?n=%" PRIu32, lba, n);
    victim->valid = false;
    if (http_req(HTTP_METHOD_GET, path, NULL, 0, victim->data, n * BLOCKDEV_SECTOR) != ESP_OK) {
        return NULL;
    }
    victim->valid = true;
    victim->chunk = chunk;
    victim->stamp = ++s_tick;
    return victim;
}

/* 已緩存的塊同步更新，未緩存的不管（下次讀會先落盤再拉） */
static void cache_update(uint32_t lba, const uint8_t *buf, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t s = lba + i;
        for (int k = 0; k < CACHE_CHUNKS; k++) {
            cache_ent_t *e = &s_cache[k];
            if (e->valid && e->chunk == s / CHUNK_SECTORS) {
                memcpy(e->data + (s % CHUNK_SECTORS) * BLOCKDEV_SECTOR,
                       buf + i * BLOCKDEV_SECTOR, BLOCKDEV_SECTOR);
                break;
            }
        }
    }
}

/* ---------------- blockdev 接口 ---------------- */

static esp_err_t http_read(uint32_t lba, void *buf, uint32_t count)
{
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t *dst = buf;
    while (count) {
        uint32_t off = lba % CHUNK_SECTORS;
        uint32_t n = CHUNK_SECTORS - off;
        if (n > count) n = count;
        cache_ent_t *e = cache_get(lba / CHUNK_SECTORS);
        if (!e) { err = ESP_FAIL; break; }
        memcpy(dst, e->data + off * BLOCKDEV_SECTOR, n * BLOCKDEV_SECTOR);
        dst += n * BLOCKDEV_SECTOR;
        lba += n;
        count -= n;
    }
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t http_write(uint32_t lba, const void *buf, uint32_t count)
{
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cache_update(lba, buf, count);
    /* 接不上前一段、或放不下，就先把前一段推上去 */
    if (s_wcount && (lba != s_wlba + s_wcount || s_wcount + count > WBUF_SECTORS)) {
        err = wbuf_flush();
    }
    if (s_wcount == 0) s_wlba = lba;
    memcpy(s_wbuf + s_wcount * BLOCKDEV_SECTOR, buf, count * BLOCKDEV_SECTOR);
    s_wcount += count;
    s_wlast_us = esp_timer_get_time();
    if (s_wcount == WBUF_SECTORS) err = wbuf_flush();
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t http_sync(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = wbuf_flush();
    if (err == ESP_OK) err = http_req(HTTP_METHOD_POST, "/sync", NULL, 0, NULL, 0);
    xSemaphoreGive(s_lock);
    return err;
}

static void http_stats(char *buf, size_t len)
{
    board_wifi_stats_t w;
    board_wifi_stats(&w);
    uint32_t total = s_st.hits + s_st.misses;
    snprintf(buf, len,
             "請求 %" PRIu32 "（重試 %" PRIu32 " 失敗 %" PRIu32 " 最慢 %" PRIu32 "ms）"
             " 緩存命中 %" PRIu32 "/%" PRIu32 " 落盤 %" PRIu32 "（失敗 %" PRIu32 "）RSSI %d",
             s_st.reqs, s_st.retries, s_st.errors, s_st.max_req_ms,
             s_st.hits, total, s_st.flushes, s_st.flush_err, w.rssi);
}

/* 後台：連 WiFi → 問服務器盤多大 → 之後每 100ms 檢查寫緩衝空閒超時 */
static void http_task(void *arg)
{
    (void)arg;
    static board_ap_info_t aps[24];
    int found = 0;
    if (board_wifi_scan(aps, 24, &found) == ESP_OK) {
        int hits = 0, best = -127;
        for (int i = 0; i < found && i < 24; i++) {
            if (aps[i].is_target) {
                hits++;
                ESP_LOGI(TAG, "  目標 AP：信道 %d，%d dBm，authmode=%d",
                         aps[i].channel, aps[i].rssi, aps[i].authmode);
                if (aps[i].rssi > best) best = aps[i].rssi;
            }
        }
        ESP_LOGI(TAG, "掃描到 %d 個 AP，目標網絡 %s 有 %d 個（最強 %d dBm）",
                 found, board_wifi_ssid_masked(), hits, hits ? best : 0);
    }
    int64_t t0 = esp_timer_get_time();
    while (board_wifi_connect(30000) != ESP_OK) {
        ESP_LOGW(TAG, "WiFi %lld 秒仍未連上，繼續等", (esp_timer_get_time() - t0) / 1000000);
    }
    ESP_LOGI(TAG, "WiFi 已連上，IP %s，用時 %lld 秒", board_wifi_ip(), (esp_timer_get_time() - t0) / 1000000);

    char info[160] = {0};
    for (;;) {
        /* /info 的長度不固定，讀到多少算多少 */
        esp_http_client_set_url(s_cli, CLOUDDISK_URL "/info");
        esp_http_client_set_method(s_cli, HTTP_METHOD_GET);
        if (esp_http_client_open(s_cli, 0) == ESP_OK &&
            esp_http_client_fetch_headers(s_cli) >= 0 &&
            esp_http_client_get_status_code(s_cli) == 200) {
            int r = esp_http_client_read(s_cli, info, sizeof(info) - 1);
            esp_http_client_flush_response(s_cli, NULL);
            const char *p = r > 0 ? strstr(info, "\"sectors\":") : NULL;
            if (p) {
                s_dev.sector_count = strtoul(p + 10, NULL, 10);
                break;
            }
        }
        ESP_LOGW(TAG, "服務器 %s 不可達（HTTP %d），5 秒後重試",
                 CLOUDDISK_URL, esp_http_client_get_status_code(s_cli));
        esp_http_client_close(s_cli);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGI(TAG, "服務器盤 %" PRIu32 " 扇區（%" PRIu32 " MB），U 盤上線",
             s_dev.sector_count, s_dev.sector_count / 2048);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (s_wcount && esp_timer_get_time() - s_wlast_us > FLUSH_IDLE_US &&
            xSemaphoreTake(s_lock, 0) == pdTRUE) {
            if (s_wcount && esp_timer_get_time() - s_wlast_us > FLUSH_IDLE_US) wbuf_flush();
            xSemaphoreGive(s_lock);
        }
    }
}

const blockdev_t *blockdev_http_open(void)
{
    for (int i = 0; i < CACHE_CHUNKS; i++) {
        s_cache[i].data = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_cache[i].data) return NULL;
    }
    s_wbuf = heap_caps_malloc(WBUF_SECTORS * BLOCKDEV_SECTOR, MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    if (!s_wbuf || !s_lock) return NULL;

    esp_http_client_config_t cfg = {
        .url               = CLOUDDISK_URL "/info",
        .crt_bundle_attach = esp_crt_bundle_attach,   /* 用 ESP-IDF 內置的公共 CA 庫驗證服務器證書 */
        .timeout_ms        = 15000,
        .keep_alive_enable = true,
        .buffer_size       = 4096,
        .buffer_size_tx    = 2048,
    };
    s_cli = esp_http_client_init(&cfg);
    if (!s_cli) return NULL;
    snprintf(s_auth, sizeof(s_auth), "Bearer %s", CLOUDDISK_TOKEN);
    esp_http_client_set_header(s_cli, "Authorization", s_auth);

    s_dev = (blockdev_t){
        .name = "http", .sector_count = 0,
        .read = http_read, .write = http_write, .sync = http_sync, .stats = http_stats,
    };
    xTaskCreatePinnedToCore(http_task, "bd_http", 8192, NULL, 4, NULL, 0);
    return &s_dev;
}

#endif /* __has_include("clouddisk_secrets.h") */
