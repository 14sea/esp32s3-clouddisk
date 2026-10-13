/*
 * PSRAM 內存盤：沒有 SD 卡時的驗證用後端，斷電即失。
 * 8MB PSRAM 留 2MB 給系統（方向 2 的 WiFi/TLS 緩衝也要用 PSRAM）。
 */
#include "blockdev.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "bd_ram";
static uint8_t *s_mem;
static blockdev_t s_dev;

static bool in_range(uint32_t lba, uint32_t count)
{
    return lba < s_dev.sector_count && count <= s_dev.sector_count - lba;
}

static esp_err_t ram_read(uint32_t lba, void *buf, uint32_t count)
{
    if (!in_range(lba, count)) return ESP_ERR_INVALID_ARG;
    memcpy(buf, s_mem + (size_t)lba * BLOCKDEV_SECTOR, (size_t)count * BLOCKDEV_SECTOR);
    return ESP_OK;
}

static esp_err_t ram_write(uint32_t lba, const void *buf, uint32_t count)
{
    if (!in_range(lba, count)) return ESP_ERR_INVALID_ARG;
    memcpy(s_mem + (size_t)lba * BLOCKDEV_SECTOR, buf, (size_t)count * BLOCKDEV_SECTOR);
    return ESP_OK;
}

static blockdev_t s_dev = { .name = "ram", .read = ram_read, .write = ram_write };

const blockdev_t *blockdev_ram_open(uint32_t size_mb)
{
    size_t bytes = (size_t)size_mb * 1024 * 1024;
    s_mem = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
    if (!s_mem) {
        ESP_LOGE(TAG, "PSRAM 分配 %lu MB 失敗", size_mb);
        return NULL;
    }
    s_dev.sector_count = bytes / BLOCKDEV_SECTOR;
    return &s_dev;
}
