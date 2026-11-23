/*
 * XIAO 2.4G dongle —— ProMicro nRF52840
 *
 * 阶段 A：USB HID 鼠标 + CDC 控制台复合设备（已验证：枚举 + 测试信号端到端）
 * 阶段 B：ESB PRX（2.4G 接收）——鼠标端 PTX 发包，这里收包后提交 HID
 *
 * 报告布局（Report ID 1，6 字节数据）：
 *   [0] Report ID（0x01，由本文件在提交时补上）
 *   [1] 按键位图（bit0=左键, bit1=右键, bit2=中键）
 *   [2..3] dx (int16 LE)
 *   [4..5] dy (int16 LE)
 *   [6] wheel (int8)
 *
 * 位移用 16 位：高 DPI 下单包位移经常超过 127，8 位会把移动"削平"，
 * 表现为甩不动、手感发涩（实测 4800 DPI 必现）。
 *
 * 注意：厂商配置通道用了 Feature Report ID 0x06，按 HID 规范只要出现
 * Report ID，所有报告都必须带 ID，因此鼠标报告也从"无 ID"改成了 ID 1
 * （无 ID + 有 ID 混用时 Windows 会直接报 Code 10，HID 驱动起不来）。
 *
 * 链路状态指示（led0 红灯）：
 *   收包中 = 常亮；静默超过 200 ms = 熄灭
 */

#include <sample_usbd.h>

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/printk.h>

#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>

#if defined(CONFIG_APP_ESB)
#include "esb_rx.h"
#else
#include "ble_ctr.h"
#endif

#include "hid_cfg.h"

#ifndef APP_VERBOSE_LOG
#define APP_VERBOSE_LOG 0
#endif

#if defined(CONFIG_APP_ESB)
static int ctl_init(void)
{
	return esb_rx_init();
}

static void ctl_stats(uint32_t *total, uint32_t *err)
{
	esb_rx_stats(total, err);
}

static int64_t ctl_last_ms(void)
{
	return esb_rx_last_ms();
}

static int ctl_battery_level(void)
{
	/* ESB 没有 BAS，电量由鼠标搭在数据包 data[9] 上回传 */
	return esb_rx_battery_level();
}
#else
static int ctl_init(void)
{
	return ble_ctr_init();
}

static void ctl_stats(uint32_t *total, uint32_t *err)
{
	ble_ctr_stats(total, err);
}

static int64_t ctl_last_ms(void)
{
	return ble_ctr_last_ms();
}

static int ctl_battery_level(void)
{
	return ble_ctr_battery_level();
}
#endif

#if !defined(CONFIG_APP_ESB)
static const struct gpio_dt_spec led =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#endif

/* 报告描述符：标准鼠标 + 厂商配置通道。
 *
 * 鼠标输入报告用 **Report ID 1**（厂商 Feature Report 是 ID 6）。
 * HID 规范要求：描述符里一旦出现 Report ID，所有报告都必须带 ID，
 * 否则 Windows 直接把 HID 设备判成 Code 10。所以提交报告时要先补一个
 * 0x01 字节，见主循环里的 hid_device_submit_report()。
 *
 * 内容与 Zephyr 的 HID_MOUSE_REPORT_DESC(5) 完全一致（5 键 + 3 位填充），
 * 只是多了一行 Report ID，无法直接复用那个宏（它自带花括号，不能追加）。
 *
 * 后半段是 Feature Report ID 0x06（32 字节），协议见 hid_cfg.h，
 * PC 驱动用 HidD_GetFeature/SetFeature 读写。
 */
