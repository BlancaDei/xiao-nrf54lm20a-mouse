/*
 * XIAO nRF54LM20A 游戏鼠标固件
 *   阶段 1：RGB LED 心跳 + PAW3395 传感器探测
 *   阶段 2：BLE HID 鼠标（HID over GATT）+ LLPM
 *   阶段 3：PAW3395 接线上机 + CPI 校准
 *
 * 阶段的目的是把传感器调通，所以：
 *   - 开机先 dump 传感器寄存器 + 打印分辨率与 AWAKE 位
 *   - 位移走 motion burst（0x16，一窗 12 字节，约 180 us），上报 1 kHz：
 *     有位移时主循环自由跑（不再 k_msleep，避免 1.2 ms 的节拍量化），
 *     空闲时挂 MOTION 中断的信号量，最多等 1 ms。上报走 pend_* 攒-发，
 *     拥塞时也不丢位移
 *   - 状态行每 3 s 打印一次，带**累计计数** counts x/y：
 *     拿尺子推传感器走一段已知距离，用首尾计数差算出真实 cpi
 *   - DPI 走硬件分辨率寄存器（0x48~0x4B + 0x47 生效），MARK 行同时给出
 *     原始 counts(delta) 与实发 counts(out)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#include "paw3395.h"
#include "ble_hid.h"
#include "input.h"
#include "battery.h"
#include "cfg_link.h"
#include "app_log.h"

/* 板载 RGB 三个脚同时接到 PWM20 的三路输出（板级 pwm-leds 已定义）：
 *   pwm_led0 -> PWM_OUT0 -> P1.22 = 红
 *   pwm_led1 -> PWM_OUT1 -> P1.23 = 蓝
 *   pwm_led2 -> PWM_OUT2 -> P1.24 = 绿
 *
 * 电量指示已经搬到 dongle 上（无线鼠标工作时这三颗灯常亮很费电），
 * 鼠标上只在**插着 USB 充电**时做呼吸灯提示，其余时间全灭。
 * 呼吸灯需要调亮度，所以这里改用 PWM 而不是 GPIO 通断。
 */
static const struct pwm_dt_spec led_red = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led0));
static const struct pwm_dt_spec led_blue = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led1));
static const struct pwm_dt_spec led_green = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led2));

static const struct pwm_dt_spec *const leds[] = { &led_red, &led_green, &led_blue };
static const char *const led_names[] = { "red", "green", "blue" };

/* 亮度 = period 的百分比。pwm_set_pulse_dt 的 pulse 单位是 ns，
 * 板级 pwm-leds 是 PWM_MSEC(20) + 反相（LED 低电平点亮），
 * 因此 pulse = 点亮时长，0 就是全灭。
 */
static void led_set_brightness(int idx, uint32_t pct)
{
	uint64_t pulse = (uint64_t)leds[idx]->period * pct / 100U;

	(void)pwm_set_pulse_dt(leds[idx], (uint32_t)pulse);
}

static void led_all_off(void)
{
	for (int i = 0; i < ARRAY_SIZE(leds); i++) {
		(void)pwm_set_pulse_dt(leds[i], 0);
	}
}

/* ---- 开关机提示灯 ----
 * 电源键长按 3 s 开/关机时给灯提示：
 *   开机 -> 蓝灯常亮 3 s
 *   关机 -> 红灯闪 3 s（250 ms 亮 / 250 ms 灭）
 *
 * 用 k_uptime_get() 记截止时刻、每轮只改一次占空比，不阻塞主循环；
 * 关机动画要在手动关机睡眠态里继续跑完，所以 poll 放在主循环最前面，
 * 睡眠分支（50 ms 兜底节拍）也会反复调到它。
 */
#define PWR_HINT_MS          3000
#define PWR_HINT_BLINK_MS    250

enum pwr_hint_kind {
	PWR_HINT_NONE = 0,
	PWR_HINT_ON_BLUE,     /* 开机：蓝灯常亮（leds[] 下标 2 = 蓝） */
	PWR_HINT_OFF_RED,     /* 关机：红灯闪（leds[] 下标 0 = 红） */
};

static enum pwr_hint_kind pwr_hint;
static int64_t pwr_hint_end_ms;
static bool pwr_hint_lit;    /* 红灯当前是否点亮（只在状态翻转时重绘） */
static bool pwr_hint_dirty;  /* 起始后需要重绘一次 */

static void pwr_hint_start(enum pwr_hint_kind kind)
{
	pwr_hint = kind;
	pwr_hint_end_ms = k_uptime_get() + PWR_HINT_MS;
	pwr_hint_lit = false;
	pwr_hint_dirty = true;
}

/* 返回 true 表示本轮提示灯占着 LED，呼吸灯不要再改亮度 */
static bool pwr_hint_poll(int *leds_on)
{
	if (pwr_hint == PWR_HINT_NONE) {
		return false;
	}

	int64_t now = k_uptime_get();

	if (now >= pwr_hint_end_ms) {
		pwr_hint = PWR_HINT_NONE;
		led_all_off();
		*leds_on = -1;
		return false;
	}

	if (pwr_hint == PWR_HINT_ON_BLUE) {
		if (pwr_hint_dirty) {
			pwr_hint_dirty = false;
			led_all_off();
			led_set_brightness(2, 100);
			*leds_on = 2;
		}
		return true;
	}

	/* 关机提示：从提示开始时刻算起，每 PWR_HINT_BLINK_MS 翻转一次 */
	bool lit = (((now - (pwr_hint_end_ms - PWR_HINT_MS)) / PWR_HINT_BLINK_MS) % 2) == 0;

	if (pwr_hint_dirty || lit != pwr_hint_lit) {
		pwr_hint_dirty = false;
		pwr_hint_lit = lit;
		led_all_off();
		if (lit) {
			led_set_brightness(0, 100);
		}
		*leds_on = lit ? 0 : -1;
	}

	return true;
}

/* ---- 低电量提示与自动关机 ----
 * 不充电时电量低于 BAT_LOW_WARN_PCT：红灯每 30 s 短闪 3 次提醒；
 * 连续 BAT_LOW_CONFIRM 次（5 s 一读）低于 BAT_LOW_OFF_PCT：走关机流程，
 * 保护电池不过放。关机后充电仍由 nPM1300 自主进行，但要用时仍需长按
 * 电源键 3 s 开机。
 * 充电时 VBAT 被抬高、折算出的电量虚高，所以判断时一律跳过。
 */
#define BAT_LOW_WARN_PCT     20
#define BAT_LOW_OFF_PCT       5
#define BAT_LOW_CONFIRM       3
#define BAT_WARN_PERIOD_MS    30000
#define BAT_WARN_ON_MS        100
#define BAT_WARN_OFF_MS       180
#define BAT_WARN_BLINKS       3
#define BAT_WARN_BURST_MS     ((BAT_WARN_ON_MS + BAT_WARN_OFF_MS) * BAT_WARN_BLINKS)

static bool bat_warn_active;
static int64_t bat_warn_next_ms;
static bool bat_warn_lit;

static void bat_warn_set(bool on)
{
	if (on == bat_warn_active) {
		return;
	}

	bat_warn_active = on;
	if (on) {
		bat_warn_next_ms = k_uptime_get() + BAT_WARN_PERIOD_MS;
	}
}

/* 每轮调用：低电时按 30 s 周期闪红灯；充电或开关机提示灯占用时让位。
 * 只在亮灭状态翻转时重绘，避免把 PWM 写爆。
 */
static void bat_warn_led_poll(int *leds_on, bool charging)
{
	if (!bat_warn_active || charging) {
		bat_warn_lit = false;
		return;
	}

	if (pwr_hint != PWR_HINT_NONE) {
		bat_warn_lit = false;   /* 提示灯优先，不争用 LED */
		return;
	}

	int64_t now = k_uptime_get();
	int64_t burst_start;

	if (now >= bat_warn_next_ms) {
		bat_warn_next_ms = now + BAT_WARN_PERIOD_MS;
		burst_start = now;
	} else {
		burst_start = bat_warn_next_ms - BAT_WARN_PERIOD_MS;
	}

	int64_t el = now - burst_start;
	bool lit = (el < BAT_WARN_BURST_MS) &&
		   ((el % (BAT_WARN_ON_MS + BAT_WARN_OFF_MS)) < BAT_WARN_ON_MS);

	if (lit != bat_warn_lit) {
		bat_warn_lit = lit;
		led_all_off();
		if (lit) {
			led_set_brightness(0, 100);   /* 红灯短闪 */
		}
		*leds_on = lit ? 0 : -1;
	}
}

/* ---- 主循环看门狗 ----
 * 主线程每转一圈就自增 loop_beat；下面的守护线程发现它 1 秒不动就复位。
 *
 * 为什么需要：Zephyr 从**普通线程**调 bt_gatt_notify 时，ATT 缓冲分配用的是
 * K_FOREVER（只有系统工作队列线程才会退化成 K_NO_WAIT）。实测在 dongle 重启、
 * 1 kHz 通知洪流的交界处，ACL/ATT 发送池被打满后缓冲区回不来，主线程就永久
 * 阻塞在 net_buf_alloc 上：表现为灯常亮、串口不再打印、不广播、按键全无响应，
 * 只能拔电。与其让鼠标变砖，不如自己复位重连（约 1 s）。
 */
static volatile uint32_t loop_beat;
/* 睡眠态标记：此时主线程可能长时间阻塞在 K_FOREVER，看门狗不该判死 */
static volatile bool app_sleeping;

static void stall_guard(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t last = loop_beat;
	int stalled = 0;

	/* 开机前 5 s 不判：传感器探测、BLE 初始化、首次配对都可能慢 */
	k_sleep(K_MSEC(5000));

	for (;;) {
		uint32_t now = loop_beat;

		/* 睡眠态本来就没有 loop_beat 变化，先同步基准再跳过判定。
		 * 周期放宽到 2 s：睡眠时这个守护线程没必要 2 Hz 唤醒 CPU。 */
		if (app_sleeping) {
			last = now;
			stalled = 0;
			k_sleep(K_MSEC(2000));
			continue;
		}

		if (now == last) {
			stalled++;
			if (stalled >= 2) {
				printk("pwr: main loop stalled 1000 ms -> reboot\n");
				sys_reboot(SYS_REBOOT_WARM);
			}
		} else {
			stalled = 0;
			last = now;
		}
		k_sleep(K_MSEC(500));
	}
}

K_THREAD_DEFINE(stall_tid, 768, stall_guard, NULL, NULL, NULL, 7, 0, 0);

/* 板载用户键（XIAO 丝印 USER_BUTTON / P0.09）：按一下就把当前累计计数打出来，
 * 用于推尺子标定 cpi 时随时取 A、B 两点，不用等 3 秒一行的状态行。
 */
static const struct gpio_dt_spec user_btn =
	GPIO_DT_SPEC_GET(DT_NODELABEL(button0), gpios);

#define LED_COUNT  ARRAY_SIZE(leds)

/* 主循环节拍：**不再用「固定 1 ms 睡一觉」**。
 * 实测 k_msleep(1) 一跳实际 ~1.2 ms，天花板只有 830 Hz，光这一项就到不了 1 kHz。
 * 改成事件驱动：有位移就立刻读（合并读一次 ~250 us），没位移就挂在 MOTION
 * 中断的信号量上等，等不到 1 ms 也算一次心跳（顺便轮询按键）。
 * 所有"每 N 毫秒"的节流全部改用 k_uptime_get() 的真实时间。
 */
#define LED_ANIM_POLL_MS   20    /* 呼吸/闪烁动画期间的轮询周期 */
#define BTN_SETTLE_POLL_MS 2     /* 按键去抖补读 / 点击补发 / 滚轮重试周期 */
#define SLEEP_POLL_MS      50    /* 手动关机态检查"长按 3 s 开机"的兜底节拍 */
/* 手动关机态低频轮询 VBUS：插上 USB 就自动唤醒去跑充电呼吸灯。
 * 1 s 一次，折合平均几 µA；不想自动开机就把它改大或删掉这段。 */
#define SLEEP_VBUS_POLL_MS 1000
#define BAT_EVERY_MS       5000  /* 5 s 读一次电量（单次约阻塞 1 ms） */
#define STATUS_EVERY_MS    3000  /* 3 s 打一行状态 */

