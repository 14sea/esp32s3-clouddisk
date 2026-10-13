#pragma once
#include "blockdev.h"

typedef struct {
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint32_t read_errors;
    uint32_t write_errors;
    uint32_t max_read_ms;       /* 單次讀回調的最長耗時，方向 2 看網絡延遲用 */
    uint32_t max_write_ms;
    bool     mounted;           /* 電腦已枚舉 */
    bool     ejected;           /* 電腦端點了「彈出」 */
} usb_msc_stats_t;

/* dev 可以是 NULL：照樣枚舉成 U 盤，但報告「沒有介質」 */
esp_err_t usb_msc_start(const blockdev_t *dev);
void usb_msc_get_stats(usb_msc_stats_t *out);

/* 主機發 FC 'L''O''G' 時調用，把診斷文本拷進 buf，返回字節數 */
typedef size_t (*usb_msc_diag_fn)(void *buf, size_t len);
void usb_msc_set_diag(usb_msc_diag_fn fn);
