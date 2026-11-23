/*
 * BLE Central 接收层（dongle 侧）
 *
 * 思路：dongle 自己当 BLE 主机去连接鼠标，收 HID Report 通知，
 * 再转成 USB HID 报告发给电脑。这样电脑只需看到一个普通 USB 鼠标，
 * 完全不经过 Windows 的蓝牙栈。
 *
 * 连接间隔由本端（Central）请求，先要 7.5 ms；后续可换 Nordic LLPM 1 ms。
 */
#ifndef BLE_CTR_H__
#define BLE_CTR_H__

#include <stdint.h>
#include <stdbool.h>

/* 鼠标广播名（与鼠标端 CONFIG_BT_DEVICE_NAME 一致） */
#define BLE_CTR_TARGET_NAME "XIAO Mouse"

int ble_ctr_init(void);

/* 统计：total = 累计收到的 HID 通知数，err = 解析异常数 */
void ble_ctr_stats(uint32_t *total, uint32_t *err);

/* 累计收到的"按键报告"(Report 1)通知数，用于区分按键链路丢在哪一段 */
uint32_t ble_ctr_btn_count(void);

/* 最近一次收到通知的时刻（k_uptime ms）；从未收到返回 0 */
int64_t ble_ctr_last_ms(void);

/* 当前是否已连接（含已完成订阅） */
bool ble_ctr_ready(void);

/* 鼠标电量百分比（来自标准 Battery Service 0x180F/0x2A19）。
 * 未连接或还没读到返回 -1。主循环据此驱动红灯（<=25% 常亮）。
 */
int ble_ctr_battery_level(void);

/* 诊断计数：安全成功/失败、特征发现完成、订阅成功、断链事件。
 * 只用于打在 5 s 统计行里（CDC 控制台会丢零散日志行）。
 */
void ble_ctr_diag(uint32_t *sec_ok, uint32_t *sec_err, uint32_t *disc_ok,
		  uint32_t *sub_ok, uint32_t *disc_evt);

/* BAS 诊断：发现成功 / 找到服务 / 找到特征 / 读成功 / 读失败 */
void ble_ctr_bas_diag(uint32_t *disc_ok, uint32_t *found, uint32_t *chr,
		      uint32_t *rd_ok, uint32_t *rd_err);

/* 下发配置命令。鼠标端目前只有标准 HID/BAS 服务，没有配置特征，
 * 所以恒返回 -ENOTSUP（设置只保存在 dongle，2.4G 变体才真正下发）。
 */
int ble_ctr_cfg_send(const uint8_t *data, uint8_t len);

#endif /* BLE_CTR_H__ */