/* 鼠标上的充电指示：只在插着 USB（充电）时亮，按电量分色呼吸：
 *   <25%   红色
 *   25~75% 绿色
 *   >75%   蓝色
 * 周期 2 s、亮度 5%~100%、每 40 ms 更新一档；
 * PWM 载波 50 Hz 由板级 pwm-leds（PWM_MSEC(20)）给定。
 * 注意：充电时 VBAT 被抬高，读数偏乐观，插上后通常会很快进入蓝色区。
 */
#define LED_BREATH_MS        2000
#define LED_BREATH_STEP_MS   40
#define LED_BREATH_MIN_PCT   5
#define LED_CHG_RED_BELOW    25   /* 低于该值红灯 */
#define LED_CHG_GREEN_MAX    75   /* 低于等于该值绿灯，之上蓝灯 */

/* HID 位移报告的最小发送间隔（µs）。
 *
 * 时钟必须用内核 tick（k_uptime_ticks），**不能用 k_cycle_get_32()**：
 * 这颗 nRF54LM20A 上 GRTC syscounter 不跟随系统时基（实测恒为 0），
 * 用它做门控会导致发送分支永远进不去 —— 表现就是"按键正常、光标不动"。
 *
 * 系统 tick 是 31250 Hz，1000 Hz 需要 31.25 tick/帧，整数 tick 装不下，
 * 所以用 Bresenham 小数分频：每 4 帧里 3 帧 31 tick、1 帧 32 tick，
 * 平均严格 1 ms，长期误差为 0。
 */
/* ---- 对照实验结论（1250 Hz 已验证完毕）----
 * 门控放到 1250 后，鼠标侧 send 仍是 993 Hz、rd≈1016 Hz、pend 恒为 0：
 * 说明数据供给（传感器出帧）和 LLPM 链路都在 1 kHz 量级，
 * 门控再放宽只会让主循环被控制器反压（loop 从 3.3 kHz 掉到 1.0 kHz），
 * 实际投递反而更低。因此正式值回到 1000 Hz。
 * 想真正超过 1000 Hz，必须换 ESB 2.4G 专用链路（prj_esb.conf）。
 */
#define REPORT_HZ          1000
#define TICK_HZ            CONFIG_SYS_CLOCK_TICKS_PER_SEC

/* 回报率可由配置通道改成 1K/2K/4K/8K，所以每帧 tick 数与 Bresenham 余数
 * 都是运行时变量（默认 1000 Hz = 31 + 0.25 tick/帧）。
 * 注意：实际能达到的上限受主循环耗时限制，本平台实测 1 kHz 已接近天花板，
 * 更高档位主要影响节拍设置，链路能否跟上要看 ESB 侧的实测。 */
static uint32_t rep_hz = REPORT_HZ;
static uint32_t rep_step = TICK_HZ / REPORT_HZ;   /* 31 */
static uint32_t rep_rem = TICK_HZ % REPORT_HZ;    /* 250 */

/* 运动期间固定 1 ms 一包。
 * 原来只在"pend 非零"时才发，导致包间隔随帧到达时间抖动（实测 1~6 ms，
 * 偶尔上百 ms）；Windows 指针加速对间隔极敏感，表现为速度漂移+抖动。
 * 现在最后一个有位移的帧之后 REPORT_ACTIVE_MS 内，即使 pend 为空也按节拍补发
 * 0,0 包，让主机看到均匀的 1 kHz 流；超过这个时间没位移就停，省电省带宽。
 *
 * 实测反馈：剧烈摇晃时鼠标常被晃离桌面，传感器读不到位移，200 ms 的窗口会让
 * 发包中断、测速网站显示掉到 500 Hz 左右。所以把窗口拉长到 1 s：短促的腾空
 * 期间仍保持 1 kHz 流，静止超过 1 s 才停，兼顾手感与省电。 */
#define REPORT_ACTIVE_MS   1000

/* 单次上报位移上限。BLE 版是 12 bit ±2047；ESB 版由 CMake 注入 32767
 * （int16 负载），超出部分留在 pend 里下轮再发，两版行为一致 */
#ifndef HID_MOTION_MAX
#define HID_MOTION_MAX     2047
#endif

/* ESB 单向链路的保活间隔：鼠标静止时不发位移包，接收器超过窗口收不到包
 * 就会判定"断开"（驱动里链路状态只在移动时亮）。所以醒着的时候定期丢一个
 * 0,0 空包，让接收器知道鼠标还在线。1 包/500 ms 对续航影响可以忽略。 */
#define ESB_KEEPALIVE_MS   500

/* 传感器轴向与"鼠标方向"的对应关系。实测左右相反，X 取反；
 * 上下方向本身是对的（往回拉 = 光标下移 = +Y，与 HID 约定一致），所以 Y 不取反。
 */
#define INVERT_X  1
#define INVERT_Y  0

/* ---- 传感器轴向串扰补偿 ----
 * 实测（尺子约束、停稳后按 MARK）：
 *   纯 Y 方向：dx/dy ≈ +0.012
 *   纯 X 方向：dy/dx ≈ +0.032（左 0.030 / 右 0.034）
 * 用 Q15 系数做反向剪切：
 *   x' = x - Kyx * y / 32768
 *   y' = y - Kxy * x / 32768
 * 0 = 关闭。补偿量很小，主要影响斜向运动；数值可单独微调。
 */
#define AXIS_CORR_YX_Q15   393    /* 0.012 * 32768 */
#define AXIS_CORR_XY_Q15   1060   /* 0.032 * 32768 */

/* 滚轮换算：驱动只输出原始正交步数，这里除以该宏才是 1 格滚轮。
 * 实测这颗编码器是 **2 步/格**（按 4 算时，要滚两格才出一格响应）。
 */
#define WHEEL_STEPS_PER_DETENT 2

/* 滚轮待发事件队列：正反方向分开排队，换方向时不会互相抵消。
 * 8 格足够覆盖一次快速来回拨动；满了就把新事件合并到队尾。 */
#define WHEEL_Q_LEN 8

/* 滚动方向：实测反了（往上滚却往下走），这里取反 */
#define WHEEL_INVERT  1

/* 按键读取节流：
 *   INT 有效时按 2 ms 间隔补读，凑满去抖需要的次数就停（次数由防误触发
 *   档位决定，见 debounce_n_from_level / input_debounce_count）；
 *   安静时每 BTN_POLL_MS 兜底读一次。
 * PCF8574 走 100 kHz I2C，单次 0.2~0.5 ms —— 每秒 100 次的旧节拍会吃掉
 * 1~5% 的上报时间，这是 1 kHz 差那几帧的主要来源之一。
 */
#define BTN_POLL_MS   200

/* 唤醒补发点击时，press 与 release 之间的间隔（ms）。
 * 同一 Report 特征上未发出的通知会在 ATT 队列里互相覆盖，必须错开。
 */
#define BTN_LATCH_HOLD_MS  10

/* 按键状态变化后额外重发几次 + 间隔（ms）。
 * USB HID 每主机只保留**最新一份**报告：1 kHz 位移洪流下，单个按键包
 * 有概率在下一帧到来前被覆盖（表现为"偶尔点不动"）。
 *
 * ESB 是单向丢包链路，按键又是"状态量"，所以这里做两件事：
 *   1) 每次状态变化多补发几包，覆盖 ACK 失败和 USB 覆盖；
 *   2) BTN_MIN_HOLD_MS：短按也至少保持这么久，保证"按下"状态有足够多
 *      包被发出去，不会整个按下+松开都落在两个成功包之间而丢失。
 * BLE 链路本身有重传，保持原来的 2 次即可。
 */
#if defined(CONFIG_APP_ESB)
#define BTN_REPEAT_COUNT   6
#define BTN_REPEAT_MS      4
#define BTN_MIN_HOLD_MS    24
#else
#define BTN_REPEAT_COUNT   2
#define BTN_REPEAT_MS      3
#define BTN_MIN_HOLD_MS    0
#endif

/* ESB 按键心跳：
 *   按住时每 20 ms 补发一次当前状态；
 *   松开后再补发 300 ms，避免依赖 500 ms 的空闲保活包。
 * BLE 链路本身有重传，保持原行为。 */
#if defined(CONFIG_APP_ESB)
#define BTN_HEARTBEAT_MS       20
#define BTN_RELEASE_REPEAT_MS  300
#else
#define BTN_HEARTBEAT_MS       0
#define BTN_RELEASE_REPEAT_MS  0
#endif

/* I2C 连续读失败超过该时长就强制释放一次，避免按键状态卡在按下 */
#define BTN_I2C_TIMEOUT_MS     300

/* ---- 统一唤醒信号量 ----
 * 能叫醒主线程的只有三件事件，且全部走中断：
 *   1) 传感器 MOTION 下降沿（有未读位移）
 *   2) 滚轮编码器 A/B 边沿
 *   3) PCF8574 按键 INT 下降沿（任意按键状态变化）
 * ISR 里只 give，具体状态由主线程醒来后再查，这样睡眠态可以真正 k_sem_take(K_FOREVER)。
 */
static K_SEM_DEFINE(wake_sem, 0, 1);
static struct gpio_callback motion_cb;

static void motion_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_sem_give(&wake_sem);
}

/* 输入层（滚轮 / 按键 INT）的唤醒回调，同样跑在 ISR 上下文，只做 give */
static void input_wake_isr(void)
{
	k_sem_give(&wake_sem);
}

/* ---- 电源管理 ----
 * 电源键（PCF 端口 bit3，低有效，原 DPI 键）只做一件事：
 *   长按 >=3 s  -> 开关机（软件睡眠：断链路 + 传感器 shutdown + LED 全灭）
 *                  开机蓝灯常亮 3 s，关机红灯闪 3 s
 * 短按**不再切换 DPI**：DPI 现在只由 dongle 配置通道下发，按键纯电源开关。
 * 无操作 3 分钟 -> 自动休眠（手动关机 / 自动休眠两种入睡原因，唤醒条件不同）
 */
#define PWR_LONG_PRESS_MS      3000
#define IDLE_SLEEP_MS          (3 * 60 * 1000)

/* 判定"用户真的动了"的最小位移（|dx|+|dy| 之和，单位 counts）。
 * 传感器静止时会偶尔吐出 ±1~2 counts 的噪声，若把这种抖动也算作操作，
 * 3 分钟无操作休眠就会被无限推迟（实测 4 分钟都没入睡）。
 * 4 counts 在 1600 dpi 下约 0.06 mm，任何真实移动都会立刻超过它。
 */
#define ACTIVITY_COUNTS        4

/* ---- PAW3395 Rest 分级 ----
 * 静止满 SENSOR_REST_ENTER_MS 就把 0x40 的 AWAKE 位清掉，让芯片自己降到
 * Rest1（run 1.7 mA -> Rest1 的几十~几百 µA，唤醒约 2 ms）。
 * Rest1 再闲置 10 s 会掉进 Rest2（唤醒 ~200 ms，肉眼可见掉帧），所以最多
 * 只在 Rest1 待 SENSOR_REST_HOLD_MS，然后写回 AWAKE 重开静止计时。
 * 折中结果：每 8 s 里 1 s 在 run 档，其余 7 s 在 Rest1。
 * 想更省电就调大这两个值，代价是"停一会儿再动"的首帧延迟变大。
 */
#define SENSOR_REST_ENTER_MS   1000
#define SENSOR_REST_HOLD_MS    7000
#define SENSOR_REST_WAKE_MS    2   /* Rest1 -> run 的响应时间 */

/* 排查开关：1 = 启用 Rest 分级（静止后清 AWAKE，换省电，代价 2 ms 唤醒）；
 * 0 = 全程 AWAKE 常开，恢复成之前的 run 档行为，用来排除本改动的影响。 */
#define SENSOR_REST_ENABLED    1

/* 诊断开关：1 = 不管 MOTION 是否拉低，每 10 ms 强制读一次 burst。
 * 用来区分"MOTION 线断了"和"传感器根本没在算位移"：
 *   mot > 0 -> 传感器有位移，只是 MOTION 没送到 MCU（查线/焊点）
 *   mot = 0 -> 传感器没在追踪（查配置/表面/抬升）
 */
#define PAW3395_MOTION_DEBUG   0

