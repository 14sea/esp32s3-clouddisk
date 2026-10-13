#pragma once
/*
 * 塊設備後端：U 盤的每次扇區讀寫都落到這裡。
 * 方向 1 是 SD 卡（blockdev_sd.c），方向 2 換成 HTTP 轉發到服務器。
 * 扇區固定 512 字節；回調在 TinyUSB 任務裡同步調用。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#define BLOCKDEV_SECTOR 512

typedef struct {
    const char *name;
    volatile uint32_t sector_count;        /* 0 表示介質不在；網絡後端連上服務器後才填 */
    esp_err_t (*read)(uint32_t lba, void *buf, uint32_t count);
    esp_err_t (*write)(uint32_t lba, const void *buf, uint32_t count);
    esp_err_t (*sync)(void);               /* 可為 NULL：把寫回緩存落盤 */
    void (*stats)(char *buf, size_t len);  /* 可為 NULL：一行運行統計 */
} blockdev_t;

/* 失敗時返回 NULL，U 盤會顯示為「請插入磁盤」 */
const blockdev_t *blockdev_sd_open(void);

/* PSRAM 內存盤，未格式化，電腦端第一次插上要格式化 */
const blockdev_t *blockdev_ram_open(uint32_t size_mb);

/*
 * 網絡後端：扇區經 HTTPS 轉發到服務器上的鏡像文件（server/clouddisk_server.py）。
 * 立即返回；WiFi 和服務器在後台連，連上之前 sector_count 為 0（U 盤顯示無介質）。
 */
const blockdev_t *blockdev_http_open(void);
