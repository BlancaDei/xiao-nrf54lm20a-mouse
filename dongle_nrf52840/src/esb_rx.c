/*
 * ESB PRX 接收层实现（dongle 侧，NCS v3.4 ESB/MPSL API）
 *
 * 收包在 ESB 事件回调（中断上下文）里解析后塞进 rep_q，
 * main 循环里取出来提交 HID。位移不做累计——ESB 是 1 ms 级链路，
 * 丢包直接丢（下一段位移自然接上），这与 BLE 的"攒着补发"策略不同。
 *
 * 【诊断版】每 5 秒打印 NRF_RADIO->STATE，验证 PRX 的 radio 是否真的
 * 处于接收状态（STATE: 0=DISABLED 1=RXRU 2=RXIDLE 3=RX 4=TXRU 5=TXIDLE 6=TX）。
 */

#include <esb.h>

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <nrfx.h>

#include "esb_rx.h"

/* 诊断打印开关（与 main.c 的 APP_VERBOSE_LOG 同名，默认关闭）。
 * 下面的 5 s radio 寄存器转储和 rssi_live() 会直接操作 NRF_RADIO：
 * 在 PRX 正在收包时反复 RSSISTART 会干扰接收，正式固件必须关掉。 */
#ifndef APP_VERBOSE_LOG
#define APP_VERBOSE_LOG 0
#endif

/* 与 main.c 共享的 HID 报告队列（6 字节：btn, dxL, dxH, dyL, dyH, wheel） */
extern struct k_msgq rep_q;

static atomic_t rx_total;
static atomic_t rx_err;
static atomic_t rx_gap;
static atomic_t last_rx_ms;

/* 鼠标电量：搭在负载 data[9] 上随包回传，0 = 未知 */
static atomic_t batt_pct = ATOMIC_INIT(0);
static atomic_t batt_ms;

#define BATT_STALE_MS 30000

static uint8_t expected_seq;

/* 滚轮累计计数解算：鼠标每包带 16 bit wheel_total，这里算增量。
 * 超过 WHEEL_RESYNC_MS 没收到包（鼠标睡着/重启/换电池）就重新对齐，
 * 避免上电瞬间把旧计数差当成一次大滚动。 */
#define WHEEL_RESYNC_MS 1500
static uint16_t wheel_last;   /* 模 65536，回绕定义明确 */
static bool wheel_synced;

#if APP_VERBOSE_LOG
static uint8_t g_selftest[80];
static uint8_t g_selftest_min;
static uint8_t g_selftest_min_ch;

/* 现场重新采样一次 ch46 的 RSSI（避开开机日志被丢弃的问题） */
static uint8_t rssi_live(void)
{
	uint8_t mn = 0xFF;

	for (int i = 0; i < 20; i++) {
		NRF_RADIO->TASKS_RSSISTART = 1;
		k_busy_wait(10);
		uint8_t v = (uint8_t)NRF_RADIO->RSSISAMPLE;

		if ((v != 0) && (v < mn)) {
			mn = v;
		}
	}
	return mn;
}

/*
 * 诊断：裸 radio 接收自检（完全脱离 ESB）。
 * 把 radio 置于原始 RX，扫 0~79 信道，记录每个信道的最强 RSSI 采样。
 * RSSISAMPLE 读数是 |dBm|（40 = -40 dBm），越小越强，0 视为无效。
 * 环境里必有 Wi-Fi，若全频段都停在 ~100 说明接收前端没工作。
 */
static void radio_rx_selftest(void)
{
	uint8_t mn_all = 0xFF;
	uint8_t ch_all = 0;

	NRF_RADIO->TASKS_DISABLE = 1;
	NRF_RADIO->POWER = 1;
	NRF_RADIO->MODE = 1;          /* Nrf_2Mbit，RSSI 测量与速率无关 */
	NRF_RADIO->PCNF0 = 0;
	NRF_RADIO->PCNF1 = 0;
	NRF_RADIO->RXADDRESSES = 0;
	NRF_RADIO->CRCCNF = 0;        /* 关 CRC，只测能量 */
	NRF_RADIO->SHORTS = 0;
	NRF_RADIO->EVENTS_READY = 0;
	NRF_RADIO->TASKS_RXEN = 1;
	k_busy_wait(200);

	for (int ch = 0; ch < 80; ch++) {
		uint8_t mn = 0xFF;

		NRF_RADIO->FREQUENCY = (uint32_t)ch;
		k_busy_wait(150);

		for (int i = 0; i < 20; i++) {
			NRF_RADIO->TASKS_RSSISTART = 1;
			k_busy_wait(10);
			uint8_t v = (uint8_t)NRF_RADIO->RSSISAMPLE;

			if ((v != 0) && (v < mn)) {
				mn = v;
			}
		}
		g_selftest[ch] = mn;

		if (mn < mn_all) {
			mn_all = mn;
			ch_all = (uint8_t)ch;
		}
	}

	NRF_RADIO->TASKS_DISABLE = 1;

	g_selftest_min = mn_all;
	g_selftest_min_ch = ch_all;

	printk("rf: selftest min=%u@ch%u | ch0=%u ch20=%u ch37=%u ch46=%u ch60=%u ch79=%u\n",
	       (unsigned int)mn_all, (unsigned int)ch_all,
	       (unsigned int)g_selftest[0], (unsigned int)g_selftest[20],
	       (unsigned int)g_selftest[37], (unsigned int)g_selftest[46],
	       (unsigned int)g_selftest[60], (unsigned int)g_selftest[79]);
}