/* ---- 抬升/落地防冲 ----
 * 鼠标抬起晃动时，传感器会拍到模糊表面并持续累加 counts，落地第一帧一次性
 * 吐出来，表现为光标"闪现"一段。三层保护：
 *   1) SQUAL 抬升判定（模式 A）：连续 3 帧表面质量 <30 判为抬起，位移清零
 *   2) 落地丢首帧：距上次读 burst 超过 20 ms，第一帧直接丢弃
 *   3) 单帧幅度兜底：|dx|/|dy| 超过「当前 CPI × 0.35」判废。
 *      0.35 英寸/ms ≈ 8.9 m/s，人手物理上到不了；按 DPI 缩放，低 DPI 不会误伤。
 * LIFT_GUARD_MODE: 1 = A（SQUAL 固定阈值 + 2 + 3），
 *                  2 = B（仅 2 + 3），
 *                  3 = AB 兼顾（SQUAL 自适应基线 + 2 + 3），
 *                  4 = Lift_Stat（传感器硬件抬升位 + 2 + 3），
 *                  5 = Lift_Stat 或 SQUAL<40（双条件 + 2 + 3，当前出货版）。
 * 实测结论（2026-10-31）：Motion 寄存器 bit3 是硬件 Lift_Stat，抬起时会置 1
 * （实测 squal=69/82 时也能抓到）；SQUAL 在抬起时 25~37、快速移动时约 111~119，
 * 两路取或后能稳定锁住抬起，且正常移动不误触发。
 */
#define LIFT_GUARD_MODE        5
#define LIFT_SQUAL_LOW         40
#define LIFT_SQUAL_HIGH        100   /* 恢复阈值：桌面/快速移动 SQUAL 约 110~190 */
#define LIFT_CONFIRM_FRAMES    3
#define LIFT_SETTLE_FRAMES     2
/* 「落地/静止后首帧」判定窗口。
 * 2026-11-01 抓包实测：游玩期间 guard_stale 约 0.7 次/秒。原 20 ms 太小——
 * 瞄准时两次修正之间常见的 20~50 ms 停顿都会被当成"落地"，把重新开始
 * 移动后的第一帧整段清零，持续造成少量但累积的位移丢失。
 * 抬高到 60 ms：只丢真正离开垫面/长时间静止后的首帧；抬手落地还有
 * lift_settle + spike 两层保护兜底。
 */
#define DELTA_STALE_GAP_MS     60
/* 落地/静止后首帧的位移门限：只有这一帧的 |dx|+|dy| 明显偏大才丢弃。
 * 原逻辑只要间隔 >60 ms 就整帧丢掉，慢速瞄准时 20~60 ms 的停顿会被误伤；
 * 小位移保留，真正落地攒出来的大位移仍然拦掉。 */
#define DELTA_STALE_BIG        128
/* 单帧位移上限：当前 CPI × 0.35 英寸/ms（约 8.9 m/s），下限 500 counts */
#define DELTA_SPIKE_PER_CPI    35
#define DELTA_SPIKE_MIN        500
/* 调试：抬升状态变化、以及未丢弃的大位移帧；正式版置 0 */
#define LIFT_STAT_DEBUG        0
#define LIFT_BIG_DEBUG         500

/* 临时标定：鼠标左键每按一次直接打印一行 MARK（推尺子测 CPI 用）。
 * 正式固件必须置 0，否则每次点击都会刷一行日志。 */
#define CALIB_MARK_ON_LEFT     0

/* ---- DPI 锁定 ----
 * 鼠标侧的 DPI 固定在编译期常量，运行期不再接受任何改动：
 *   - 电源键只管开关机，短按不切 DPI
 *   - 配置通道下发的 SET_DPI / SET_ACTIVE 只更新接收器自己的状态，
 *     不再写传感器
 * 这样"用着用着 DPI 自己变了"从根上不可能再发生。想换速度就改这里
 * （或 paw3395.h 的 PAW3395_CPI_DEFAULT）后重新烧录。
 */
#define MOUSE_FIXED_CPI  PAW3395_CPI_DEFAULT

/* ---- 分辨率（DPI） ----
 * 早先定位过 0x0D/0x0E/0x0F，都不改位移量；真正生效的是 datasheet §8.2 的
 * 0x48~0x4B（16 bit，50 cpi/LSB）+ 0x47 写 1 确认，见 paw3395_set_cpi。
 */

/* PCF8574 端口位 -> BLE HID 按键位的映射（实机标定，2026-10-20，已端到端验证）：
 *   bit0 = P0（网名 BTN_LEFT，U4.3）  -> **鼠标右键**（网表命名与实际左右相反）
 *   bit1 = P1（网名 BTN_RIGHT，U11.3）-> **鼠标左键**
 *   bit2 = P2（BTN_WHEEL，SW2.2 D2F-F 机械微动）-> 中键
 *   bit3 = P3（BTN_DPI，SW3.3 轻触）  -> 预留做电源开关，不映射
 *   bit4 = P4（BTN_SIDE1，FPC1.1）    -> 侧键2（与 bit5 对调）
 *   bit5 = P5（BTN_SIDE2，FPC1.4）    -> 侧键1
 * 硬件补充：BTN_LEFT/BTN_RIGHT 网络上原本没有上拉，实测已各加 5.1k 到 3.3V，
 * 空闲 2.17V / 按下 0.13V，可正常识别。
 * PCF8574 端口：未按下 = 1、按下 = 0（内部上拉），故判定用取反。
 */
static uint8_t btn_map(uint8_t raw)
{
	/* 安全锁：只有「曾经读到过 1（即确认松开过）」的位，才允许被判定为按下。
	 * 期初 BTN_LEFT/BTN_RIGHT 恒读 0（弱上拉撑不起逻辑高），这道锁避免误点击；
	 * 现在外部上拉已就位，第一次读到高电平即解锁。
	 */
	static uint8_t seen_high;
	uint8_t pressed;
	uint8_t btns = 0;

	seen_high |= raw;
	pressed = (uint8_t)(~raw & seen_high);

	if (pressed & BIT(0)) {
		btns |= BLE_HID_BTN_RIGHT;
	}
	if (pressed & BIT(1)) {
		btns |= BLE_HID_BTN_LEFT;
	}
	if (pressed & BIT(2)) {
		btns |= BLE_HID_BTN_MIDDLE;
	}
	if (pressed & BIT(4)) {
		btns |= BLE_HID_BTN_SIDE2;
	}
	if (pressed & BIT(5)) {
		btns |= BLE_HID_BTN_SIDE1;
	}
	/* bit3（BTN_DPI 轻触）刻意不映射：留给电源开关 */

	return btns;
}

/* 防误触发档位 -> 去抖需要的"连续相同读值"次数（低/中/高）。
 * 次数越多，抬鼠复位时的机械抖动越不容易被当成点击，代价是响应慢几毫秒。 */
static uint8_t debounce_n_from_level(uint8_t level)
{
	static const uint8_t table[3] = { 3, 5, 8 };

	return table[MIN(level, 2)];
}

