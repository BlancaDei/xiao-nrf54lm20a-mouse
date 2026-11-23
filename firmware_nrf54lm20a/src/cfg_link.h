/*
 * 配置通道（鼠标侧）：接收 dongle 通过 ESB ACK payload 下发的设置
 *
 * 链路包（8 字节，与 dongle 的 hid_cfg.c 对应）：
 *   [0]=0xC7 [1]=cmd [2]=index [3..4]=value(u16 LE) [5]=seq [6..7]=0
 *
 * ESB 事件回调里只做入队（cfg_link_on_air_packet），
 * 解析和应用都放到主循环（cfg_link_process），避免在回调里动传感器/radio。
 *
 * 设置只存在 RAM：dongle 每次链路建立后会把整套设置重发一遍，
 * 所以鼠标不需要自己掉电保存，也不存在两边配置打架的问题。
 */
#ifndef CFG_LINK_H__
#define CFG_LINK_H__

#include <stdint.h>
#include <stdbool.h>

#include <zephyr/sys/util.h>

#define CFG_LINK_DPI_SLOTS   6

/* cfg_link_process() 返回的变更位 */
#define CFG_LINK_CHG_DPI      BIT(0)
#define CFG_LINK_CHG_RATE     BIT(1)
#define CFG_LINK_CHG_MODE     BIT(2)
#define CFG_LINK_CHG_SYNC     BIT(3)
#define CFG_LINK_CHG_DEBOUNCE BIT(4)
#define CFG_LINK_CHG_LIFTOFF  BIT(5)
#define CFG_LINK_CHG_ALL      (CFG_LINK_CHG_DPI | CFG_LINK_CHG_RATE | CFG_LINK_CHG_MODE | \
			       CFG_LINK_CHG_SYNC | CFG_LINK_CHG_DEBOUNCE | CFG_LINK_CHG_LIFTOFF)

void cfg_link_init(void);

/* ESB 回调（中断/协议栈线程）：只把包塞进队列 */
void cfg_link_on_air_packet(const uint8_t *data, uint8_t len);

/* 主循环：解析队列并更新状态，返回本次累计的变更位 */
uint32_t cfg_link_process(void);

uint8_t cfg_link_slots(void);
uint8_t cfg_link_active_slot(void);
uint16_t cfg_link_dpi(uint8_t index);
uint16_t cfg_link_cpi(void);
uint16_t cfg_link_report_rate(void);
uint8_t cfg_link_system_mode(void);
bool cfg_link_motion_sync(void);
uint8_t cfg_link_debounce(void);
uint8_t cfg_link_lift_off(void);

/* 本地 DPI 键切换了档位：同步给配置状态，避免之后下发的 SET_DPI
 * 因为"挡位对不上"而不更新硬件。 */
void cfg_link_set_active_slot(uint8_t index);

#endif /* CFG_LINK_H__ */