/* 每 5 秒打印 radio STATE（定时器回调 = 系统时钟上下文，printk 安全） */
static void stat_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	printk("esb: PRX rx %u total, gap %u, err %u radio STATE=0x%02X ADDR=%u CRCOK=%u CRCERR=%u END=%u RSSI=%u\n",
	       (unsigned int)atomic_get(&rx_total),
	       (unsigned int)atomic_get(&rx_gap),
	       (unsigned int)atomic_get(&rx_err),
	       (unsigned int)NRF_RADIO->STATE,
	       (unsigned int)NRF_RADIO->EVENTS_ADDRESS,
	       (unsigned int)NRF_RADIO->EVENTS_CRCOK,
	       (unsigned int)NRF_RADIO->EVENTS_CRCERROR,
	       (unsigned int)NRF_RADIO->EVENTS_END,
	       (unsigned int)NRF_RADIO->RSSISAMPLE);
	printk("rf: selftest min=%u@ch%u | ch0=%u ch20=%u ch37=%u ch46=%u ch60=%u ch79=%u | live(ch46)=%u\n",
	       (unsigned int)g_selftest_min, (unsigned int)g_selftest_min_ch,
	       (unsigned int)g_selftest[0], (unsigned int)g_selftest[20],
	       (unsigned int)g_selftest[37], (unsigned int)g_selftest[46],
	       (unsigned int)g_selftest[60], (unsigned int)g_selftest[79],
	       (unsigned int)rssi_live());
	printk("esb: rx radio FREQ=%u MODE=0x%X PCNF0=0x%08X PCNF1=0x%08X CRCCNF=0x%X POLY=0x%X INIT=0x%X BASE0=0x%08X PFX0=0x%08X RXADDRS=0x%X\n",
	       (unsigned int)NRF_RADIO->FREQUENCY,
	       (unsigned int)NRF_RADIO->MODE,
	       (unsigned int)NRF_RADIO->PCNF0,
	       (unsigned int)NRF_RADIO->PCNF1,
	       (unsigned int)NRF_RADIO->CRCCNF,
	       (unsigned int)NRF_RADIO->CRCPOLY,
	       (unsigned int)NRF_RADIO->CRCINIT,
	       (unsigned int)NRF_RADIO->BASE0,
	       (unsigned int)NRF_RADIO->PREFIX0,
	       (unsigned int)NRF_RADIO->RXADDRESSES);
}

static K_TIMER_DEFINE(stat_timer, stat_timer_fn, NULL);
#endif /* APP_VERBOSE_LOG */

