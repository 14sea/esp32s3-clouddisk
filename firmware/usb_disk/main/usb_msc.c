/*
 * USB 大容量存儲（MSC）：TinyUSB 回調 + 描述符 + 任務。
 * 扇區讀寫轉給 blockdev_t 後端，USB 這一層不關心數據最終存在哪裡。
 */
#include "usb_msc.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "soc/rtc_cntl_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

static const char *TAG = "usb_msc";

static const blockdev_t *s_dev;
static usb_msc_stats_t s_stats;
static usb_phy_handle_t s_phy;
static usb_msc_diag_fn s_diag;

void usb_msc_set_diag(usb_msc_diag_fn fn)
{
    s_diag = fn;
}

/* ---------------- 描述符 ---------------- */

#define USB_VID 0x303A          /* Espressif */
#define USB_PID 0x4002          /* 與 esp_tinyusb 的 MSC 編號規則一致（Espressif VID 僅限自用/測試） */

#define USB_MANUFACTURER "DIY"
#define SCSI_VENDOR      "DIY     "   /* SCSI INQUIRY 廠商名，固定 8 字節 */

enum { ITF_NUM_MSC = 0, ITF_NUM_TOTAL };
enum { STRID_LANGID = 0, STRID_MANUFACTURER, STRID_PRODUCT, STRID_SERIAL, STRID_MSC };

#define EPNUM_MSC_OUT 0x01
#define EPNUM_MSC_IN  0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const tusb_desc_device_t s_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,     /* 由接口描述符決定 */
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t s_desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 500),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

static char s_serial[13];       /* MAC 的 12 位十六進制，同型號多塊板子不撞 */

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&s_desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_desc_config;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[32];
    const char *str;
    switch (index) {
    case STRID_LANGID:
        buf[0] = (TUSB_DESC_STRING << 8) | 4;
        buf[1] = 0x0409;
        return buf;
    case STRID_MANUFACTURER: str = USB_MANUFACTURER; break;
    case STRID_PRODUCT:      str = "ESP32-S3 Cloud Disk"; break;
    case STRID_SERIAL:       str = s_serial; break;
    case STRID_MSC:          str = "Cloud Disk MSC"; break;
    default:                 return NULL;
    }
    size_t n = strlen(str);
    if (n > 31) n = 31;
    for (size_t i = 0; i < n; i++) buf[1 + i] = str[i];
    buf[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    return buf;
}

/* ---------------- MSC 回調 ---------------- */

void tud_mount_cb(void)   { s_stats.mounted = true;  s_stats.ejected = false; ESP_LOGI(TAG, "電腦已識別"); }
void tud_umount_cb(void)  { s_stats.mounted = false; ESP_LOGI(TAG, "USB 斷開"); }

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id,   SCSI_VENDOR, 8);
    memcpy(product_id,  "Cloud Disk      ", 16);
    memcpy(product_rev, "0.1 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    if (!s_dev || s_dev->sector_count == 0 || s_stats.ejected) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);   /* MEDIUM NOT PRESENT */
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = s_dev ? s_dev->sector_count : 0;
    *block_size  = BLOCKDEV_SECTOR;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)power_condition;
    if (!load_eject) return true;
    if (start) {
        s_stats.ejected = false;
        ESP_LOGI(TAG, "電腦端重新載入");
        return true;
    }
    /* 彈出前把寫回緩存推上去；推不上去就讓彈出失敗，主機會提示而不是默默丟數據 */
    if (s_dev && s_dev->sync && s_dev->sync() != ESP_OK) {
        ESP_LOGW(TAG, "彈出前同步失敗，拒絕彈出");
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0C, 0x00);   /* WRITE ERROR */
        return false;
    }
    s_stats.ejected = true;
    ESP_LOGI(TAG, "電腦端彈出");
    return true;
}

static uint32_t ms_since(int64_t t0)
{
    return (uint32_t)((esp_timer_get_time() - t0) / 1000);
}

/*
 * 讀寫前的公共檢查。TinyUSB 不會先拿 LBA 和容量比，越界請求會原樣送到後端
 * （PSRAM 後端直接 memcpy 就越界了），所以這裡把關：
 * 介質要在、緩衝要按扇區對齊、[lba, lba+count) 要落在盤內（寫成減法避免加法溢出）。
 */
static bool io_allowed(uint8_t lun, uint32_t lba, uint32_t offset, uint32_t bufsize)
{
    if (!s_dev || s_dev->sector_count == 0 || s_stats.ejected) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);        /* MEDIUM NOT PRESENT */
        return false;
    }
    /* EP 緩衝是 512 的整數倍，TinyUSB 每次都從扇區邊界開始 */
    if (offset != 0 || bufsize % BLOCKDEV_SECTOR) return false;
    uint32_t count = bufsize / BLOCKDEV_SECTOR;
    if (lba >= s_dev->sector_count || count > s_dev->sector_count - lba) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);  /* LBA OUT OF RANGE */
        return false;
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    if (!io_allowed(lun, lba, offset, bufsize)) return -1;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = s_dev->read(lba, buffer, bufsize / BLOCKDEV_SECTOR);
    uint32_t ms = ms_since(t0);
    if (ms > s_stats.max_read_ms) s_stats.max_read_ms = ms;
    if (err != ESP_OK) {
        s_stats.read_errors++;
        ESP_LOGW(TAG, "讀 lba=%lu n=%lu 失敗: %s", lba, bufsize / BLOCKDEV_SECTOR, esp_err_to_name(err));
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);  /* UNRECOVERED READ ERROR */
        return -1;
    }
    s_stats.bytes_read += bufsize;
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    if (!io_allowed(lun, lba, offset, bufsize)) return -1;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = s_dev->write(lba, buffer, bufsize / BLOCKDEV_SECTOR);
    uint32_t ms = ms_since(t0);
    if (ms > s_stats.max_write_ms) s_stats.max_write_ms = ms;
    if (err != ESP_OK) {
        s_stats.write_errors++;
        ESP_LOGW(TAG, "寫 lba=%lu n=%lu 失敗: %s", lba, bufsize / BLOCKDEV_SECTOR, esp_err_to_name(err));
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0C, 0x00);  /* WRITE ERROR */
        return -1;
    }
    s_stats.bytes_written += bufsize;
    return (int32_t)bufsize;
}