#define HID_REPORT_ID_MOUSE  0x01
static const uint8_t hid_report_desc[] = {
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_MOUSE),
	HID_COLLECTION(HID_COLLECTION_APPLICATION),
		0x85, HID_REPORT_ID_MOUSE,   /* Report ID (1) —— 见文件头说明 */
		HID_USAGE(HID_USAGE_GEN_DESKTOP_POINTER),
		HID_COLLECTION(HID_COLLECTION_PHYSICAL),
			HID_USAGE_PAGE(HID_USAGE_GEN_BUTTON),
			HID_USAGE_MIN8(1),
			HID_USAGE_MAX8(5),
			HID_LOGICAL_MIN8(0),
			HID_LOGICAL_MAX8(1),
			HID_REPORT_SIZE(1),
			HID_REPORT_COUNT(5),
			HID_INPUT(0x02),
			HID_REPORT_SIZE(3),
			HID_REPORT_COUNT(1),
			HID_INPUT(1),
			HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
			HID_USAGE(HID_USAGE_GEN_DESKTOP_X),
			HID_USAGE(HID_USAGE_GEN_DESKTOP_Y),
			HID_LOGICAL_MIN16(0x01, 0x80),   /* -32767 */
			HID_LOGICAL_MAX16(0xFF, 0x7F),   /*  32767 */
			HID_REPORT_SIZE(16),
			HID_REPORT_COUNT(2),
			HID_INPUT(0x06),
			HID_USAGE(HID_USAGE_GEN_DESKTOP_WHEEL),
			HID_LOGICAL_MIN8(-127),
			HID_LOGICAL_MAX8(127),
			HID_REPORT_SIZE(8),
			HID_REPORT_COUNT(1),
			HID_INPUT(0x06),
		HID_END_COLLECTION,
	HID_END_COLLECTION,

	/* ---- 厂商配置通道：Feature Report ID 0x06，32 字节 ---- */
	0x06, 0x00, 0xFF,          /* Usage Page (Vendor Defined 0xFF00) */
	0x09, 0x01,                /* Usage (0x01) */
	0xA1, 0x01,                /* Collection (Application) */
	0x85, HID_CFG_REPORT_ID,   /*   Report ID (6) */
	0x09, 0x02,                /*   Usage (0x02) */
	0x15, 0x00,                /*   Logical Minimum (0) */
	0x26, 0xFF, 0x00,          /*   Logical Maximum (255) */
	0x75, 0x08,                /*   Report Size (8) */
	0x95, HID_CFG_REPORT_LEN,  /*   Report Count (32) */
	0xB1, 0x02,                /*   Feature (Data,Var,Abs) */
	0xC0,                      /* End Collection */
};

enum report_idx {
	REP_BTN = 0,
	REP_XL,
	REP_XH,
	REP_YL,
	REP_YH,
	REP_WHEEL,
	REP_COUNT,
};

/* 上报队列：ESB RX 回调（中断上下文）填充，主循环消费。
 * 1 kHz 下 32 ms 缓冲，满了丢旧包（位移只关心最新状态） */
K_MSGQ_DEFINE(rep_q, REP_COUNT, 32, 1);

/* USB 回调（中断/系统线程上下文）写、主循环读，必须 volatile，
 * 否则优化后主循环可能一直看到缓存的 false 而丢掉所有报告。 */
static volatile bool hid_ready;

/* 电量低于该值就呼吸红灯（鼠标现在始终回传真实电量） */
#define LOW_BATTERY_PCT 25

#if defined(CONFIG_APP_ESB)
/* 低电呼吸灯：pwm-leds 跑在独立线程，避免占住 1 kHz 主循环。
 * 三角波 0→100→0，周期 2 s，每 20 ms 一步；电量正常时保持全灭。
 *
 * 两处防抖，避免"还没到 25% 就开始亮"：
 *   LOW_BATTERY_CONFIRM_MS：电量 ≤25% 要连续保持 15 s 才点亮
 *                           （鼠标 5 s 报一次，即 3 次读数确认，滤掉负载压降）
 *   LOW_BATTERY_HYST      ：点亮后要回到 27% 以上才熄灭，防止临界值闪烁
 */
#define BREATH_STEP_MS 20
#define BREATH_PERIOD_MS 2000
#define LOW_BATTERY_CONFIRM_MS 15000
#define LOW_BATTERY_HYST 2

/* pwm-leds 的设备节点是父节点 /pwmleds，子节点只是通道；
 * 别名指向子节点，所以要往上取一层，通道号按子节点序号。
 */
static const struct device *const batt_led =
	DEVICE_DT_GET(DT_PARENT(DT_ALIAS(pwm_led0)));
#define BATT_LED_CHANNEL DT_NODE_CHILD_IDX(DT_ALIAS(pwm_led0))

static void batt_led_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	if (!device_is_ready(batt_led)) {
		printk("batt led not ready\n");
		return;
	}

	uint32_t phase = 0;
	int64_t low_since = 0;
	bool low_lit = false;

	while (true) {
		int pct = ctl_battery_level();

		/* 点亮门槛 25%，熄灭门槛 27%（已点亮时） */
		int off_above = low_lit ? (LOW_BATTERY_PCT + LOW_BATTERY_HYST)
				       : LOW_BATTERY_PCT;
		bool low = (pct > 0 && pct <= off_above);

		if (!low) {
			(void)led_set_brightness(batt_led, BATT_LED_CHANNEL, 0);
			phase = 0;
			low_since = 0;
			low_lit = false;
			k_msleep(200);
			continue;
		}

		if (!low_lit) {
			int64_t now = k_uptime_get();

			if (low_since == 0) {
				low_since = now;
			}
			if ((now - low_since) < LOW_BATTERY_CONFIRM_MS) {
				(void)led_set_brightness(batt_led, BATT_LED_CHANNEL, 0);
				phase = 0;
				k_msleep(BREATH_STEP_MS);
				continue;
			}
			low_lit = true;
		}

		uint32_t half = BREATH_PERIOD_MS / 2;
		uint8_t bright = (uint8_t)((phase < half)
			? (phase * 100 / half)
			: ((BREATH_PERIOD_MS - phase) * 100 / half));

		(void)led_set_brightness(batt_led, BATT_LED_CHANNEL, bright);
		phase = (phase + BREATH_STEP_MS) % BREATH_PERIOD_MS;
		k_msleep(BREATH_STEP_MS);
	}
}

