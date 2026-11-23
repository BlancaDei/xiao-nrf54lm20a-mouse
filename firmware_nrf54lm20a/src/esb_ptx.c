/*
 * ESB PTX 发射层 —— 实现 ble_hid.h 的同一套 API
 *
 * CONFIG_APP_ESB=y 时由 CMake 选用本文件替代 ble_hid.c。
 * main.c 不感知底层差异：同样的 init/start/stop/send_motion/send_buttons。
 *
 * 协议参数与 dongle（dongle_nrf52840/src/esb_rx.c）严格一致：
 *   2 Mbps / 信道 75 / 16bit CRC / 10 字节负载 / 地址 A5:3C:9D:71:2E
 *
 * 负载布局：[seq, btn, dxL, dxH, dyL, dyH, wheelL, wheelH, 0, batt]
 *   位移 int16 小端：高 DPI 下单包位移可超过 127，int8 会削顶。
 *   位移每 2 ms 一包，丢了靠下一包自然接上（重传无意义，旧位移已过时）；
 *   按键是状态量，每包都带最新值；滚轮改成 **16 bit 累计计数**（不是相对量），
 *   接收器用"本次计数 - 上次计数"得到增量。这样丢一包后，下一包会把漏掉的
 *   格数一起补上；重复发送同一计数也不会重复滚动。
 *
 * 发射时序（关键，esb.c 2699 行）：AUTO 模式下 esb_write_payload 入队后
 * 会自动 schedule_tx_transaction，无需也不应再调 esb_start_tx()——
 * 事务已在跑时 start_tx 只会返回 -EBUSY（无害但误导）。
 *
 * 链路活性：200 ms 内有过 TX 事件（成功或失败）即认为 dongle 在范围内；
 * 不活跃时 send_* 返回 0 直接丢弃（与 BLE 版断线时丢位移的行为一致）。
 */

#include <esb.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include <hal/nrf_radio.h>

#include "ble_hid.h"
#include "cfg_link.h"
#include "app_log.h"

/* 信道 75 = 2475 MHz：避开邻居 Wi-Fi 信道 6（2426~2448 MHz）和
 * 信道 11（2451~2473 MHz）。原信道 46（2446 MHz）正好落在信道 6 里，
 * 抓包时约 100 ms 的丢失突发与 Wi-Fi beacon 周期吻合。
 * 鼠标与接收器的信道必须一致。
 */
#define ESB_RF_CHANNEL   75
#define ESB_PAYLOAD_LEN  10
#define ESB_ADDR_BASE    { 0xA5, 0x3C, 0x9D, 0x71 }
#define ESB_ADDR_PREFIX  0x2E

static atomic_t last_tx_ms;
static atomic_t tx_ok;
static atomic_t tx_fail;
static atomic_t write_err;   /* write_payload 返回非 0（FIFO 满等） */
static bool started;
static uint8_t seq;
/* 最新按键位图。dongle 把每个包都当成一份完整 HID 报告，
 * 位移包如果按键字段写 0，长按期间的位移包会立刻把按键"松开"，
 * 主机就会判成 4~8 ms 间隔的连点。所以每个包都必须带上当前按键状态。
 */
static uint8_t cur_buttons;
/* 最新电量百分比（0xFF = 未知）。ESB 没有 BAS，搭在负载 data[9] 上
 * 随每个包回传，接收器据此做低电提示和续航记录。
 */
static uint8_t cur_battery = 0xFF;
/* 滚轮累计计数：每个包都带当前值，接收器算差值。
 * 丢包/ACK 失败都不会丢格数，下一包自然补齐；重复包差值为 0，不会重复滚。
 * 用 uint16 做模 65536 累加，回绕时的行为是定义明确的。 */
static uint16_t wheel_total;

/* 每 5 秒打印一次发射统计 */
static void stat_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
#if APP_VERBOSE_LOG
	printk("esb: PTX ok %u fail %u werr %u\n",
	       (unsigned int)atomic_get(&tx_ok),
	       (unsigned int)atomic_get(&tx_fail),
	       (unsigned int)atomic_get(&write_err));
#endif
}

static K_TIMER_DEFINE(stat_timer, stat_timer_fn, NULL);

#if APP_VERBOSE_LOG
/* 诊断：打印 PTX 端 radio 实际配置，用于与 dongle（nRF52840）逐项对比。
 * 每次调用要打两行长日志（串口上数毫秒），只能在排查时打开。 */
