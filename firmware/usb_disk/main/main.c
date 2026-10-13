/*
 * usb_disk：ESP32-S3 原生 USB 口模擬成 U 盤。
 *   有 clouddisk_secrets.h：扇區經 WiFi + HTTPS 轉發到服務器上的鏡像文件（方向 2）
 *   沒有：扇區直通板載 SD 卡，沒卡就用 PSRAM 內存盤（方向 1，驗證 USB 本身）
 * 日誌走 UART0（CH343 那個 Type-C 口），網絡模式下同時回傳到服務器 esp.log。
 */
#include <inttypes.h>
#include <stdio.h>

#include "blockdev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_msc.h"

#if __has_include("clouddisk_secrets.h")
#include "board_wifi.h"
#include "clouddisk_secrets.h"
#include "remote_log.h"
#define USE_NETWORK 1
#else
#define USE_NETWORK 0
#endif

static const char *TAG = "usb_disk";

#define RAMDISK_MB 6

static const blockdev_t *open_backend(void)
{
#if USE_NETWORK
    static char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", CLOUDDISK_TOKEN);
    remote_log_start(CLOUDDISK_URL "/log", auth);
    usb_msc_set_diag(remote_log_tail);
    board_wifi_set_hostname_auto("clouddisk");
#ifdef CLOUDDISK_EAP_SSID
    board_wifi_set_enterprise(CLOUDDISK_EAP_SSID, CLOUDDISK_EAP_IDENTITY,
                              CLOUDDISK_EAP_USERNAME, CLOUDDISK_EAP_PASSWORD);
#endif
    return blockdev_http_open();
#else
    const blockdev_t *dev = blockdev_sd_open();
    if (!dev) {
        ESP_LOGW(TAG, "沒有 SD 卡，改用 %d MB PSRAM 內存盤", RAMDISK_MB);
        dev = blockdev_ram_open(RAMDISK_MB);
    }
    return dev;
#endif
}

void app_main(void)
{
    const blockdev_t *dev = open_backend();
    if (dev) {
        ESP_LOGI(TAG, "後端 %s", dev->name);
    }
    ESP_ERROR_CHECK(usb_msc_start(dev));
    ESP_LOGI(TAG, "USB_DISK_READY");

    /* 每 10 秒報一次吞吐 */
    usb_msc_stats_t prev = {0}, now;
    char extra[200] = "";
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        usb_msc_get_stats(&now);
        if (dev && dev->stats) dev->stats(extra, sizeof(extra));
        bool busy = now.bytes_read != prev.bytes_read || now.bytes_written != prev.bytes_written;
        if (busy || now.read_errors != prev.read_errors || now.write_errors != prev.write_errors) {
            ESP_LOGI(TAG, "%s%s 讀 %.1f KB/s 寫 %.1f KB/s 累計 R%.1fMB W%.1fMB 錯誤 R%" PRIu32 "/W%" PRIu32
                     " 最慢回調 R%" PRIu32 "ms W%" PRIu32 "ms | %s",
                     now.mounted ? "已連接" : "未連接", now.ejected ? "(已彈出)" : "",
                     (now.bytes_read - prev.bytes_read) / 10240.0,
                     (now.bytes_written - prev.bytes_written) / 10240.0,
                     now.bytes_read / 1048576.0, now.bytes_written / 1048576.0,
                     now.read_errors, now.write_errors, now.max_read_ms, now.max_write_ms, extra);
        }
        prev = now;
    }
}
