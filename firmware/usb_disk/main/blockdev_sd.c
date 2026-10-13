/*
 * SD 卡直通：不在 ESP32 上掛 FATFS，扇區原樣交給電腦。
 * 電腦和 ESP32 同時掛載同一個 FAT 必然損壞，所以這裡只做裸扇區讀寫。
 */
#include "blockdev.h"

#include "board_pins.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"

static const char *TAG = "bd_sd";
static sdmmc_card_t s_card;

static esp_err_t sd_read(uint32_t lba, void *buf, uint32_t count)
{
    return sdmmc_read_sectors(&s_card, buf, lba, count);
}

static esp_err_t sd_write(uint32_t lba, const void *buf, uint32_t count)
{
    return sdmmc_write_sectors(&s_card, buf, lba, count);
}

static blockdev_t s_dev = { .name = "sd", .read = sd_read, .write = sd_write };

const blockdev_t *blockdev_sd_open(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_PIN_CLK;
    slot.cmd = SD_PIN_CMD;
    slot.d0  = SD_PIN_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = sdmmc_host_init();
    if (err == ESP_OK) err = sdmmc_host_init_slot(host.slot, &slot);
    if (err == ESP_OK) err = sdmmc_card_init(&host, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD 卡初始化失敗: %s（沒插卡？）", esp_err_to_name(err));
        return NULL;
    }
    s_dev.sector_count = s_card.csd.capacity;
    if (s_card.csd.sector_size != BLOCKDEV_SECTOR) {
        ESP_LOGE(TAG, "扇區大小 %d 不是 512，不支持", s_card.csd.sector_size);
        return NULL;
    }
    sdmmc_card_print_info(stdout, &s_card);
    return &s_dev;
}