static void dump_radio_cfg(void)
{
	printk("esb: TXcfg BASE0=0x%08X PFX0=0x%08X TXADDR=%u RXADDRS=0x%X MODE=0x%X TXPOWER=0x%X FREQ=%u STATE=0x%02X\n",
	       (unsigned int)NRF_RADIO->BASE0,
	       (unsigned int)NRF_RADIO->PREFIX0,
	       (unsigned int)NRF_RADIO->TXADDRESS,
	       (unsigned int)NRF_RADIO->RXADDRESSES,
	       (unsigned int)NRF_RADIO->MODE,
	       (unsigned int)NRF_RADIO->TXPOWER,
	       (unsigned int)NRF_RADIO->FREQUENCY,
	       (unsigned int)NRF_RADIO->STATE);
	printk("esb: TXevt TXREADY=%u READY=%u ADDR=%u PAYLOAD=%u END=%u PHYEND=%u DISABLED=%u\n",
	       (unsigned int)NRF_RADIO->EVENTS_TXREADY,
	       (unsigned int)NRF_RADIO->EVENTS_READY,
	       (unsigned int)NRF_RADIO->EVENTS_ADDRESS,
	       (unsigned int)NRF_RADIO->EVENTS_PAYLOAD,
	       (unsigned int)NRF_RADIO->EVENTS_END,
	       (unsigned int)NRF_RADIO->EVENTS_PHYEND,
	       (unsigned int)NRF_RADIO->EVENTS_DISABLED);
}
#endif

static void esb_event_cb(const struct esb_evt *evt)
{
	switch (evt->evt_id) {
	case ESB_EVENT_TX_SUCCESS:
		if (atomic_get(&tx_ok) == 0) {
			printk("esb: first TX_SUCCESS\n");
		}
		atomic_inc(&tx_ok);
		atomic_set(&last_tx_ms, (atomic_val_t)k_uptime_get());
		break;
	case ESB_EVENT_TX_FAILED:
#if APP_VERBOSE_LOG
		if ((atomic_get(&tx_fail) % 500) == 0) {
			dump_radio_cfg();
		}
#endif
		/* ACK 没收到。关键：TX_FAILED 后库**不弹出**该包
		 * （esb.c 1847 路径无 remove_first），高频位移数据会把
		 * 8 深度 FIFO 全部塞死（实测 werr 600/s）。旧位移没有
		 * 重发价值，直接清空 FIFO，下一包立即恢复发射。
		 * 事件回调运行时 state 已是 IDLE，flush_tx 合法。 */
		atomic_inc(&tx_fail);
		atomic_set(&last_tx_ms, (atomic_val_t)k_uptime_get());
		(void)esb_flush_tx();
		break;
	case ESB_EVENT_RX_RECEIVED:
		/* PTX 正常不会收到数据包，这里收到的是 dongle 挂在 ACK 上的
		 * 配置命令（DPL 协议下 ACK payload 会推进 RX FIFO）。
		 * 必须读空 FIFO，否则后续事件不再触发。回调里只入队。 */
		{
			struct esb_payload pl;

			while (esb_read_rx_payload(&pl) == 0) {
				cfg_link_on_air_packet(pl.data, pl.length);
			}
		}
		break;
	default:
		break;
	}
}

static int esb_radio_start(void)
{
	static const uint8_t base[4] = ESB_ADDR_BASE;
	static const uint8_t prefix = ESB_ADDR_PREFIX;
	struct esb_config cfg = ESB_DEFAULT_CONFIG;
	int err;

	cfg.mode = ESB_MODE_PTX;
	cfg.tx_mode = ESB_TXMODE_AUTO;
	cfg.event_handler = esb_event_cb;
	/* 自动重传：ACK 没收到时 450 us 后重发，最多 2 次。
	 * 抓包实测无重传时会出现成批丢包（最坏 211 包/5 s ≈ 4.2%），
	 * 每次突发都对应一次明显卡顿。注意 ESB 要求 retransmit_delay
	 * 不小于 435 us（600 us 是原值，300 us 会让 esb_init 返回 -EINVAL）。
	 * 发射功率由配置通道的竞技模式（+8 dBm）在运行时设置。
	 */
	cfg.retransmit_count = 2;
	cfg.retransmit_delay = 450;
	cfg.tx_output_power = 0;

	err = esb_init(&cfg);
	if (err) {
		printk("esb: init failed %d\n", err);
		return err;
	}

	err = esb_set_base_address_0(base);
	if (err) {
		printk("esb: base addr failed %d\n", err);
		return err;
	}

	err = esb_set_prefixes(&prefix, 1);
	if (err) {
		printk("esb: prefix failed %d\n", err);
		return err;
	}

	err = esb_set_rf_channel(ESB_RF_CHANNEL);
	if (err) {
		printk("esb: channel failed %d\n", err);
		return err;
	}

	return 0;
}

int ble_hid_init(void)
{
	int err = esb_radio_start();

	if (err == 0) {
		started = true;
		k_timer_start(&stat_timer, K_SECONDS(5), K_SECONDS(5));
		printk("esb: PTX started ch=%u addr=A5:3C:9D:71:%02X 2Mbps\n",
		       ESB_RF_CHANNEL, ESB_ADDR_PREFIX);
#if APP_VERBOSE_LOG
		dump_radio_cfg();
#endif
	}
	return err;
}

