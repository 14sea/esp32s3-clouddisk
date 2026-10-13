#pragma once
/* TinyUSB 配置：只開 MSC，跑在 S3 的 OTG 全速口上（GPIO19/20） */

#define CFG_TUSB_MCU                OPT_MCU_ESP32S3
#define CFG_TUSB_OS                 OPT_OS_FREERTOS
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_DEBUG              0
#define CFG_TUD_DWC2_SLAVE_ENABLE   1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#define CFG_TUSB_MEM_ALIGN          TU_ATTR_ALIGNED(4)

#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 1
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

/*
 * 每次讀寫回調最多搬這麼多字節，必須是 512 的整數倍（回調裡按整扇區處理）。
 * 實測 16KB 的 OUT 傳輸會卡死（主機 121 秒超時後把盤下線），4KB 正常：
 * S3 的 DWC2 單次傳輸包數有上限，64 字節一包，16KB = 256 包超了。
 */
#define CFG_TUD_MSC_EP_BUFSIZE      4096