K_THREAD_DEFINE(batt_led_tid, 1536, batt_led_thread, NULL, NULL, NULL, 7, 0, 0);
#endif

/* 链路空闲判定：超过该时长没收到包就算空闲（只用于打印，不再驱动灯）。
 * 鼠标静止时只有 500 ms 一次的保活包，窗口必须比它大，否则日志每几秒
 * 就 link up/link idle 刷一对。 */
#define LINK_IDLE_MS 1500

/* ---- HID 回调 ---- */
static void iface_ready(const struct device *dev, const bool ready)
{
	printk("usb: HID iface %s\n", ready ? "ready" : "not ready");
	hid_ready = ready;
}

static int get_report(const struct device *dev,
		      const uint8_t type, const uint8_t id, const uint16_t len,
		      uint8_t *const buf)
{
	ARG_UNUSED(dev);

	if (type == HID_REPORT_TYPE_FEATURE && id == HID_CFG_REPORT_ID) {
		return hid_cfg_get_report(buf, len);
	}

	return -ENOTSUP;
}

static int set_report(const struct device *dev,
		      const uint8_t type, const uint8_t id, const uint16_t len,
		      const uint8_t *const buf)
{
	ARG_UNUSED(dev);

	if (type == HID_REPORT_TYPE_FEATURE && id == HID_CFG_REPORT_ID) {
		return hid_cfg_set_report(buf, len);
	}

	return -ENOTSUP;
}

static const struct hid_device_ops hid_ops = {
	.iface_ready = iface_ready,
	.get_report = get_report,
	.set_report = set_report,
};