static void parse_payload(const struct esb_payload *pl)
{
	uint8_t rep[6];
	int64_t now;
	int64_t prev;
	int32_t wheel_delta = 0;
	uint16_t wheel_total;

	if (pl->length != ESB_PAYLOAD_LEN) {
		atomic_inc(&rx_err);
		return;
	}

	now = k_uptime_get();
	prev = (int64_t)atomic_get(&last_rx_ms);

	/* data[6..7] = 鼠标端滚轮累计计数（uint16 LE），不是相对量。
	 * 丢包后下一包会把漏掉的格数一起带过来；重复包差值为 0。 */
	wheel_total = (uint16_t)(pl->data[6] | (pl->data[7] << 8));
	if (!wheel_synced || prev == 0 || (now - prev) > WHEEL_RESYNC_MS) {
		wheel_last = wheel_total;
		wheel_synced = true;
	} else {
		int32_t d = (int16_t)(wheel_total - wheel_last);

		wheel_delta = CLAMP(d, -127, 127);
		/* 只推进实际发出的量，超出 ±127 的部分留给下一包。
		 * 用 uint16 回绕，避免累计到边界时出现有符号溢出。 */
		wheel_last = (uint16_t)(wheel_last + wheel_delta);
	}

	rep[0] = pl->data[1];                 /* btn */
	rep[1] = pl->data[2];                 /* dx 低字节 */
	rep[2] = pl->data[3];                 /* dx 高字节 */
	rep[3] = pl->data[4];                 /* dy 低字节 */
	rep[4] = pl->data[5];                 /* dy 高字节 */
	rep[5] = (uint8_t)(int8_t)wheel_delta; /* 本次应滚动的格数 */

	/* 丢包只计数，不打印——1 kHz 下每包一行 printk 会拖死中断 */
	if (pl->data[0] != expected_seq) {
		atomic_inc(&rx_gap);
	}
	expected_seq = (uint8_t)(pl->data[0] + 1);

	atomic_inc(&rx_total);
	atomic_set(&last_rx_ms, (atomic_val_t)now);

	/* data[9] = 电量百分比（1..100）。0/0xFF 视为未知，避免旧固件
	 * 全 0 负载被误判成 0% 而点亮低电红灯。 */
	if (pl->data[9] > 0 && pl->data[9] <= 100) {
		atomic_set(&batt_pct, pl->data[9]);
		atomic_set(&batt_ms, (atomic_val_t)k_uptime_get());
	}

	/* 队列满就丢最旧的包——鼠标位移只关心最新状态 */
	if (k_msgq_put(&rep_q, rep, K_NO_WAIT) != 0) {
		uint8_t drop[6];

		(void)k_msgq_get(&rep_q, drop, K_NO_WAIT);
		(void)k_msgq_put(&rep_q, rep, K_NO_WAIT);
	}
}

static void esb_event_cb(const struct esb_evt *evt)
{
	struct esb_payload pl;

	switch (evt->evt_id) {
	case ESB_EVENT_RX_RECEIVED:
		while (esb_read_rx_payload(&pl) == 0) {
			parse_payload(&pl);
		}
		break;
	case ESB_EVENT_TX_SUCCESS:
	case ESB_EVENT_TX_FAILED:
		/* PRX 不会发这些事件（未启用 ACK payload） */
		break;
	default:
		printk("esb: evt %d\n", evt->evt_id);
		break;
	}
}

void esb_rx_stats(uint32_t *total, uint32_t *err)
{
	*total = atomic_get(&rx_total);
	*err = atomic_get(&rx_err);
}

int64_t esb_rx_last_ms(void)
{
	return (int64_t)atomic_get(&last_rx_ms);
}

int esb_rx_battery_level(void)
{
	int pct = (int)atomic_get(&batt_pct);
	int64_t t = (int64_t)atomic_get(&batt_ms);

	if (pct <= 0) {
		return -1;
	}
	if ((k_uptime_get() - t) > BATT_STALE_MS) {
		return -1;
	}
	return pct;
}

/*
 * PRX 方向的 ACK payload：鼠标（PTX）每发一个包都会请求 ACK，
 * 这里塞进队列的数据就搭在 ACK 上回给鼠标。
 * 队列深度有限（CONFIG_ESB_ACK_FIFO_SIZE），满了返回 -ENOMEM，
 * 上层会重发，不需要在这里重试。
 */
int esb_rx_cfg_send(const uint8_t *data, uint8_t len)
{
	struct esb_payload pl = { .length = len };

	if (len > sizeof(pl.data)) {
		return -EINVAL;
	}

	memcpy(pl.data, data, len);

	return esb_write_payload(&pl);
}

int esb_rx_init(void)
{
	static const uint8_t base[4] = ESB_ADDR_BASE;
	static const uint8_t prefix = ESB_ADDR_PREFIX;
	struct esb_config cfg = ESB_DEFAULT_CONFIG;
	int err;

#if APP_VERBOSE_LOG
	/* 诊断：先做裸 radio 接收自检（此时 ESB 尚未接管 radio）。
	 * 会扫全频段，约 30 ms，正式固件不跑。 */
	radio_rx_selftest();
#endif

	cfg.mode = ESB_MODE_PRX;
	cfg.event_handler = esb_event_cb;
	cfg.retransmit_count = 2;   /* PRX 不重传，仅占位 */
	cfg.retransmit_delay = 600;
	cfg.tx_output_power = 0;    /* RX-only，无意义，占位 */
	cfg.selective_auto_ack = false;

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

	err = esb_start_rx();
	if (err) {
		printk("esb: start_rx failed %d\n", err);
		return err;
	}

#if APP_VERBOSE_LOG
	/* 诊断：每次地址匹配自动测 RSSI（不干扰 ESB 自身 shorts） */
	NRF_RADIO->SHORTS |= NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK;

	k_timer_start(&stat_timer, K_SECONDS(5), K_SECONDS(5));
#endif

	printk("esb: PRX started ch=%u addr=A5:3C:9D:71:%02X 2Mbps\n",
	       ESB_RF_CHANNEL, ESB_ADDR_PREFIX);

	return 0;
}