/*
 * 軟件進下載模式：原生口被 U 盤佔用後 esptool 連不上，以前只能按 BOOT+RST。
 * 收到廠商命令 FB 'B''O''O''T' 後，把內部 PHY 切回 USB-Serial-JTAG
 * （選擇位在 RTC 寄存器裡，軟件復位後保留），再置強制下載標誌重啟，
 * ROM 就會在同一個口上等 esptool。
 */
#define SCSI_CMD_REBOOT_DOWNLOAD 0xFB
#define SCSI_CMD_READ_LOG        0xFC   /* FC 'L' 'O' 'G'：讀最近的日誌，不用串口也能排查 */

static void reboot_to_download(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "切回 USB-Serial-JTAG 並重啟進下載模式");
    /* 先撤掉 D+ 上拉並停一會兒，讓主機明確看到「拔出」。不這樣做主機一直以為
     * U 盤還在（Windows 上 usbipd 顯示 4002 Attached），不會去枚舉新的下載口 */
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    usb_del_phy(s_phy);
    usb_phy_config_t usj = {
        .controller = USB_PHY_CTRL_SERIAL_JTAG,
        .target     = USB_PHY_TARGET_INT,
    };
    usb_phy_handle_t h;
    usb_new_phy(&usj, &h);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
    case 0x35:  /* SYNCHRONIZE CACHE(10)：網絡後端在這裡刷寫回緩存 */
        if (s_dev && s_dev->sync && s_dev->sync() != ESP_OK) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0C, 0x00);
            return -1;
        }
        return 0;
    case SCSI_CMD_READ_LOG:
        if (memcmp(&scsi_cmd[1], "LOG", 3) != 0 || !s_diag) break;
        return (int32_t)s_diag(buffer, bufsize);
    case SCSI_CMD_REBOOT_DOWNLOAD:
        if (memcmp(&scsi_cmd[1], "BOOT", 4) != 0) break;
        /* 先把這條命令的狀態回給主機，300ms 後再切 PHY，不然主機會當成命令超時 */
        static esp_timer_handle_t t;
        if (!t) {
            const esp_timer_create_args_t a = { .callback = reboot_to_download, .name = "dl" };
            esp_timer_create(&a, &t);
        }
        esp_timer_start_once(t, 300 * 1000);
        return 0;
    default:
        break;
    }
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);  /* INVALID COMMAND */
    return -1;
}

/* ---------------- 啟動 ---------------- */

/*
 * 網絡後端開機時沒有介質，Linux 枚舉時看到「無介質」就不再輪詢（dmesg: stopped polling），
 * 之後介質就緒它也不知道。所以盤從無到有時斷開重連一次 USB，讓主機重新識別容量。
 */
static void media_watch_task(void *arg)
{
    (void)arg;
    uint32_t last = s_dev ? s_dev->sector_count : 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        uint32_t now = s_dev ? s_dev->sector_count : 0;
        if (last == 0 && now > 0 && tud_inited()) {
            ESP_LOGI(TAG, "介質就緒，重新枚舉 USB");
            tud_disconnect();
            vTaskDelay(pdMS_TO_TICKS(1000));
            tud_connect();
        }
        last = now;
    }
}

static void usb_task(void *arg)
{
    (void)arg;
    if (!tusb_init()) {
        ESP_LOGE(TAG, "TinyUSB 初始化失敗");
        vTaskDelete(NULL);
    }
    for (;;) tud_task();    /* 內部阻塞等事件，不會空轉 */
}

esp_err_t usb_msc_start(const blockdev_t *dev)
{
    s_dev = dev;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* 內部 PHY 從 USB-Serial-JTAG 切到 OTG，原生口從此變成 U 盤 */
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,
        .otg_mode   = USB_OTG_MODE_DEVICE,
    };
    ESP_RETURN_ON_ERROR(usb_new_phy(&phy_conf, &s_phy), TAG, "USB PHY 初始化失敗");

    BaseType_t ok = xTaskCreatePinnedToCore(usb_task, "usb", 6144, NULL, 5, NULL, 1);
    if (ok == pdPASS && s_dev && s_dev->sector_count == 0) {
        ok = xTaskCreate(media_watch_task, "msc_watch", 3072, NULL, 3, NULL);
    }
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void usb_msc_get_stats(usb_msc_stats_t *out)
{
    *out = s_stats;
}
