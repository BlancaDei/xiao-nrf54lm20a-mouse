/*
 * BLE HID 鼠标（HID over GATT）+ LLPM
 *
 * 说明：
 *   - 用 NCS 的 HIDS 服务（nrf/subsys/bluetooth/services/hids.c），
 *     上报格式与官方 peripheral_hids_mouse 示例一致：
 *       Report ID 1 = 按键 + 滚轮 + 横向拨动
 *       Report ID 2 = 12 bit X/Y 位移
 *   - LLPM（低延迟 1ms 连接间隔）：本模块只负责把控制器的 LLPM 模式打开
 *     （hci_vs_sdc_llpm_mode_set），并关掉 GAP 偏好参数。真正协商 1ms 需要
 *     对端也是 Nordic SDC 并主动发起；连普通 PC 蓝牙会退回常规间隔。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_HID_H
#define BLE_HID_H

#include <stdint.h>
#include <stdbool.h>

/* 鼠标按键位，对应 Report ID 1 的 bit0..bit4 */
#define BLE_HID_BTN_LEFT   0x01
#define BLE_HID_BTN_RIGHT  0x02
#define BLE_HID_BTN_MIDDLE 0x04
#define BLE_HID_BTN_SIDE1  0x08
#define BLE_HID_BTN_SIDE2  0x10

/**
 * @brief 初始化 HIDS 服务、打开蓝牙、开启 LLPM 模式并开始广播。
 * @return 0 成功，负值为 errno
 */
int ble_hid_init(void);

/** @brief 当前是否有主机连着。 */
bool ble_hid_connected(void);

/**
 * @brief 发一次位移上报（Report ID 2）。未连接时直接返回 0。
 * @param dx 相对位移，范围内会被限幅到 ±2047
 */
int ble_hid_send_motion(int16_t dx, int16_t dy);

/**
 * @brief 发一次按键/滚轮上报（Report ID 1）。未连接时直接返回 0。
 * @param buttons BLE_HID_BTN_* 的位或
 * @param wheel   纵向滚轮（-127..127）
 * @param pan     横向拨动（-127..127）
 */
int ble_hid_send_buttons(uint8_t buttons, int8_t wheel, int8_t pan);

/**
 * @brief 主动停止广播并断开当前连接（用于睡眠：PC 会认为设备掉线）。
 *
 * 只停广播 / 断链路，协议栈保持开启（不用 bt_disable）。
 * 调用后会抑制 disconnected 回调里的自动重播，直到 ble_hid_start 清除。
 */
void ble_hid_stop(void);

/**
 * @brief 重新开始广播（幂等）。用于唤醒，PC 凭已绑定信息会自动重连。
 */
void ble_hid_start(void);

/**
 * @brief 应用"系统模式"：0 = 高速（省电），1 = 竞技（更高发射功率）。
 *
 * ESB 变体直接把射频功率从 0 dBm 提到 +8 dBm；BLE 变体的发射功率由协议栈
 * 调度，这里不动（配置通道也只有 2.4G 链路在用）。
 */
void ble_hid_set_system_mode(uint8_t mode);

/**
 * @brief 更新回传给接收器的电量（仅 ESB 变体使用）。
 *
 * ESB 没有 BAS 通道，电量搭在每个包的 data[9] 上；BLE 变体仍走
 * 标准 Battery Service，不需要调用。0xFF 表示未知。
 */
void ble_hid_set_battery(uint8_t pct);

#endif /* BLE_HID_H */