bool ble_hid_connected(void)
{
	/* ESB 没有连接态：PRX 一直挂在接收，发射器启动就算链路可用。
	 * 原来用"200 ms 内有过 TX"判定，静止时鼠标不发包 -> 判定为断开，
	 * main.c 会把点击先扣在 btn_latch 里，要等下一次位移才补发，
	 * 表现为"静止一会儿后的第一下点击慢半拍甚至要晃一下才生效"。 */
	return started;
}

int ble_hid_send_motion(int16_t dx, int16_t dy)
{
	struct esb_payload pl = { .length = ESB_PAYLOAD_LEN };
	int err;

	pl.data[0] = seq++;
	pl.data[1] = cur_buttons;
	pl.data[2] = (uint8_t)((uint16_t)dx & 0xFF);
	pl.data[3] = (uint8_t)(((uint16_t)dx >> 8) & 0xFF);
	pl.data[4] = (uint8_t)((uint16_t)dy & 0xFF);
	pl.data[5] = (uint8_t)(((uint16_t)dy >> 8) & 0xFF);
	/* 位移包也带累计滚轮计数：之前只放在按键包里，
	 * 一旦那个按键包被丢，滚轮就永久丢了。 */
	pl.data[6] = (uint8_t)(wheel_total & 0xFF);
	pl.data[7] = (uint8_t)((wheel_total >> 8) & 0xFF);
	pl.data[8] = 0;
	pl.data[9] = cur_battery;

	err = esb_write_payload(&pl);
	if (err) {
		atomic_inc(&write_err);
	}

	/* 把错误如实返回：main.c 只有拿到 0 才会扣减 pend，
	 * 这里若吞掉 -ENOMEM（TX FIFO 满），这段位移就永久丢了。 */
	return err;
}

int ble_hid_send_buttons(uint8_t buttons, int8_t wheel, int8_t pan)
{
	struct esb_payload pl = { .length = ESB_PAYLOAD_LEN };
	uint16_t new_total = (uint16_t)(wheel_total + (uint16_t)(int16_t)wheel);
	int err;

	ARG_UNUSED(pan);

	/* 状态量：无论本次发送成功与否都记录下来，
	 * 后面每个位移包都会带上它，丢一包也能在下个包自愈 */
	cur_buttons = buttons;

	pl.data[0] = seq++;
	pl.data[1] = buttons;
	pl.data[2] = 0;
	pl.data[3] = 0;
	pl.data[4] = 0;
	pl.data[5] = 0;
	pl.data[6] = (uint8_t)(new_total & 0xFF);
	pl.data[7] = (uint8_t)((new_total >> 8) & 0xFF);
	pl.data[8] = 0;
	pl.data[9] = cur_battery;

	err = esb_write_payload(&pl);
	if (err) {
		atomic_inc(&write_err);
	} else {
		/* 只有真正进了 radio FIFO 才推进累计值：
		 * 写失败时 main.c 会保留 wheel_pending，下一轮用同一个基数重试。 */
		wheel_total = new_total;
	}

	/* 返回错误让 main.c 保留 wheel_pending 重试。按键本身是状态量，
	 * 已经写进 cur_buttons，后续位移包会自动带上，不会丢。 */
	return err;
}

void ble_hid_stop(void)
{
	if (started) {
		(void)esb_disable();
		/* 休眠/关机期间不需要发射统计，停掉 5 s 周期定时器，
		 * 避免它每隔 5 s 把 CPU 从空闲里唤醒一次。 */
		k_timer_stop(&stat_timer);
		cur_buttons = 0;   /* 断电/休眠时主机侧按键状态归零 */
		atomic_set(&last_tx_ms, 0);
	}
}

void ble_hid_start(void)
{
	if (!started) {
		(void)ble_hid_init();
		return;
	}

	(void)esb_disable();
	if (esb_radio_start() == 0) {
		k_timer_start(&stat_timer, K_SECONDS(5), K_SECONDS(5));
		printk("esb: PTX restarted\n");
	}
}

/*
 * 系统模式：
 *   高速（0）   -> 0 dBm，续航优先（原来就是这个值）
 *   竞技（1）   -> +8 dBm，链路更稳，耗电明显增加
 * 只改发射功率：传感器扫描率由 PAW3395 自身配置决定，这里没有可调项。
 */
void ble_hid_set_system_mode(uint8_t mode)
{
	int8_t dbm = (mode != 0) ? 8 : 0;
	int err = esb_set_tx_power(dbm);

	if (err) {
		printk("esb: tx power %d dBm failed (%d), fallback 4 dBm\n", dbm, err);
		dbm = 4;
		(void)esb_set_tx_power(dbm);
	}

	printk("esb: system mode %s, tx %d dBm\n",
	       (mode != 0) ? "competitive" : "high-speed", dbm);
}

void ble_hid_set_battery(uint8_t pct)
{
	cur_battery = (pct > 100) ? 0xFF : pct;
}