int main(void)
{
	struct usbd_context *usbd;
	const struct device *hid_dev;
	int ret;

#if defined(CONFIG_APP_ESB)
	printk("\n=== XIAO 2.4G dongle (ESB PRX + USB HID) ===\n");
#else
	printk("\n=== XIAO 2.4G dongle (BLE central + USB HID) ===\n");
#endif
	printk("build: %s %s\n\n", __DATE__, __TIME__);

#if !defined(CONFIG_APP_ESB)
	if (!gpio_is_ready_dt(&led)) {
		printk("led not ready\n");
		return 0;
	}
	(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
#endif

	ret = ctl_init();
	if (ret != 0) {
		printk("link init failed (%d), continuing USB-only\n", ret);
	}

	hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
	if (!device_is_ready(hid_dev)) {
		printk("HID device not ready\n");
		return -EIO;
	}

	/* 配置通道要早于 USB 使能：主机枚举完就可能立刻读 Feature Report */
	hid_cfg_init();

	ret = hid_device_register(hid_dev,
				  hid_report_desc, sizeof(hid_report_desc),
				  &hid_ops);
	if (ret != 0) {
		printk("hid_device_register failed (%d)\n", ret);
		return ret;
	}

	usbd = sample_usbd_init_device(NULL);
	if (usbd == NULL) {
		printk("usbd init failed\n");
		return -ENODEV;
	}

	ret = usbd_enable(usbd);
	if (ret != 0) {
		printk("usbd_enable failed (%d)\n", ret);
		return ret;
	}

#if defined(CONFIG_APP_ESB)
	printk("usb: started, waiting for ESB packets\n");
#else
	printk("usb: started, waiting for BLE HID notifications\n");
#endif

	uint32_t sent_total = 0;
	uint32_t hid_busy = 0;
	uint32_t usb_btn = 0;   /*  Diagnostics: 提交到 USB 的"有按键"报告数 */
	uint32_t rx_total = 0;
	uint32_t rx_err = 0;
	uint32_t rx_win0 = 0;
	int64_t stat_t0 = k_uptime_get();
	bool link_fresh = false;
	int64_t led_t0 = 0;
	bool led_on = false;
	/* USB 侧没送出去的滚轮格数。USB 每 1 ms 只取最新一份报告，
	 * 提交失败时如果不补偿，这一格滚轮就永久丢了。
	 * 下一份报告把欠的格数一起带上（饱和到 ±127，余量继续留）。 */
	int32_t wheel_retry = 0;

	while (true) {
		uint8_t rep[REP_COUNT];

		/* 把 PC 端改过的设置推给鼠标（内部按 20 ms 节流，链路断开时自动挂起） */
		hid_cfg_poll();

		/* 100 ms 超时兜底：链路静默时也要维护灯和统计 */
		if (k_msgq_get(&rep_q, rep, K_MSEC(100)) == 0) {
			if (!hid_ready) {
				/* USB 还没枚举好，报告被丢弃；滚轮先记下来 */
				wheel_retry = CLAMP(wheel_retry + (int8_t)rep[REP_WHEEL],
						    -32767, 32767);
			} else {
				uint8_t report[REP_COUNT + 1];
				int32_t w = (int8_t)rep[REP_WHEEL] + wheel_retry;
				int32_t w_send = CLAMP(w, -127, 127);

				/* 描述符里用了 Report ID，所以每条报告首字节都要带 ID */
				report[0] = HID_REPORT_ID_MOUSE;
				memcpy(&report[1], rep, REP_COUNT);
				report[1 + REP_WHEEL] = (uint8_t)(int8_t)w_send;

				ret = hid_device_submit_report(hid_dev, sizeof(report), report);
				if (ret != 0) {
					hid_busy++;
					/* 提交失败：这份滚轮没送到主机，累计到下一次 */
					wheel_retry = CLAMP(wheel_retry +
							    (int8_t)rep[REP_WHEEL],
							    -32767, 32767);
				} else {
					sent_total++;
					/* 成功：只扣掉实际送出的部分，饱和的余量留下 */
					wheel_retry = CLAMP(w - w_send, -32767, 32767);
					if (rep[0] & 0x1f) {
						usb_btn++;
					}
				}
			}
		}

		/* ---- 链路状态 + 统计（每 100 ms 维护一次） ---- */
		uint32_t now_total;
		uint32_t now_err;

		ctl_stats(&now_total, &now_err);
		rx_total = now_total;
		rx_err = now_err;

		bool fresh = (rx_total > 0) &&
			     ((k_uptime_get() - ctl_last_ms()) <= LINK_IDLE_MS);

		if (fresh != link_fresh) {
			link_fresh = fresh;
			printk("link %s\n", fresh ? "up" : "idle");
		}

		/* LED：ESB 变体由 batt_led_thread 做低电呼吸；
		 * 这里只维护统计用的 led_on 状态。BLE 变体仍用 GPIO 常亮。
		 */
		if ((k_uptime_get() - led_t0) >= 1000) {
			int pct = ctl_battery_level();

			led_on = (pct > 0 && pct <= LOW_BATTERY_PCT);
#if !defined(CONFIG_APP_ESB)
			(void)gpio_pin_set_dt(&led, led_on);
#endif
			led_t0 = k_uptime_get();
		}

		if ((k_uptime_get() - stat_t0) >= 5000) {
#if defined(CONFIG_APP_ESB)
#if APP_VERBOSE_LOG
			printk("esb: rx %u in 5 s (tot %u, err %u) | hid sent %u (busy %u) | btn usb %u | bat %d%% led %d\n",
			       (unsigned int)(rx_total - rx_win0), rx_total, rx_err,
			       sent_total, hid_busy, (unsigned int)usb_btn,
			       ctl_battery_level(), led_on ? 1 : 0);
#endif
#else
			uint32_t sec_ok, sec_err, disc_ok, sub_ok, disc_evt;
			uint32_t bdisc, bfound, bchr, brok, brerr;

			ble_ctr_diag(&sec_ok, &sec_err, &disc_ok, &sub_ok, &disc_evt);
			ble_ctr_bas_diag(&bdisc, &bfound, &bchr, &brok, &brerr);
#if APP_VERBOSE_LOG
			printk("ble: rx %u in 5 s (tot %u, err %u) | hid sent %u (busy %u) | btn rx %u usb %u | bat %d%% led %d | sec %u/%u disc %u sub %u dc %u | bas %u/%u/%u/%u/%u\n",
			       (unsigned int)(rx_total - rx_win0), rx_total, rx_err,
			       sent_total, hid_busy,
			       (unsigned int)ble_ctr_btn_count(), (unsigned int)usb_btn,
			       ble_ctr_battery_level(), led_on ? 1 : 0,
			       (unsigned int)sec_ok, (unsigned int)sec_err,
			       (unsigned int)disc_ok, (unsigned int)sub_ok,
			       (unsigned int)disc_evt,
			       (unsigned int)bdisc, (unsigned int)bfound,
			       (unsigned int)bchr, (unsigned int)brok,
			       (unsigned int)brerr);
#endif
#endif
			stat_t0 = k_uptime_get();
			rx_win0 = rx_total;
		}
	}

	return 0;
}
