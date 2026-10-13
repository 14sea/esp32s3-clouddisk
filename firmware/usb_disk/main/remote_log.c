#include "remote_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "board_wifi.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BUF_SIZE 16384      /* 與服務器 MAX_LOG 一致；滿了丟新日誌，不阻塞調用方 */

static char *s_buf;
static size_t s_len;
static uint32_t s_dropped;
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_orig;

/* 最近 8KB 日誌的環形副本，發走也不清；連不上網時靠 USB 讀它（usb_msc 的 FC 命令） */
#define TAIL_SIZE 8192
static char *s_tail;
static size_t s_tail_pos;       /* 下一個寫入位置 */
static bool s_tail_wrapped;

static void tail_append(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s_tail[s_tail_pos++] = p[i];
        if (s_tail_pos == TAIL_SIZE) { s_tail_pos = 0; s_tail_wrapped = true; }
    }
}

size_t remote_log_tail(void *buf, size_t len)
{
    if (!s_tail || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    size_t have = s_tail_wrapped ? TAIL_SIZE : s_tail_pos;
    if (len > have) len = have;
    size_t start = (s_tail_pos + TAIL_SIZE - len) % TAIL_SIZE;   /* 取最新的 len 字節 */
    for (size_t i = 0; i < len; i++) ((char *)buf)[i] = s_tail[(start + i) % TAIL_SIZE];
    xSemaphoreGive(s_lock);
    return len;
}

static int log_hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);
    if (n > 0 && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
        tail_append(line, n);
        if (s_len + n <= BUF_SIZE) {
            memcpy(s_buf + s_len, line, n);
            s_len += n;
        } else {
            s_dropped++;
        }
        xSemaphoreGive(s_lock);
    }
    return s_orig(fmt, ap);
}

static void sender_task(void *arg)
{
    esp_http_client_handle_t cli = arg;
    char *out = heap_caps_malloc(BUF_SIZE + 64, MALLOC_CAP_SPIRAM);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        board_wifi_stats_t w;
        board_wifi_stats(&w);
        if (!w.connected || s_len == 0) continue;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t n = s_len;
        memcpy(out, s_buf, n);
        s_len = 0;
        uint32_t dropped = s_dropped;
        s_dropped = 0;
        xSemaphoreGive(s_lock);
        if (dropped) n += snprintf(out + n, 64, "[remote_log] 丟了 %lu 行\n", dropped);
        if (n > BUF_SIZE) n = BUF_SIZE;

        esp_http_client_set_post_field(cli, out, n);
        if (esp_http_client_perform(cli) != ESP_OK) {
            esp_http_client_close(cli);     /* 這批丟掉，不重試，免得卡住後面的 */
        }
    }
}

void remote_log_start(const char *url, const char *auth_header)
{
    s_buf = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_tail = heap_caps_malloc(TAIL_SIZE, MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    if (!s_buf || !s_tail || !s_lock) return;

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_http_client_set_header(cli, "Authorization", auth_header);
    esp_http_client_set_header(cli, "Content-Type", "text/plain; charset=utf-8");

    s_orig = esp_log_set_vprintf(log_hook);
    xTaskCreatePinnedToCore(sender_task, "rlog", 6144, cli, 2, NULL, 0);
}
