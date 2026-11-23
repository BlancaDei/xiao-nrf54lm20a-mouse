/*
 * 厂商 HID 配置通道（dongle 侧）
 *
 * PC 驱动通过 USB Feature Report（ID 0x06，32 字节）读写鼠标设置：
 *   SetFeature: [0]=0xA5 [1]=cmd [2]=index [3..4]=value(u16 LE)
 *   GetFeature: [0]=0x5A [1]=版本 [2]=有效挡位 [3]=当前挡位
 *               [4..5]=回报率 [6]=系统模式 [7]=运动同步
 *               [8]=防误触发 [9]=静默高度 [10..21]=6 档 DPI
 *               [22]=链路(0 未连接/1 ESB/2 BLE)
 *
 * 设置改完立刻落盘（NVS），并通过链路层下发给鼠标：
 *   ESB 变体 -> PRX 的 ACK payload（鼠标 PTX 在 ESB_EVENT_RX_RECEIVED 里收）
 *   BLE 变体 -> 鼠标暂无配置特征，返回 -ENOTSUP（日志提示一次）
 *
 * 链路包（8 字节）：[0]=0xC7 [1]=cmd [2]=index [3..4]=value [5]=seq [6..7]=0
 * 同一命令连发 3 次（60 ms 内），丢一包也能在下一包补上。
 */
#ifndef HID_CFG_H__
#define HID_CFG_H__

#include <stdint.h>
#include <stdbool.h>

#define HID_CFG_REPORT_ID     0x06
#define HID_CFG_REPORT_LEN    32

#define HID_CFG_MAGIC_SET     0xA5
#define HID_CFG_MAGIC_GET     0x5A
#define HID_CFG_MAGIC_LINK    0xC7
#define HID_CFG_VERSION       2

/* SetFeature 命令码 */
enum hid_cfg_cmd {
	HID_CFG_CMD_GET_STATE     = 0x01, /* 空操作：状态永远从 GetFeature 读 */
	HID_CFG_CMD_SET_DPI       = 0x02, /* index=挡位(0..5)，value=CPI */
	HID_CFG_CMD_SET_SLOTS     = 0x03, /* value=有效挡位 1..6 */
	HID_CFG_CMD_SET_ACTIVE    = 0x04, /* value=当前挡位 0..5 */
	HID_CFG_CMD_SET_RATE      = 0x05, /* value=回报率 Hz */
	HID_CFG_CMD_SET_MODE      = 0x06, /* value=0 高速 / 1 竞技 */
	HID_CFG_CMD_SET_SYNC      = 0x07, /* value=0 关 / 1 开 */
	HID_CFG_CMD_SET_DEBOUNCE  = 0x08, /* value=0 低 / 1 中 / 2 高 */
	HID_CFG_CMD_SET_LIFTOFF   = 0x09, /* value=0 低 / 1 中 / 2 高 */
};

#define HID_CFG_DPI_SLOTS     6
#define HID_CFG_DPI_MIN       100
#define HID_CFG_DPI_MAX       16000

void hid_cfg_init(void);

/* USB 回调（hid_ops.get_report / set_report） */
int hid_cfg_get_report(uint8_t *buf, uint16_t len);
int hid_cfg_set_report(const uint8_t *buf, uint16_t len);

/* 主循环里调用：把待下发的设置推给鼠标 */
void hid_cfg_poll(void);

#endif /* HID_CFG_H__ */