int main(void)
{
	int32_t sum_x = 0;
	int32_t sum_y = 0;
	/* 轴向补偿前的累计值，MARK 时和补偿后一起打印，方便判断补偿是否生效 */
	int32_t raw_sum_x = 0;
	int32_t raw_sum_y = 0;
	/* 补偿的余数累加器：1 kHz 下不能每帧直接右移取整，
	 * 否则截断误差会累积成很大的假位移。 */
	int32_t axis_yx_rem = 0;
	int32_t axis_xy_rem = 0;

	printk("\n=== xiao-fw: nRF54LM20A gaming mouse ===\n");
	printk("build: %s %s\n\n", __DATE__, __TIME__);

	for (int i = 0; i < LED_COUNT; i++) {
		if (!pwm_is_ready_dt(leds[i])) {
			printk("led %s not ready\n", led_names[i]);
			return 0;
		}
	}

	/* 开机先全灭，再给蓝灯常亮 3 s 的开机提示 */
	led_all_off();
	pwr_hint_start(PWR_HINT_ON_BLUE);

	/* ---- 输入层：滚轮编码器 + PCF8574 按键扩展 ---- */
	int input_err = input_init();

	if (input_err) {
		printk("input init failed (%d)\n", input_err);
	}

	/* 用户键：按下即打印一行 MARK，便于取 A/B 两点 */
	if (gpio_is_ready_dt(&user_btn)) {
		(void)gpio_pin_configure_dt(&user_btn, GPIO_INPUT);
	}

	/* ---- SPI 回环自检：短接 D9<->D10 时 rx 应为 0xA5 ----
	 * 用来把「MCU 侧 SPI 外设/引脚」和「传感器侧接线/供电」两类问题切开
	 */
	uint8_t lb_rx = 0;
	int lb_err = paw3395_spi_loopback(&lb_rx);

	printk("spi loopback (jumper D9-D10): err=%d rx=0x%02X %s\n",
	       lb_err, lb_rx, (lb_rx == 0xA5) ? "PASS" : "(no jumper)");

	/* ---- PAW3395 ---- */
	int err = paw3395_init();

	printk("paw3395 init: %d\n", err);

	uint8_t product_id = 0xEE, revision_id = 0xEE;
	bool sensor_ok = false;

	if (paw3395_probe(&product_id, &revision_id) == 0) {
		printk("paw3395 product_id=0x%02X revision_id=0x%02X\n",
		       product_id, revision_id);
		sensor_ok = (product_id != 0x00) && (product_id != 0xFF);
		if (sensor_ok) {
			/* 寄存器表来自 datasheet v0.8 §8.1：分辨率在 0x48~0x4B（16 bit，50 cpi/LSB） */
			paw3395_reg_dump();

			uint16_t res_x = (uint16_t)(paw3395_read_reg(PAW3395_REG_RES_X_L) |
						    (paw3395_read_reg(PAW3395_REG_RES_X_H) << 8));
			uint16_t res_y = (uint16_t)(paw3395_read_reg(PAW3395_REG_RES_Y_L) |
						    (paw3395_read_reg(PAW3395_REG_RES_Y_H) << 8));

			printk("paw3395 res X=0x%04X Y=0x%04X -> %u cpi (50 cpi/LSB)\n",
			       res_x, res_y, (unsigned int)res_x * 50U);
			printk("paw3395 perf 0x40=0x%02X (AWAKE=%u)\n",
			       paw3395_read_reg(PAW3395_REG_PERFORMANCE),
			       (unsigned int)(paw3395_read_reg(PAW3395_REG_PERFORMANCE) >> 7));
		} else {
			/* 串口控制台只保证 ASCII，这里不要用中文 */
			printk("  -> invalid ID: sensor not wired or not powered\n");
		}
	}

	/* MOTION 中断常驻使能：主循环空闲时就是靠它的信号量被唤醒的，
	 * 否则每次都要等满 1 ms 超时，位移响应会被拉粗到 ~800 Hz。
	 * 回调只挂一次（睡眠路径复用同一个，不重复 add/remove）。 */
	if (sensor_ok) {
		(void)paw3395_motion_irq_configure(&motion_cb, motion_isr, true);
	}

	/* 滚轮 / 按键 INT 的中断都投递到 wake_sem，睡眠态才可能真正睡死 */
	input_set_wake_notify(input_wake_isr);

	/* ---- 电池电量（nPM1300 PMIC，V(BAT) 折算百分比） ---- */
	int bat_err = battery_init();

	if (bat_err) {
		printk("battery init failed (%d)\n", bat_err);
	}

	/* ---- 配置通道（dongle 通过 ESB ACK payload 下发 DPI/回报率等） ---- */
	cfg_link_init();

	/* 按保存的"静默高度"档位应用 LOD：0=1mm，1/2=2mm */
	if (sensor_ok) {
		(void)paw3395_lift_cutoff_set(cfg_link_lift_off());
	}

	/* ---- BLE HID ---- */
	int ble_err = ble_hid_init();

	if (ble_err) {
		printk("ble init failed (%d)\n", ble_err);
	}

	int leds_on = -1;
	int btn_prev = 0;
	int32_t mark_x = 0;
	int32_t mark_y = 0;
	int32_t mark_raw_x = 0;
	int32_t mark_raw_y = 0;
#if CALIB_MARK_ON_LEFT
	bool cal_mark_req = false;
	bool cal_left_prev = false;
#endif

	/* 按键端口：raw = 最近一次原始读值，debounced = 去抖后确认值，
	 * btn_sent = 上一次实际下发给 HID 的按键位（避免重复上报）
	 */
	uint8_t btn_raw_prev = 0xFF;
	uint8_t btn_debounced = 0xFF;
	uint8_t btn_sent = 0;
	/* 断链窗口里按下过的按键位，以及补发 release 的时刻 */
	uint8_t btn_latch = 0;
	int64_t btn_latch_release_at = 0;
	/* 按键状态变化后还要重发几次、下一次重发的时刻 */
	uint8_t btn_repeat = 0;
	int64_t btn_repeat_at = 0;
	/* 短按保持窗口：btn_hold_bits 是需要继续保持按下的位，
	 * btn_hold_until 是截止时刻；btn_phys_prev 用来识别"新按下"。 */
	uint8_t btn_hold_bits = 0;
	int64_t btn_hold_until = 0;
	uint8_t btn_phys_prev = 0;
	/* ESB 按键心跳：下一次补发时刻；松开后继续补发的截止时刻 */
	int64_t btn_heartbeat_at = 0;
	int64_t btn_release_until = 0;
	/* 上一轮是否已连接（用于检测"断开→连接"跳变） */
	bool was_connected = false;

	/* ---- 传感器 Rest 分级状态 ----
	 * sensor_resting：当前 AWAKE 位是否已清零（芯片在 Rest1/2/3）
	 * sensor_rest_at：本次进 Rest 的时刻，用于 SENSOR_REST_HOLD_MS 上限
	 * last_motion_ms：最近一次"真实位移"（>= ACTIVITY_COUNTS）的时刻，
	 *                 静止噪声不参与计时，否则永远进不了 Rest
	 */
	bool sensor_resting = false;
	int64_t sensor_rest_at = 0;
	int64_t last_motion_ms = k_uptime_get();
	/* 最后一个"有位移"的帧时刻：决定固定 1 ms 节拍还要补发多久 */
	int64_t last_delta_ms = k_uptime_get();

	/* ---- 抬升/落地防冲状态 ---- */
#if LIFT_GUARD_MODE == 1 || LIFT_GUARD_MODE == 3 || LIFT_GUARD_MODE == 5
	bool lifted = false;
	uint8_t lift_low_cnt = 0;
	uint8_t lift_ok_cnt = 0;
#elif LIFT_GUARD_MODE == 4
	bool lifted = false;
#endif
#if LIFT_GUARD_MODE == 3
	uint16_t squal_base = 0;   /* 桌面上的 SQUAL 慢速平均，用来定抬起阈值 */
#endif
#if LIFT_STAT_DEBUG
	int64_t dbg_big_ms = 0;
	bool dbg_lift_prev = false;
#endif
	uint8_t lift_settle = 0;
	int64_t last_burst_ms = 0;
	uint32_t guard_lift = 0;    /* 因判定抬起而丢掉的帧数 */
	uint32_t guard_stale = 0;   /* 因落地/静止后首帧被丢的帧数 */
	uint32_t guard_spike = 0;   /* 因单帧幅度异常被丢的帧数 */

	/* ---- 电源键（bit3）状态机 ----
	 * pwr_seen_high：安全锁，bit3 必须"确认松开过"（读到高）才允许判按下
	 * pwr_pressed / pwr_press_start：当前是否按住、按下起始时刻
	 * pwr_long_fired：本次按住是否已经触发过关机（防止重复进入睡眠）
	 */
	uint8_t pwr_seen_high = 0;
	bool pwr_pressed = false;
	bool pwr_long_fired = false;
	int64_t pwr_press_start = 0;

	/* ---- 电源状态 ----
	 * sleeping：当前是否处于睡眠态
	 * sleep_manual：true = 电源键长按关机（必须长按 3 s 才能开）；
	 *               false = 3 分钟无操作自动休眠（任意键/滚轮即可唤醒）
	 * last_activity：最近一次"用户操作"时刻，用于 3 分钟超时判定
	 */
	bool sleeping = false;
	bool sleep_manual = false;
	int64_t last_activity = k_uptime_get();
	uint8_t sleep_seen_high = 0;
	bool sleep_pressed = false;
	int64_t sleep_press_start = 0;
	/* 关机后用于识别"插上 USB"这个跳变：-1 = 还没读到基线。
	 * 只在 未充电 -> 充电 的跳变时唤醒，避免边充边关机时被立刻叫醒。 */
	int8_t sleep_vbus_prev = -1;

	/* 滚轮折算的余数（正交步数不足一格的部分），必须跨跳保留 */
	int wheel_rem = 0;
	/* 滚轮待发事件队列（有符号格数，按发生顺序排列）。
	 * 用队列而不是单个累加器：换方向时 +1/-1 不会被抵消。 */
	int32_t wheel_q[WHEEL_Q_LEN] = { 0 };
	int wheel_q_head = 0;
	int wheel_q_tail = 0;
	int wheel_q_count = 0;

	/* 启动时打一行按键端口初始值，方便按位辨认是哪个键 */
	{
		uint8_t raw = 0xFF;

		if (input_buttons_read(&raw) == 0) {
			printk("btn: raw=0x%02X\n", raw);
			btn_raw_prev = raw;
		}
	}

	/* 输出累计（真正发给 HID 的 counts），MARK 时用来核对比例 */
	int32_t out_x = 0;
	int32_t out_y = 0;
	int32_t mark_out_x = 0;
	int32_t mark_out_y = 0;

	/* 待发送位移累计（软件缩放后）：只有发成功才清零，BLE 拥塞时不丢位移 */
	int32_t pend_x = 0;
	int32_t pend_y = 0;
	int64_t last_tx_ms = 0;    /* 最近一次成功发包时刻（保活用） */
	uint32_t send_due_ticks = 0;
	uint32_t send_frac = 0;
	uint32_t rep_ok = 0;
	uint32_t rep_busy = 0;
	/* 本窗口内传感器"有新位移待读"（MOTION 有效）的次数。
	 * 和 rep_ok 对比就能分开两种上限：
	 *   frame_cnt ≈ send      -> 数据供给就是 ~1 kHz，卡在传感器出帧率
	 *   frame_cnt >  send     -> 数据有富余，卡在链路/发送侧
	 */
	uint32_t frame_cnt = 0;
	uint32_t motion_cnt = 0;   /* 强制读里 MOT 位置 1 的次数（诊断用） */
	int64_t dbg_last_ms = k_uptime_get();
	/* 位移读失败的日志节流：SPI 真坏时如果每轮都 printk，
	 * 1 kHz 主循环会被串口彻底拖死，反而看不到问题。 */
	int64_t motion_err_ms = 0;
	/* 按键链路计数（累计）：成功发出 / 发送被拒 / 唤醒补偿点击次数。
	 * 用计数而不是逐事件打印 —— printk 走 115200 串口一次要几 ms，
	 * 会直接把 1 kHz 循环拖慢，也会改变被测行为。
	 */
	uint32_t btn_ok = 0;
	uint32_t btn_fail = 0;
	uint32_t btn_latch_cnt = 0;

	/* 电量百分比（-1 = 不可用）。电量指示已经搬到 dongle 上，
	 * 鼠标这三颗灯平时不亮（只有插着 USB 时才呼吸），所以这里只读值不上灯。
	 */
	int bat_pct = -1;
	/* 连续"电量低于关机阈值"的读数次数（避开发热/负载引起的瞬时压降） */
	uint8_t bat_low_cnt = 0;

	(void)battery_percent_get(&bat_pct);
#if defined(CONFIG_APP_ESB)
	/* ESB 没有 BAS：电量搭在负载 data[9] 上回传。**始终发真实值**，
	 * 接收器才能按真实电量判断低电（只在 ≤25% 亮呼吸灯），
	 * 并在充电/回升到 25% 以上时立刻灭灯，不会残留旧的低电状态。
	 * 10 字节负载里的这一格本来就有，不影响 1 kHz 发包节奏。
	 */
	ble_hid_set_battery((bat_pct >= 0 && bat_pct <= 100)
			    ? (uint8_t)bat_pct : 0xFF);
#endif

	/* I2C 读一次约 0.5~0.7 ms（PCF8574 最高 100 kHz），1 kHz 循环里必须限流：
	 * 否则 INT 一直有效时会每轮都读，循环周期被拖到 2 ms 以上，上报率掉到 400 Hz 左右。 */
	int64_t last_btn_rd = 0;
	int64_t btn_settle_until = 0;
	int64_t bot_last_ms = k_uptime_get();

	/* 时间节流基准（自由跑之后不能再拿 tick 当毫秒用） */
	int64_t last_status_ms = bot_last_ms;
	int64_t last_full_ms = bot_last_ms;
	int64_t last_bat_ms = bot_last_ms;
	int64_t last_btn_poll_ms = bot_last_ms;
	/* I2C 读失败保护/诊断 */
	int64_t last_btn_ok_ms = k_uptime_get();
	int64_t btn_timeout_log_ms = 0;
	uint32_t btn_err_last = 0;
	int64_t btn_err_log_ms = 0;
	/* 充电指示 / 电量上报状态：
	 *   led_charging：最近一次读到的 VBUS 状态（true = 插着 USB）
	 *   last_led_ms ：下一次呼吸调光时刻
	 *   bas_last_pct：上次经 BAS 上报过的电量（变化才上报）
	 */
	bool led_charging = false;
	int64_t last_led_ms = 0;
	int bas_last_pct = -1;

	uint32_t iters = 0;

	while (1) {
		int64_t now_ms = k_uptime_get();
		/* 喂看门狗：休眠态也照样转圈，所以这里是无条件自增 */
		loop_beat++;
		/* 开关机提示灯（睡眠态也要继续跑，保证关机红闪 3 s 完整） */
		(void)pwr_hint_poll(&leds_on);

		/* 本轮是否真的读到位移（决定循环是自由跑还是挂起等中断） */
		bool motion_seen = false;

		/* ================= SLEEP 态 =================
		 * 睡眠期间不做任何 HID 上报、不跑传感器轮询，只等唤醒中断：
		 *   - 手动关机（电源键长按）：必须再长按 3 s 才开机，避免误碰唤醒
		 *   - 自动休眠（3 分钟无操作）：任意鼠标键、滚轮或移动就醒，符合直觉
		 */
		if (sleeping) {
			uint8_t raw = 0xFF;
			bool woken = false;

			/* 手动关机：空闲时彻底挂起等中断（电源键/滚轮 INT 都会唤醒），
			 * 只在"关机红灯动画"和"正在计长按开机时间"期间保留短节拍。
			 * 这样关机后不再有 20 Hz 的周期性轮询耗电。 */
			if (sleep_manual) {
				k_timeout_t wait;

				if (pwr_hint != PWR_HINT_NONE) {
					wait = K_MSEC(SLEEP_POLL_MS);
				} else if (sleep_pressed) {
					wait = K_MSEC(SLEEP_POLL_MS);
				} else {
					wait = K_MSEC(SLEEP_VBUS_POLL_MS);
				}
				(void)k_sem_take(&wake_sem, wait);
			} else {
				(void)k_sem_take(&wake_sem, K_FOREVER);
			}

			/* 滚轮编码器靠 GPIO 中断计数，睡眠时仍然有效。
			 * 注意每轮都要 take（清空），否则手动关机期间累积的步数会在
			 * 唤醒瞬间一次性发出去，表现为"凭空滚了一大段"。
			 * 只有自动休眠才把滚轮当作唤醒事件；手动关机不认，防误触。
			 */
			{
				int wsteps = input_wheel_take();

				if (!sleep_manual && wsteps != 0) {
					woken = true;
				}
			}

			/* 自动休眠：移动鼠标也算唤醒（PAW3395 在 rest 模式下仍会拉低 MOTION）。
			 * 读一次把待读位移清掉并丢弃，避免唤醒瞬间光标跳一下。
			 */
			if (!sleep_manual && !woken && paw3395_motion_asserted()) {
				int16_t dx = 0, dy = 0;
				bool ovf = false;

				(void)paw3395_read_motion(&dx, &dy, &ovf);
				woken = true;
			}

			if (!woken && input_buttons_read(&raw) == 0) {
				sleep_seen_high |= raw;

				/* 自动休眠：任意鼠标键按下（靠 btn_map 的映射，bit3 不在其中）
				 * 或电源键按下都算唤醒。
				 *
				 * 这里必须用**原始端口值**：去抖要连续 3 次读到相同值，
				 * 而睡眠态是中断唤醒（一次边沿只读一次），等不到第 3 次。
				 * btn_map 内部有"该位必须曾读到高电平"的安全锁，误判可控。
				 */
				if (!sleep_manual) {
					bool pwr_down = ((raw & BIT(3)) == 0) &&
							((sleep_seen_high & BIT(3)) != 0);

					if (btn_map(raw) != 0 || pwr_down) {
						woken = true;
					}
				}

				/* 手动关机：电源键必须重新按住满 PWR_LONG_PRESS_MS 才开机。
				 * 入睡时该键通常仍被按住，所以要先读到高电平（松开过）才计时。
				 */
				if (sleep_manual) {
					bool pwr_down = ((raw & BIT(3)) == 0) &&
							((sleep_seen_high & BIT(3)) != 0);

					if (pwr_down) {
						if (!sleep_pressed) {
							sleep_pressed = true;
							sleep_press_start = k_uptime_get();
						}
					} else {
						sleep_pressed = false;
					}

					if (sleep_pressed &&
					    (k_uptime_get() - sleep_press_start) >= PWR_LONG_PRESS_MS) {
						woken = true;
					}
				}
			}

			/* 手动关机态低频轮询 VBUS：插上 USB 就自动唤醒，
			 * 让充电呼吸灯能亮起来，也可以边充边用。
			 * 只在"未充电 -> 充电"的跳变时唤醒；关机时本来就插着
			 * 线的情况先记基线，不会立刻把自己叫醒。 */
			if (sleep_manual && !woken) {
				bool chg = false;

				if (battery_charging_get(&chg) == 0) {
					if (sleep_vbus_prev < 0) {
						sleep_vbus_prev = chg ? 1 : 0;
					} else if (chg && sleep_vbus_prev == 0) {
						printk("pwr: vbus detected -> wake\n");
						last_bat_ms = 0;  /* 唤醒后立刻读电量/充电状态 */
						woken = true;
					} else {
						sleep_vbus_prev = chg ? 1 : 0;
					}
				}
			}

			if (!woken) {
				continue;
			}

			/* 唤醒后 MOTION 中断**保持使能**：诊断版靠它统计边沿率，
			 * 正常轮询期间也不关（ISR 只做一次自增，成本可忽略）。 */

			/* ---- 唤醒：手动关机时传感器确实 shutdown 过，要重走复位；
			 * 自动休眠只是没读它（芯片自己进 rest 模式），直接恢复轮询即可，
			 * 避免 Power_Up_Reset 带来的 ~50 ms 唤醒延迟。 ---- */
			if (sleep_manual) {
				/* wake 内部会重走 Power_Up_Reset + reset_and_config，
				 * 其中已经写入锁定的 CPI（PAW3395_CPI_DEFAULT），
				 * 所以这里不需要再单独设一次分辨率。 */
				(void)paw3395_wake();
			} else {
				/* 自动休眠期间传感器可能已降到 Rest2/Rest3，
				 * 先写回 AWAKE 让它回到 run 档，再恢复正常轮询。 */
				(void)paw3395_set_awake(true);
				k_msleep(SENSOR_REST_WAKE_MS);
			}

			ble_hid_start();
			printk("pwr: wake (%s)\n", sleep_manual ? "by power key long press" : "by activity");

			/* 手动开机（电源键长按唤醒）给蓝灯常亮 3 s 提示；
			 * 自动休眠是"睡"不是"关机"，不再重复闪灯。 */
			if (sleep_manual) {
				pwr_hint_start(PWR_HINT_ON_BLUE);
			}

			sleeping = false;
			app_sleeping = false;
			last_activity = k_uptime_get();

			/* 复位所有跟踪状态，避免刚唤醒的按住被当成新的长按 */
			sleep_seen_high = 0;
			sleep_pressed = false;
			pwr_seen_high = 0;
			pwr_pressed = false;
			pwr_long_fired = false;
			btn_raw_prev = 0xFF;
			btn_debounced = 0xFF;
			/* 不能假设主机记得"全部松开"：断链可能发生在按下与松开之间，
			 * 主机的 HID 状态会停在"按下"，之后所有点击都变成拖拽（表现为没反应）。
			 * 用 0xFF（不可能出现的按键状态）当"未知"，强制连上后第一轮
			 * 一定发一次真实状态，把主机拉回正确状态。
			 */
			btn_sent = 0xFF;
			btn_latch = 0;     /* 新的一轮：清掉上一轮遗留的待补发点击 */
			btn_latch_release_at = 0;
			btn_repeat = 0;
			btn_repeat_at = 0;
			btn_hold_bits = 0;
			btn_hold_until = 0;
			btn_phys_prev = 0;
			btn_heartbeat_at = 0;
			btn_release_until = 0;
			last_btn_ok_ms = k_uptime_get();
			pend_x = 0;
			pend_y = 0;
			axis_yx_rem = 0;
			axis_xy_rem = 0;
			/* 唤醒后传感器已经回到 run 档，Rest 计时从这一刻重新开始 */
			sensor_resting = false;
			last_motion_ms = k_uptime_get();
			wheel_rem = 0;     /* 丢弃睡前不足一格的正交余数，避免唤醒后跳一格 */
			wheel_q_head = 0;
			wheel_q_tail = 0;
			wheel_q_count = 0;
			leds_on = -1;
			continue;
		}

		/* ---- 应用 dongle 下发的设置 ----
		 * 解析在 cfg_link_process() 里，硬件动作留在这一层做：
		 * ESB 回调只入队，避免在协议栈上下文里动传感器/radio。
		 */
		{
			uint32_t chg = cfg_link_process();

			/* DPI 已锁死：CFG_LINK_CHG_DPI 故意不处理。
			 * 接收器侧的档位变化只留在接收器里，不会改传感器分辨率。 */

			if (chg & CFG_LINK_CHG_RATE) {
				rep_hz = cfg_link_report_rate();
				rep_step = TICK_HZ / rep_hz;
				rep_rem = TICK_HZ % rep_hz;
				send_frac = 0;
				send_due_ticks = (uint32_t)k_uptime_ticks() + rep_step;
#if APP_VERBOSE_LOG
				printk("cfg: report rate -> %u Hz (%u.%02u ticks/frame)\n",
				       rep_hz, rep_step,
				       (unsigned int)(rep_rem * 100U / rep_hz));
#endif
			}

			if (chg & CFG_LINK_CHG_MODE) {
				ble_hid_set_system_mode(cfg_link_system_mode());
			}

			if (chg & CFG_LINK_CHG_DEBOUNCE) {
				(void)input_set_debounce_count(
					debounce_n_from_level(cfg_link_debounce()));
			}

			if (chg & CFG_LINK_CHG_LIFTOFF) {
				(void)paw3395_lift_cutoff_set(cfg_link_lift_off());
			}

			if (chg & CFG_LINK_CHG_SYNC) {
				/* 运动同步：当前链路没有对应的硬件开关（传感器固定按
				 * MOTION 边沿出帧，主机侧的平滑由驱动负责），这里只记录。 */
#if APP_VERBOSE_LOG
				printk("cfg: motion sync %s (no hardware switch)\n",
				       cfg_link_motion_sync() ? "on" : "off");
#endif
			}
		}

		/* 用户键按下沿 -> 打印 MARK。
		 * 同时给出「距上一次 MARK 的增量」，推尺子标定 cpi 时：
		 * 起手按一次清零基准，推完再按一次，直接读 delta 就是 Δ。
		 */
		int btn_now = gpio_pin_get_dt(&user_btn);

#if CALIB_MARK_ON_LEFT
		if (cal_mark_req || (btn_now == 1 && btn_prev == 0)) {
			cal_mark_req = false;
#else
		if (btn_now == 1 && btn_prev == 0) {
#endif
			printk("MARK: raw=%d,%d corr=%d,%d | total=%d,%d squal=0x%02X\n",
			       raw_sum_x - mark_raw_x, raw_sum_y - mark_raw_y,
			       sum_x - mark_x, sum_y - mark_y, sum_x, sum_y,
			       sensor_ok ? paw3395_read_reg(PAW3395_REG_SQUAL) : 0);
			mark_raw_x = raw_sum_x;
			mark_raw_y = raw_sum_y;
			mark_x = sum_x;
			printk("  out: delta=%d,%d total=%d,%d (hw cpi %u)\n",
			       out_x - mark_out_x, out_y - mark_out_y,
			       out_x, out_y, MOUSE_FIXED_CPI);
			mark_out_x = out_x;
			mark_out_y = out_y;
			mark_y = sum_y;
		}
		btn_prev = btn_now;
		/* 每 5 s 读一次电池：
		 *   1) 百分比经 BAS（标准 Battery Service 0x180F/0x2A19）报给 dongle，
		 *      由 dongle 上的红灯做低电提示 —— 鼠标上不再常亮，省电
		 *   2) 同时读 VBUS 是否插着，只有插着才让鼠标上的呼吸灯亮
		 */
		if ((now_ms - last_bat_ms) >= BAT_EVERY_MS) {
			int pct = bat_pct;
			bool chg = false;

			last_bat_ms = now_ms;
			if (battery_percent_get(&pct) == 0) {
				bat_pct = pct;
#if defined(CONFIG_APP_ESB)
				/* ESB 变体：始终回传真实电量（见初始化处说明）。
				 * 低电判定与确认窗口放在接收器侧，鼠标只管报数。 */
				ble_hid_set_battery((bat_pct >= 0 && bat_pct <= 100)
						    ? (uint8_t)bat_pct : 0xFF);
#else
				if (bat_pct != bas_last_pct) {
					bt_bas_set_battery_level((uint8_t)bat_pct);
					bas_last_pct = bat_pct;
				}
#endif
			}
			if (battery_charging_get(&chg) == 0) {
				led_charging = chg;
			}

			/* ---- 低电量提示 / 自动关机 ----
			 * 充电时 VBAT 被抬高、电量读数虚高，所以整段跳过。
			 * 关机阈值要连续 BAT_LOW_CONFIRM 次命中，避开发热或大负载
			 * 造成的瞬时压降误关机（每 5 s 一读，即 15 s 确认窗口）。
			 */
			if (!led_charging && pct >= 0) {
				if (pct <= BAT_LOW_OFF_PCT) {
					if (bat_low_cnt < BAT_LOW_CONFIRM) {
						bat_low_cnt++;
					}
				} else {
					bat_low_cnt = 0;
				}

				bat_warn_set(pct <= BAT_LOW_WARN_PCT);

				if (bat_low_cnt >= BAT_LOW_CONFIRM) {
					bat_low_cnt = 0;
					bat_warn_set(false);
					printk("pwr: battery %d%% critical -> power off\n", pct);

					/* 和保护关机一样：断链 + 传感器 shutdown + 红闪提示 */
					ble_hid_stop();
					(void)paw3395_sleep();
					pwr_hint_start(PWR_HINT_OFF_RED);

					sleeping = true;
					app_sleeping = true;
					sleep_manual = true;
					sleep_seen_high = 0;
					sleep_pressed = false;
					sleep_vbus_prev = -1;
					pwr_pressed = false;
					wheel_rem = 0;
					continue;
				}
			} else {
				bat_low_cnt = 0;
				bat_warn_set(false);
			}
		}

		/* 低电提示：红灯每 30 s 短闪 3 次（充电时让位给蓝色呼吸灯） */
		bat_warn_led_poll(&leds_on, led_charging);

		/* 开关机提示灯占用 LED 期间不要动，等提示结束再恢复充电指示/灭灯。
		 * 之前这里写成 `if (led_charging && hint==NONE) ... else if (leds_on>=0)`
		 * 会在提示灯刚点亮时立刻走到 else 分支把 LED 全灭，表现就是长按
		 * 开/关机时灯完全没反应。
		 */
		if (pwr_hint != PWR_HINT_NONE) {
			/* 提示灯自己控制 LED */
		} else if (led_charging) {
			/* 充电模式：按电量分色呼吸（红<25% / 绿 25~75% / 蓝>75%）。
			 * 插线时无线链路照常跑 1 kHz，不做自动休眠，可以边充边玩。 */
			int idx;

			if (bat_pct < 0) {
				idx = 1;   /* 读数未知：用中性绿色，不误报低电 */
			} else if (bat_pct < LED_CHG_RED_BELOW) {
				idx = 0;   /* 红 */
			} else if (bat_pct <= LED_CHG_GREEN_MAX) {
				idx = 1;   /* 绿 */
			} else {
				idx = 2;   /* 蓝 */
			}

			if (idx != leds_on) {
				led_all_off();
				leds_on = idx;
				last_led_ms = 0;
			}
			if ((now_ms - last_led_ms) >= LED_BREATH_STEP_MS) {
				int64_t ph = now_ms % LED_BREATH_MS;
				uint32_t pct;

				if (ph > LED_BREATH_MS / 2) {
					ph = LED_BREATH_MS - ph;
				}
				pct = LED_BREATH_MIN_PCT +
				      (uint32_t)(ph * (100 - LED_BREATH_MIN_PCT) /
						 (LED_BREATH_MS / 2));
				last_led_ms = now_ms;
				led_set_brightness(leds_on, pct);
			}
		} else if (leds_on >= 0) {
			led_all_off();
			leds_on = -1;
		}

		/* 位移 + 上报：走 motion burst（0x16 一次读回 12 字节，2 个 SPI 事务，
		 * 约 180 us；逐寄存器读要 700 us）。
		 *
		 * 两级判定：
		 *   1) raw[0] bit7 = MOT 清零 -> 没有待读位移，直接当 0（此时 burst 的
		 *      delta 区会读成 FF FF FF FF，若照抄就会变成假的 -1,-1 位移）
		 *   2) MOT 置位但 delta 区仍全 FF -> burst 异常，回退到逐寄存器读
		 * 10 cm 实测定标：burst 下 delta=-6279（期望 6299，误差 0.3%）已通过。
		 */
		if (sensor_ok) {
			int16_t dx = 0, dy = 0;
			bool ovf = false;
			/* MOTION 引脚未拉低 = 传感器内部没有待读位移，空闲时跳过本次 SPI 读。
			 * 注意：跳过**不是丢数据** —— 传感器内部自己累加 counts，
			 * 等它再次拉低 MOTION 时读回来的就是累积值，只是省掉空闲时的 SPI 流量。
			 */
			bool have = paw3395_motion_asserted();

#if PAW3395_MOTION_DEBUG
			/* MOTION 一直不拉低时，每 10 ms 强制读一次 burst */
			if (!have && (now_ms - dbg_last_ms) >= 10) {
				dbg_last_ms = now_ms;
				have = true;
			}
#endif

			/* Rest -> run：必须先把 AWAKE 位写回再读 burst。
			 * Rest1 的响应约 2 ms，不写回的话这一帧读到的是 Rest 档的
			 * 旧数据（表现为唤醒后第一下"丢位移"）。
			 */
			if (have && sensor_resting) {
				(void)paw3395_set_awake(true);
				k_msleep(SENSOR_REST_WAKE_MS);
				sensor_resting = false;
				/* 被 Rest 唤醒也算"动过"，保证至少 SENSOR_REST_ENTER_MS
				 * 的 run 窗口，避免慢速移动时一帧一帧地在 Rest 之间来回翻。 */
				last_motion_ms = k_uptime_get();
			}

			if (have) {
				uint8_t raw[PAW3395_BURST_LEN] = { 0 };
				int rerr = paw3395_read_burst_raw(&dx, &dy, raw);

				frame_cnt++;

				if (!rerr && (raw[0] & 0x80) != 0) {
					motion_cnt++;
				}

				if (!rerr) {
					if ((raw[0] & 0x80) == 0) {
						dx = 0;
						dy = 0;
					} else if (raw[2] == 0xFF && raw[3] == 0xFF &&
						   raw[4] == 0xFF && raw[5] == 0xFF) {
						/* burst 异常 -> 逐寄存器读（burst 没清位移时仍能拿到真值） */
						rerr = paw3395_read_motion(&dx, &dy, &ovf);
					}
				}

				if (!rerr) {
#if LIFT_GUARD_MODE == 1 || LIFT_GUARD_MODE == 3 || LIFT_GUARD_MODE == 5
					bool has_mot = (raw[0] & 0x80) != 0;
#endif
					bool stale = (last_burst_ms != 0) &&
						     ((now_ms - last_burst_ms) >
						      DELTA_STALE_GAP_MS);
					int32_t adx = dx < 0 ? -dx : dx;
					int32_t ady = dy < 0 ? -dy : dy;
					int32_t spike_limit =
						((int32_t)MOUSE_FIXED_CPI * DELTA_SPIKE_PER_CPI) / 100;

					if (spike_limit < DELTA_SPIKE_MIN) {
						spike_limit = DELTA_SPIKE_MIN;
					}
					bool spike = (adx > spike_limit) ||
						     (ady > spike_limit);

#if LIFT_GUARD_MODE == 1
					/* 固定阈值版：连续 N 帧表面质量过低 = 已抬起 */
					if (has_mot) {
						if (raw[6] < LIFT_SQUAL_LOW) {
							if (lift_low_cnt < LIFT_CONFIRM_FRAMES) {
								lift_low_cnt++;
							}
							lift_ok_cnt = 0;
							if (lift_low_cnt >= LIFT_CONFIRM_FRAMES) {
								lifted = true;
								lift_settle = LIFT_SETTLE_FRAMES;
							}
						} else {
							lift_low_cnt = 0;
							if (lifted) {
								if (lift_ok_cnt < LIFT_CONFIRM_FRAMES) {
									lift_ok_cnt++;
								}
								if (lift_ok_cnt >= LIFT_CONFIRM_FRAMES) {
									lifted = false;
									lift_settle = LIFT_SETTLE_FRAMES;
								}
							}
						}
					}

					if (lifted) {
						dx = 0;
						dy = 0;
						guard_lift++;
					}
#elif LIFT_GUARD_MODE == 3
					/* AB 兼顾：用桌面上的 SQUAL 平均值定阈值。
					 * 阈值 = 基线 1/3（夹在 20~80），抬起后 SQUAL 回升到
					 * 阈值 +15 以上连续 3 帧才恢复，避免临界值抖动。
					 */
					if (has_mot && !lifted && raw[6] >= 40) {
						if (squal_base == 0) {
							squal_base = raw[6];
						} else {
							squal_base = (uint16_t)((squal_base * 7u +
									 raw[6]) / 8u);
						}
					}
					uint16_t lift_thr = squal_base / 3u;

					if (lift_thr < 20) {
						lift_thr = 20;
					}
					if (lift_thr > 80) {
						lift_thr = 80;
					}
					if (has_mot) {
						bool low = raw[6] < lift_thr;
						bool high = raw[6] > (uint16_t)(lift_thr + 15);

						if (low) {
							if (lift_low_cnt < LIFT_CONFIRM_FRAMES) {
								lift_low_cnt++;
							}
							lift_ok_cnt = 0;
							if (lift_low_cnt >= LIFT_CONFIRM_FRAMES) {
								lifted = true;
								lift_settle = LIFT_SETTLE_FRAMES;
							}
						} else if (high) {
							lift_low_cnt = 0;
							if (lifted) {
								if (lift_ok_cnt < LIFT_CONFIRM_FRAMES) {
									lift_ok_cnt++;
								}
								if (lift_ok_cnt >= LIFT_CONFIRM_FRAMES) {
									lifted = false;
									lift_settle = LIFT_SETTLE_FRAMES;
								}
							}
						}
					}

#if LIFT_SQUAL_DEBUG
					if (has_mot && raw[6] < LIFT_SQUAL_LOG_LEVEL &&
					    (now_ms - dbg_squal_ms) >= 100) {
						dbg_squal_ms = now_ms;
						printk("lift: squal=%u base=%u thr=%u dx=%d dy=%d\n",
						       raw[6], squal_base, lift_thr, dx, dy);
					}
#endif
					if (lifted) {
						dx = 0;
						dy = 0;
						guard_lift++;
					}
#elif LIFT_GUARD_MODE == 4
					/* 硬件 Lift_Stat：Motion 寄存器 bit3，
					 * 0 = 在表面，1 = 已抬起。burst BYTE[00] 直接带出。
					 */
					bool hw_lifted = (raw[0] & 0x08) != 0;

#if LIFT_STAT_DEBUG
					if (hw_lifted != dbg_lift_prev) {
						dbg_lift_prev = hw_lifted;
						printk("lift: hw=%d\n", hw_lifted);
					}
#endif
					if (hw_lifted) {
						lifted = true;
						lift_settle = LIFT_SETTLE_FRAMES;
					} else if (lifted) {
						lifted = false;
						lift_settle = LIFT_SETTLE_FRAMES;
					}

					if (lifted) {
						dx = 0;
						dy = 0;
						guard_lift++;
					}
#elif LIFT_GUARD_MODE == 5
					/* 双条件：硬件 Lift_Stat 或 SQUAL 连续 3 帧 <40。
					 * 抬起后要 SQUAL 回升到 100 以上连续 3 帧才恢复——
					 * 实测抬起/半落状态 SQUAL 常在 56~85，旧的 55 阈值会
					 * 提前放行，残留位移漏过去。
					 */
					bool hw_lifted = (raw[0] & 0x08) != 0;
					bool squal_low = has_mot &&
							 raw[6] < LIFT_SQUAL_LOW;
					bool squal_high = has_mot &&
							  raw[6] > LIFT_SQUAL_HIGH;

					if (squal_low) {
						if (lift_low_cnt < LIFT_CONFIRM_FRAMES) {
							lift_low_cnt++;
						}
					} else {
						lift_low_cnt = 0;
					}

					bool now_lifted = hw_lifted ||
							  (lift_low_cnt >= LIFT_CONFIRM_FRAMES);

					if (now_lifted && !lifted) {
						lifted = true;
						lift_settle = LIFT_SETTLE_FRAMES;
						lift_ok_cnt = 0;
#if LIFT_STAT_DEBUG
						printk("lift: on hw=%d squal=%u dx=%d dy=%d\n",
						       hw_lifted, raw[6], dx, dy);
#endif
					} else if (!now_lifted && lifted) {
						if (squal_high) {
							if (lift_ok_cnt < LIFT_CONFIRM_FRAMES) {
								lift_ok_cnt++;
							}
						} else {
							lift_ok_cnt = 0;
						}
						if (lift_ok_cnt >= LIFT_CONFIRM_FRAMES) {
							lifted = false;
							lift_settle = LIFT_SETTLE_FRAMES;
#if LIFT_STAT_DEBUG
							printk("lift: off squal=%u\n", raw[6]);
#endif
						}
					}

					if (lifted) {
						dx = 0;
						dy = 0;
						guard_lift++;
					}
#endif
					if (lift_settle > 0) {
						dx = 0;
						dy = 0;
						lift_settle--;
						guard_lift++;
					}

					/* 落地/静止后首帧：只有攒出来的位移明显偏大才丢，
					 * 慢速瞄准的小修正保留，避免"丢一点"的粘滞感。 */
					if (stale && ((adx + ady) > DELTA_STALE_BIG)) {
						dx = 0;
						dy = 0;
						guard_stale++;
					} else if (spike) {
						dx = 0;
						dy = 0;
						lift_settle = LIFT_SETTLE_FRAMES;
						guard_spike++;
					}

#if LIFT_STAT_DEBUG
					/* 没被任何保护丢掉的"大位移帧"：闪现的来源 */
					if ((dx != 0 || dy != 0) &&
					    ((adx + ady) > LIFT_BIG_DEBUG) &&
					    ((now_ms - dbg_big_ms) >= 200)) {
						dbg_big_ms = now_ms;
						printk("big: dx=%d dy=%d squal=%u raw0=0x%02X\n",
						       dx, dy, raw[6], raw[0]);
					}
#endif

					last_burst_ms = now_ms;
				} else {
					last_burst_ms = now_ms;
				}

				motion_seen = true;

				if (rerr != 0) {
					if ((now_ms - motion_err_ms) >= 1000) {
						motion_err_ms = now_ms;
						printk("motion read failed (%d)\n", rerr);
					}
				} else {
#if INVERT_X
					dx = -dx;
#endif
#if INVERT_Y
					dy = -dy;
#endif
					raw_sum_x += dx;
					raw_sum_y += dy;
					/* 轴向串扰补偿：把传感器 X/Y 轴不垂直的部分掰回来。
					 * 放在 INVERT 之后，因为系数是按“屏幕坐标”实测的。 */
					{
						int32_t n;
						int32_t corr;
						int32_t dxc;
						int32_t dyc;

						/* 用余数累加代替每帧右移，长期补偿量严格准确。 */
						n = (int32_t)AXIS_CORR_YX_Q15 * dy + axis_yx_rem;
						corr = n / 32768;
						axis_yx_rem = n - corr * 32768;
						dxc = (int32_t)dx - corr;

						n = (int32_t)AXIS_CORR_XY_Q15 * dx + axis_xy_rem;
						corr = n / 32768;
						axis_xy_rem = n - corr * 32768;
						dyc = (int32_t)dy - corr;

						dx = (int16_t)CLAMP(dxc, INT16_MIN, INT16_MAX);
						dy = (int16_t)CLAMP(dyc, INT16_MIN, INT16_MAX);
					}
					sum_x += dx;
					sum_y += dy;
					if (dx || dy) {
						last_delta_ms = k_uptime_get();
					}
					/* 有位移 = 用户操作，重置 3 分钟自动休眠计时。
					 * 注意用阈值而不是 dx/dy 非零：见 ACTIVITY_COUNTS 的说明。 */
					int32_t mag = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);

					if (mag >= ACTIVITY_COUNTS) {
						last_activity = k_uptime_get();
						last_motion_ms = last_activity;
					}
					/* 硬件 DPI 已生效，位移直接用原始 counts（不再软件缩放）；
					 * sum_x/sum_y 同样是原始 counts，推尺子定标读的就是它
					 */
					pend_x += dx;
					pend_y += dy;
				}
			}

			/* 到点就把攒下来的位移发出去。三条原则：
			 *   1) 发成功才扣减 pend —— 拥塞或未连接时留着下一轮发，不丢位移
			 *   2) 单条报告上限 ±2047（12bit），超出部分**留在 pend 里**下轮再发
			 *   3) 失败只计数不打日志，避免高节拍下刷屏
			 */
			/* 运动期间（最后 REPORT_ACTIVE_MS 内有位移）固定 1 ms 一包：
			 * 即使 pend 为空也发 0,0，保证主机看到均匀的 1 kHz 间隔。
			 * 静止超过该窗口后停发，回到"有数据才发"以省电。 */
			/* ESB 保活：静止期间也要定期发包，否则接收器判定断开、
			 * 驱动里的链路状态只在移动时才亮。BLE 版有连接态，不需要。 */
			bool keepalive_due = false;

#if defined(CONFIG_APP_ESB)
			keepalive_due =
				(k_uptime_get() - last_tx_ms) >= ESB_KEEPALIVE_MS;
#endif

			if (pend_x || pend_y || keepalive_due ||
			    (k_uptime_get() - last_delta_ms) < REPORT_ACTIVE_MS) {
				uint32_t now_t = (uint32_t)k_uptime_ticks();

				/* 按内核 tick 排期，每帧 31.25 tick（= 1 ms = 1000 Hz）。
				 * 用绝对时刻而不是"距上次 >= N"：主循环每 ~300 µs 才查一次，
				 * 相对计时会把每次检查的延迟累加进周期里。
				 */
				if ((int32_t)(now_t - send_due_ticks) >= 0) {
					int16_t px = (int16_t)CLAMP(pend_x, -HID_MOTION_MAX,
								    HID_MOTION_MAX);
					int16_t py = (int16_t)CLAMP(pend_y, -HID_MOTION_MAX,
								    HID_MOTION_MAX);

					if (ble_hid_send_motion(px, py) == 0) {
						uint32_t behind = now_t - send_due_ticks;
						uint32_t step = rep_step;

						last_tx_ms = k_uptime_get();
						out_x += px;
						out_y += py;
						pend_x -= px;
						pend_y -= py;

						if (behind > 2U * rep_step + 2U) {
							/* 空闲/拥塞后恢复：重新对齐，别一次补发一大串 */
							send_due_ticks = now_t + rep_step;
							send_frac = 0;
						} else {
							/* Bresenham：每帧累计 TICK_HZ%rate，
							 * 凑满 rate 就多走一个 tick，平均严格
							 * TICK_HZ/rate tick 一帧 */
							send_frac += rep_rem;
							if (send_frac >= rep_hz) {
								send_frac -= rep_hz;
								step++;
							}
							send_due_ticks += step;
						}
						rep_ok++;
					} else {
						rep_busy++;
					}
				}
			}

			/* ---- Rest 分级 ----
			 * 进：静止满 SENSOR_REST_ENTER_MS -> 清 AWAKE，芯片自己降 Rest1。
			 * 出：在 Rest1 待满 SENSOR_REST_HOLD_MS -> 写回 AWAKE，重开静止计时，
			 *     免得 10 s 后掉进 Rest2（~200 ms 唤醒，会明显掉帧）。
			 * 有位移时上面已经写回 AWAKE 并清了 sensor_resting，这里不重复处理。
			 */
#if SENSOR_REST_ENABLED
			{
				int64_t rest_now = k_uptime_get();

				if (!sensor_resting &&
				    (rest_now - last_motion_ms) >= SENSOR_REST_ENTER_MS) {
					(void)paw3395_set_awake(false);
					sensor_resting = true;
					sensor_rest_at = rest_now;
				} else if (sensor_resting &&
					   (rest_now - sensor_rest_at) >= SENSOR_REST_HOLD_MS) {
					(void)paw3395_set_awake(true);
					last_motion_ms = rest_now;
					sensor_resting = false;
				}
			}
#endif
		}

		/* ---- 输入层：按键 + 滚轮（新增，位移上报逻辑不变） ----
		 * 按键读节流（PCF8574 走 100 kHz I2C，单次 ~0.2~0.5 ms，是主循环里
		 * 剩下的大头阻塞）：
		 *   INT 有效 -> 以 2 ms 间隔补读，凑满去抖需要的 3 次就停；
		 *   其余时间 -> 每 BTN_POLL_MS(200 ms) 兜底读一次。
		 * 这样从"每秒 100 次"降到"每次按键 3 次 + 每秒 5 次"。
		 */
		bool do_btn_read = false;

			if (input_btn_int_asserted()) {
				/* 补读窗口要覆盖"防误触发"档位需要的读次数（每 2 ms 一次） */
				btn_settle_until = now_ms + (int64_t)input_debounce_count() * 2 + 2;
				if ((now_ms - last_btn_rd) >= 2) {
					do_btn_read = true;
				}
			} else if (now_ms < btn_settle_until &&
				   (now_ms - last_btn_rd) >= 2) {
				do_btn_read = true;
			} else if ((now_ms - last_btn_poll_ms) >= BTN_POLL_MS) {
				do_btn_read = true;
			}

			if (do_btn_read) {
				uint8_t raw;

				last_btn_rd = now_ms;
				last_btn_poll_ms = now_ms;

				if (input_buttons_read(&raw) == 0) {
					last_btn_ok_ms = now_ms;
				if (raw != btn_raw_prev) {
#if APP_VERBOSE_LOG
					printk("btn: raw=0x%02X (changed 0x%02X) -> mapped 0x%02X\n",
					       raw, raw ^ btn_raw_prev, btn_map(raw));
#endif
					btn_raw_prev = raw;
					/* 按键有变化 = 用户操作 */
					last_activity = k_uptime_get();
				}

				/* 电源键（bit3，低有效）：只做开关机，**短按不响应**。
				 * 按下沿开始计时，按住满 PWR_LONG_PRESS_MS 关机；
				 * 松开只是清掉这次按住，不做任何动作。
				 * 与 btn_map 同样的安全锁：bit3 必须曾读到高电平（确认松开过）。
				 *
				 * 注意：这里必须用去抖后的值（input_buttons_get），不能用
				 * input_buttons_read 返回的原始值。原始值只要有一次 I2C 读
				 * 毛刺或按键抖动让 bit3 变 0、下一轮恢复 1，就会凭空起一次
				 * 长按计时。
				 */
				uint8_t db_pwr = 0xFF;

				(void)input_buttons_get(&db_pwr);
				pwr_seen_high |= db_pwr;

				bool pwr_now = ((db_pwr & BIT(3)) == 0) &&
					       ((pwr_seen_high & BIT(3)) != 0);

				if (pwr_now && !pwr_pressed) {
					pwr_pressed = true;
					pwr_long_fired = false;
					pwr_press_start = k_uptime_get();
				} else if (!pwr_now && pwr_pressed) {
					pwr_pressed = false;
					pwr_long_fired = false;
				}
			}
		}

		/* I2C 保护/诊断：
		 * 读失败时 input.c 的 btn_confirmed 会停在旧值；如果超过
		 * BTN_I2C_TIMEOUT_MS 没有成功读，强制释放一次，避免"松开还在拖拽"。
		 */
		{
			uint32_t rd_err = input_button_read_errors();

			if (rd_err != btn_err_last && (now_ms - btn_err_log_ms) >= 1000) {
				btn_err_last = rd_err;
				btn_err_log_ms = now_ms;
				printk("btn: i2c read errors %u\n", (unsigned int)rd_err);
			}

			if ((now_ms - last_btn_ok_ms) > BTN_I2C_TIMEOUT_MS) {
				input_buttons_force_release();
				btn_debounced = 0xFF;
				btn_raw_prev = 0xFF;
				btn_hold_bits = 0;
				btn_hold_until = 0;
				btn_phys_prev = 0;
				last_btn_ok_ms = now_ms;

				if ((now_ms - btn_timeout_log_ms) >= 1000) {
					btn_timeout_log_ms = now_ms;
					printk("btn: i2c timeout, force release (errors %u)\n",
					       (unsigned int)rd_err);
				}
			}
		}

		/* ---- 长按电源键 >= PWR_LONG_PRESS_MS -> 关机睡眠 ---- */
		if (pwr_pressed && !pwr_long_fired &&
		    (k_uptime_get() - pwr_press_start) >= PWR_LONG_PRESS_MS) {
			pwr_long_fired = true;
			printk("pwr: power key long press (%u ms) -> power off\n",
			       (unsigned int)(k_uptime_get() - pwr_press_start));

			/* 1) 断链路 + 停广播（PC 会认为设备掉线） */
			ble_hid_stop();
			/* 2) 传感器写 0x3B=0xB6 进入 shutdown：
			 *    关掉内部成像阵列与 LED 驱动，灯才会真正灭。
			 *    （旧代码写 0x00 是空操作，芯片继续跑，灯一直亮） */
			(void)paw3395_sleep();
			/* 3) 红灯闪 3 s 作为关机提示（睡眠态继续跑完） */
			pwr_hint_start(PWR_HINT_OFF_RED);

			/* 4) 进入软件休眠：射频已关、传感器已 shutdown，
			 *    主循环在睡眠分支里挂起等中断，不再 20 Hz 轮询。 */
			sleeping = true;
			app_sleeping = true;
			sleep_manual = true;
			sleep_seen_high = 0;
			sleep_pressed = false;
			sleep_vbus_prev = -1;
			pwr_pressed = false;
			wheel_rem = 0;
			continue;
		}

		{
			/* 余数要跨跳保留：一次读到的正交步数往往不足 4 步，直接整除
			 * 会把不足一格的部分每跳都丢掉，结果就是滚轮完全不动。
			 * 但换方向时要丢掉上一方向的半格余数，否则它会吃掉新方向的
			 * 第一格（表现为"换方向第一格被吞"）。
			 */
			int raw_steps = input_wheel_take();

			if (raw_steps != 0 && wheel_rem != 0 &&
			    ((raw_steps > 0) != (wheel_rem > 0))) {
				wheel_rem = 0;
			}

			int steps = raw_steps + wheel_rem;
			int8_t wheel = (int8_t)CLAMP(steps / WHEEL_STEPS_PER_DETENT, -127, 127);
			uint8_t btns;

			/* 滚轮有动作 = 用户操作 */
			if (steps != 0) {
				last_activity = k_uptime_get();
			}

			/* 余数必须按**未取反**的格数算，否则会和累积的步数对不上 */
			wheel_rem = steps - (int)wheel * WHEEL_STEPS_PER_DETENT;

#if WHEEL_INVERT
			wheel = (int8_t)-wheel;
#endif

			(void)input_buttons_get(&btn_debounced);
#if CALIB_MARK_ON_LEFT
			{
				bool left_down = (btn_debounced & BIT(1)) == 0;
				if (left_down && !cal_left_prev) {
					cal_mark_req = true;
				}
				cal_left_prev = left_down;
			}
#endif
			btns = btn_map(btn_debounced);

			/* 短按拉伸：物理按键可能只闭合几毫秒，ESB 丢一包就整段没了。
			 * 新按下的位至少保持 BTN_MIN_HOLD_MS，期间每 BTN_REPEAT_MS
			 * 补发一次"按下"；松手后同样补发 release。
			 * BLE 版 BTN_MIN_HOLD_MS = 0，这段不会改变行为。 */
			{
				int64_t btn_now = k_uptime_get();
				uint8_t newly = btns & ~btn_phys_prev;

				btn_phys_prev = btns;
				if (newly) {
					btn_hold_bits |= newly;
					btn_hold_until = btn_now + BTN_MIN_HOLD_MS;
				}
				if (btn_now < btn_hold_until) {
					btns |= btn_hold_bits;
				} else {
					btn_hold_bits = 0;
				}
			}

			/* 发送必须成功才更新状态：链路被 1 kHz 位移包占满时
			 * bt_hids_inp_rep_send() 可能返回 -ENOMEM，按一下右键会被直接丢掉。
			 * 未连接时（移动唤醒后、重连完成前）不发、也不更新 btn_sent；
			 * 断链期间按下过的位记在 btn_latch 里，连上后补发一次点击。
			 */
			if (wheel != 0) {
				if (wheel_q_count == WHEEL_Q_LEN) {
					/* 队列满：合并到最后一个事件，保持顺序 */
					wheel_q[(wheel_q_tail + WHEEL_Q_LEN - 1) % WHEEL_Q_LEN] +=
						wheel;
				} else {
					wheel_q[wheel_q_tail] = wheel;
					wheel_q_tail = (wheel_q_tail + 1) % WHEEL_Q_LEN;
					wheel_q_count++;
				}
			}

			bool conn_now = ble_hid_connected();

			if (conn_now && !was_connected) {
				/* 刚从断开变为连接：主机端的按键状态是"未知"，
				 * 用 0xFF 强制下一轮把真实状态补发一次（见唤醒路径的说明） */
				btn_sent = 0xFF;
			}
			was_connected = conn_now;

			if (!conn_now) {
				/* 断链期间（移动唤醒后、重连完成前）：记下按下过的位。
				 * 这类点击的"按下+松开"整个落在断开窗口里，PC 从未收到按下，
				 * 只补发当前状态是没用的（那时已经松开了），必须稍后补发一次
				 * press -> release，否则唤醒后的第一下点击必然丢失。
				 */
				btn_latch |= btns;
			} else if (btn_latch) {
				if (btns != 0) {
					/* 连上时键还按着：交给下面的正常逻辑，不要伪造松开 */
					btn_latch = 0;
					btn_latch_release_at = 0;
				} else if (btn_latch_release_at == 0) {
					if (ble_hid_send_buttons(btn_latch, 0, 0) == 0) {
						btn_sent = btn_latch;
						btn_ok++;
						btn_latch_cnt++;
						/* 同一属性的未发通知会在 ATT 队列里被覆盖，
						 * press 与 release 必须隔开几毫秒 */
						btn_latch_release_at = k_uptime_get() + BTN_LATCH_HOLD_MS;
					}
				} else if (k_uptime_get() >= btn_latch_release_at) {
					(void)ble_hid_send_buttons(0, 0, 0);
					btn_sent = 0;
					btn_latch = 0;
					btn_latch_release_at = 0;
				}
			} else if (wheel_q_count > 0 || btns != btn_sent) {
				/* 单条 HID 滚轮最多 ±127，剩下的留在队列头
				 * 下一包继续发，拥塞时也不会丢格数。 */
				int8_t wheel_out = 0;
				bool btn_changed = (btns != btn_sent);

				if (wheel_q_count > 0) {
					wheel_out = (int8_t)CLAMP(wheel_q[wheel_q_head],
								  -127, 127);
				}

				int br = ble_hid_send_buttons(btns, wheel_out, 0);

				if (br == 0) {
					/* 松开时开一个 300 ms 的补发窗口，避免松开包丢了
					 * 还要等 500 ms 空闲保活才恢复。 */
					if (btns == 0 && btn_sent != 0 && btn_sent != 0xFF) {
						btn_release_until = k_uptime_get() +
								    BTN_RELEASE_REPEAT_MS;
					} else if (btns != 0) {
						btn_release_until = 0;
					}
					btn_sent = btns;
					btn_ok++;
					/* 只有按键状态真的变化才补发 6 次。
					 * 滚轮包只是借这个报告带 wheel，不能每次都重置重复计数，
					 * 否则按住按键滚动时会插入大量纯按键包，和滚轮抢链路。 */
					if (btn_changed) {
						btn_repeat = BTN_REPEAT_COUNT;
						btn_repeat_at = k_uptime_get() + BTN_REPEAT_MS;
					}
					btn_heartbeat_at = k_uptime_get() + BTN_HEARTBEAT_MS;
					if (wheel_out != 0) {
#if APP_VERBOSE_LOG
						printk("wheel: steps=%d -> %d notch\n", steps, wheel_out);
#endif
					}
					if (wheel_q_count > 0) {
						wheel_q[wheel_q_head] -= wheel_out;
						if (wheel_q[wheel_q_head] == 0) {
							wheel_q_head = (wheel_q_head + 1) %
								       WHEEL_Q_LEN;
							wheel_q_count--;
						}
					}
				} else {
					btn_fail++;
					if (btns != 0) {
						/* 链路刚连上、HIDS 订阅还没就绪时 bt_hids_inp_rep_send()
						 * 会一直返回 -EACCES（实测重连后约 200 ms）。
						 * 这个窗口里也把按下的位记进补偿队列：等订阅就绪时
						 * 若键已经松开，还要补发一次 press->release，
						 * 否则唤醒后第一下点击就永久丢失。
						 */
						btn_latch |= btns;
					}
				}
			} else if (btn_repeat != 0 && k_uptime_get() >= btn_repeat_at) {
				/* 重发当前按键状态，抵掉 USB 侧"每周期只留最新一包"的覆盖 */
				if (ble_hid_send_buttons(btn_sent, 0, 0) == 0) {
					btn_repeat--;
					btn_repeat_at = k_uptime_get() + BTN_REPEAT_MS;
					btn_heartbeat_at = k_uptime_get() + BTN_HEARTBEAT_MS;
				} else {
					btn_repeat = 0;
				}
			} else if (BTN_HEARTBEAT_MS > 0 &&
				   k_uptime_get() >= btn_heartbeat_at &&
				   (btn_sent != 0 ||
				    k_uptime_get() < btn_release_until)) {
				/* 按住期间/松开后补发当前状态：不依赖 500 ms 保活包，
				 * 丢几包也能在 20 ms 内重新收敛。 */
				(void)ble_hid_send_buttons(btn_sent, 0, 0);
				btn_heartbeat_at = k_uptime_get() + BTN_HEARTBEAT_MS;
			}
		}

		/* ---- 3 分钟无操作 -> 自动休眠（移动 / 任意键 / 滚轮可唤醒） ----
		 * 放在滚轮处理之后，避免"刚好在滚动"的那一跳被误判为空闲。
		 * 注意**不做** paw3395_sleep()：那会让传感器 shutdown、彻底失去移动检测；
		 * 自动休眠只停 BLE 和停止轮询，传感器自己会进 rest 模式（低功耗但仍检测运动）。
		 */
		/* 插着 USB（充电）时不自动休眠：否则呼吸灯 3 分钟后就灭了，
		 * 用户会以为"充电指示坏了"。充电时功耗由 USB 供，不必省电。 */
		if (!led_charging && (k_uptime_get() - last_activity) >= IDLE_SLEEP_MS) {
			printk("pwr: idle %u ms -> auto sleep\n",
			       (unsigned int)(k_uptime_get() - last_activity));

			ble_hid_stop();

			/* 自动休眠期间不再回写 AWAKE：允许芯片一路降到 Rest2/Rest3，
			 * 换更低的静态电流。唤醒本来就要等 BLE 重连，多 200 ms 的
			 * 传感器延迟感知不明显（见上面的唤醒分支）。 */
#if SENSOR_REST_ENABLED
			(void)paw3395_set_awake(false);
			sensor_resting = true;
			sensor_rest_at = k_uptime_get();
#endif

			/* 只停 BLE / 灭灯；传感器留在 rest 模式，靠 MOTION 中断唤醒。
			 * 中断在开机时就已常驻使能（诊断用），这里只清掉可能积压的信号量。 */
			k_sem_reset(&wake_sem);
			led_all_off();
			leds_on = -1;

			sleeping = true;
			app_sleeping = true;
			sleep_manual = false;
			sleep_seen_high = 0;
			sleep_pressed = false;
			pwr_pressed = false;
			wheel_rem = 0;
			continue;
		}

		/* 状态行：每 3 s 固定输出一次。
		 *
		 * 打印本身会阻塞（三行 ~19 ms），会吃掉约 0.6% 的上报；
		 * 诊断阶段优先保证"运动中也能看到数"，所以这里不做延迟打印。
		 */
		if ((now_ms - last_status_ms) >= STATUS_EVERY_MS) {
			int64_t now = k_uptime_get();
			uint32_t el = (uint32_t)(now - bot_last_ms);

			if (el == 0) {
				el = 1;
			}

#if APP_VERBOSE_LOG
			printk("  hid: sent %u / busy %u, rd %u mot %u, pend %d,%d in %u ms -> loop %u Hz, send %u Hz, rest=%d\n",
			       rep_ok, rep_busy, frame_cnt, motion_cnt,
			       (int)pend_x, (int)pend_y, el,
			       (unsigned int)(iters * 1000U / el),
			       (unsigned int)(rep_ok * 1000U / el),
			       (int)sensor_resting);
#endif
			if ((now_ms - last_full_ms) >= 30000) {
				last_full_ms = now_ms;
#if APP_VERBOSE_LOG
				printk("  btn: ok %u fail %u latch %u i2c_err %u\n",
				       btn_ok, btn_fail, btn_latch_cnt,
				       input_button_read_errors());
				printk("  guard: lift %u stale %u spike %u\n",
				       guard_lift, guard_stale, guard_spike);
				printk("status: led=%s, sensor=%s, ble=%s, dpi=%u, bat=%d%%, counts x=%d y=%d, squal=0x%02X\n",
				       leds_on >= 0 ? led_names[leds_on] : "off",
				       sensor_ok ? "ok" : "absent",
				       ble_hid_connected() ? "connected" : "advertising",
				       MOUSE_FIXED_CPI, bat_pct,
				       sum_x, sum_y,
				       sensor_ok ? paw3395_read_reg(PAW3395_REG_SQUAL) : 0);
#endif
			}

			bot_last_ms = now;
			last_status_ms = now;
			iters = 0;
			rep_ok = 0;
			rep_busy = 0;
			frame_cnt = 0;
			motion_cnt = 0;
		}

		iters++;

		/* 事件驱动节拍：
		 *   本轮读到了位移 -> 不睡，立刻下一轮（传感器还在持续出数，抢时间）
		 *   本轮没位移   -> 挂起等唤醒：MOTION / 滚轮 / 按键 INT 三个中断都会
		 *                   give 信号量，所以真实唤醒几乎无延迟，超时只是兜底。
		 * 这样空闲时 CPU 真正进 WFI，移动时也不会被 k_msleep(1) 的 1.2 ms 量化卡住。
		 */
		if (!motion_seen) {
			/* 事件驱动节拍，按"当前还要做什么"选超时：
			 *   - 运动窗口内：250 us，保证 1 ms 发送节拍不被量化到 2 ms
			 *   - 灯效动画中：20 ms，肉眼连续，又不会占满 CPU
			 *   - 按键去抖 / 点击补发 / 滚轮重试：2 ms
			 *   - 纯空闲：200 ms（MOTION / 按键 / 滚轮中断都会 give 信号量，
			 *     这个超时只服务保活包、电量读取和状态行节流）
			 * 原来空闲固定 1 ms 会让 MCU 每秒醒 1000 次，白白多耗电。 */
			int64_t wake_now = k_uptime_get();
			k_timeout_t wait;
			bool btn_heartbeat_wait = false;

#if defined(CONFIG_APP_ESB)
			/* 按住/松开补发窗口内要保持 2 ms 节拍，心跳才能每 20 ms 发出去 */
			btn_heartbeat_wait = (btn_sent != 0) || (wake_now < btn_release_until);
#endif

			if ((wake_now - last_delta_ms) < REPORT_ACTIVE_MS) {
				wait = K_USEC(250);
			} else if (pwr_hint != PWR_HINT_NONE || bat_warn_active) {
				wait = K_MSEC(LED_ANIM_POLL_MS);
			} else if (btn_latch || btn_repeat || wheel_q_count > 0 ||
				   (btn_settle_until > wake_now) ||
				   input_btn_int_asserted() || btn_heartbeat_wait) {
				wait = K_MSEC(BTN_SETTLE_POLL_MS);
			} else {
				wait = K_MSEC(BTN_POLL_MS);
			}
			(void)k_sem_take(&wake_sem, wait);
		}
	}

	return 0;
}
