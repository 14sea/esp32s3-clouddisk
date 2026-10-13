#pragma once
/*
 * 日誌回傳：攔截 ESP_LOG 輸出，攢起來每 2 秒 POST 到服務器 /log（落在 ~/clouddisk/esp.log）。
 * 板子只插原生 USB 口時沒有串口可看，排查全靠這個。串口照常輸出。
 */
void remote_log_start(const char *url, const char *auth_header);

/* 拷出最近 len 字節日誌（最多 8KB），返回實際字節數 */
#include <stddef.h>
size_t remote_log_tail(void *buf, size_t len);
