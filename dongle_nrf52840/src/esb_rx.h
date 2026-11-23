/*
 * ESB PRX 接收层（dongle 侧）
 *
 * 2.4G 私有协议：Nordic ESB（Enhanced ShockBurst）
 *   2 Mbps / 信道 75（2475 MHz）/ 16bit CRC / 固定 10 字节负载
 *   地址：base0 = A5:3C:9D:71，prefix = 2E
 *
 * 负载布局（与鼠标端 esb_ptx.c 共用约定）：
 *   [0] seq       包序号（丢包/乱序观测用）
 *   [1] btn       按键位图（bit0=左键 bit1=右键 bit2=中键，与 HID 报告一致）
 *   [2..3] dx     int16 小端位移
 *   [4..5] dy     int16 小端位移
 *   [6..7] wheel  int16 小端 **累计计数**（不是相对量）
 *                 接收器用"本包计数 - 上次计数"得到滚动格数；丢包后下一包
 *                 自动补齐，重复包差值为 0，不会重复滚动。
 *   [8]           保留（0）
 *   [9] batt      电量百分比（1..100，0xFF/0 = 未知）
 */
#ifndef ESB_RX_H__
#define ESB_RX_H__

#include <stdint.h>
#include <stdbool.h>

/* 信道 75 = 2475 MHz：避开邻居 Wi-Fi 信道 6（2426~2448 MHz）和
 * 信道 11（2451~2473 MHz）。必须与鼠标端 esb_ptx.c 的 ESB_RF_CHANNEL 一致。
 */
#define ESB_RF_CHANNEL   75
#define ESB_PAYLOAD_LEN  10
/* 地址 5 字节，鼠标端必须一致 */
#define ESB_ADDR_BASE    { 0xA5, 0x3C, 0x9D, 0x71 }
#define ESB_ADDR_PREFIX  0x2E

int esb_rx_init(void);

/* 统计：total = 累计正确收包数，err = CRC/长度异常包数 */
void esb_rx_stats(uint32_t *total, uint32_t *err);

/* 最近一次收包时刻（k_uptime_ms）；从未收包返回 0 */
int64_t esb_rx_last_ms(void);

/* 鼠标回传的电量百分比；超过 30 s 没更新或从未收到返回 -1 */
int esb_rx_battery_level(void);

/* 把一包配置命令塞进 PRX 的 ACK payload 队列（鼠标端在 RX_RECEIVED 事件里收）。
 * 队列空/已初始化失败时返回负 errno。
 */
int esb_rx_cfg_send(const uint8_t *data, uint8_t len);

#endif /* ESB_RX_H__ */
