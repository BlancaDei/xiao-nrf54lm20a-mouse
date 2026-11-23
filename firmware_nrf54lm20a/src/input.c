/*
 * 输入层实现：滚轮编码器（正交解码）+ PCF8574T 按键扩展
 *
 * 编码器：A/B 都配成输入 + 双沿中断，共用一个回调，靠标准正交解码状态表
 *         累加 ±1/0。不用定时器去抖 —— 状态表本身能抵消机械抖动。
 * 按键：PCF8574 走 i2c22，先写 0xFF（准双向口置输入）再读端口；
 *       连续 3 次相同读值才接受为新状态。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>

#include "input.h"

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

/* 滚轮编码器 A/B（正交）与按键扩展 INT，均来自 app.overlay 的 zephyr,user */
static const struct gpio_dt_spec wheel_a =
	GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, wheel_a_gpios);
static const struct gpio_dt_spec wheel_b =
	GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, wheel_b_gpios);
static const struct gpio_dt_spec btn_int =
	GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, btn_int_gpios);

/* PCF8574T 挂在 XIAO 外部 I2C（D4/D5 = i2c22） */
static const struct device *const pcf_i2c = DEVICE_DT_GET(DT_NODELABEL(i2c22));

/* PCF8574T 默认地址 0x20（A0/A1/A2 全接地）。
 * 若模块跳线不同，需按丝印改这里。
 */
#define PCF8574_ADDR          0x20

/* 去抖：连续 N 次相同读值才接受为新状态。
 * N 由配置通道的"防误触发"档位改（默认 3 = 低档）。
 */
#define BTN_DEBOUNCE_N        3
#define BTN_DEBOUNCE_N_MAX    10

static uint8_t debounce_n = BTN_DEBOUNCE_N;

/* 编码器累积的正交步数（4 步 = 1 机械格）；ISR 累加、主循环取走清零 */
static int32_t wheel_steps;
static uint8_t enc_state;

/* 正交解码状态表：index = (prev << 2) | cur，(prev,cur) 均为 (A<<1)|B。
 * 4x 分辨率：一个机械刻度 = 4 个状态变化。
 */
static const int8_t enc_table[16] = {
	/*        cur:  0    1    2    3 */
	/* prev 0 */  0,   1,  -1,   0,
	/* prev 1 */ -1,   0,   0,   1,
	/* prev 2 */  1,   0,   0,  -1,
	/* prev 3 */  0,  -1,   1,   0,
};

/* 按键去抖状态：candidate = 最近一次读值，stable = 连续命中次数 */
static uint8_t btn_candidate = 0xFF;
static uint8_t btn_stable;
static uint8_t btn_confirmed = 0xFF;

/* I2C 读失败计数：失败时 btn_confirmed 会保持旧值，上层据此做超时释放 */
static uint32_t read_errors;

static struct gpio_callback wheel_cb;
static struct gpio_callback btn_int_cb;

/* 唤醒通知：ISR 里只做 k_sem_give 这类极短动作 */
static void (*wake_notify)(void);

void input_set_wake_notify(void (*fn)(void))
{
	wake_notify = fn;
}

static inline void notify_wake(void)
{
	if (wake_notify != NULL) {
		wake_notify();
	}
}

/* 读 A/B 逻辑电平并打包成 (A<<1)|B */
static inline uint8_t wheel_read_ab(void)
{
	uint8_t a = (uint8_t)gpio_pin_get_dt(&wheel_a);
	uint8_t b = (uint8_t)gpio_pin_get_dt(&wheel_b);

	return (uint8_t)((a << 1) | b);
}

static void wheel_isr(const struct device *dev, struct gpio_callback *cb,
		      uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	uint8_t cur = wheel_read_ab();

	wheel_steps += enc_table[(enc_state << 2) | cur];
	enc_state = cur;

	notify_wake();
}

/* PCF8574 的 INT：端口有变化就拉低，主循环读一次后释放。
 * 下降沿 = 有按键动作，只负责把主线程叫醒，真正的去抖仍走 I2C 读。 */
static void btn_int_isr(const struct device *dev, struct gpio_callback *cb,
			uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	notify_wake();
}

int input_wheel_take(void)
{
	unsigned int key = irq_lock();
	int32_t v = wheel_steps;

	wheel_steps = 0;
	irq_unlock(key);

	return (int)v;
}

int input_buttons_read(uint8_t *raw)
{
	uint8_t wr = 0xFF;   /* 准双向口：写 1 置为输入 */
	uint8_t rd = 0xFF;
	int err;

	if (!device_is_ready(pcf_i2c)) {
		return -ENODEV;
	}

	err = i2c_write_read(pcf_i2c, PCF8574_ADDR, &wr, 1, &rd, 1);
	if (err < 0) {
		read_errors++;
		return err;
	}

	if (raw) {
		*raw = rd;
	}

	/* 去抖：连续 BTN_DEBOUNCE_N 次相同才确认 */
	if (rd == btn_candidate) {
		if (btn_stable < debounce_n) {
			btn_stable++;
		}
	} else {
		btn_candidate = rd;
		btn_stable = 1;
	}
	if (btn_stable >= debounce_n) {
		btn_confirmed = btn_candidate;
	}

	return 0;
}

void input_set_debounce_count(uint8_t n)
{
	if (n < 2) {
		n = 2;
	}
	if (n > BTN_DEBOUNCE_N_MAX) {
		n = BTN_DEBOUNCE_N_MAX;
	}

	debounce_n = n;
	/* 换档后重新计数，避免旧状态直接被判定成"已稳定" */
	btn_stable = 0;
	btn_candidate = btn_confirmed;

	printk("input: debounce count -> %u\n", n);
}

uint8_t input_debounce_count(void)
{
	return debounce_n;
}

int input_buttons_get(uint8_t *debounced)
{
	if (debounced) {
		*debounced = btn_confirmed;
	}

	return 0;
}

uint32_t input_button_read_errors(void)
{
	return read_errors;
}

void input_buttons_force_release(void)
{
	/* I2C 长时间读失败时调用：把确认状态强制拉回"全松开"，
	 * 避免按键永久卡在按下。下一次成功读会重新走去抖确认。 */
	btn_confirmed = 0xFF;
	btn_candidate = 0xFF;
	btn_stable = 0;
}

bool input_btn_int_asserted(void)
{
	if (!gpio_is_ready_dt(&btn_int)) {
		return false;
	}

	/* GPIO_ACTIVE_LOW：INT 拉低时逻辑值为 1 */
	return gpio_pin_get_dt(&btn_int) != 0;
}

int input_init(void)
{
	int ready = 0;

	/* 编码器 A/B：输入 + 双沿中断，共用一个回调 */
	if (gpio_is_ready_dt(&wheel_a) && gpio_is_ready_dt(&wheel_b)) {
		int err = gpio_pin_configure_dt(&wheel_a, GPIO_INPUT);

		if (err == 0) {
			err = gpio_pin_configure_dt(&wheel_b, GPIO_INPUT);
		}
		if (err == 0) {
			/* 先记录初始电平，避免使能中断后的第一次边沿被误判 */
			enc_state = wheel_read_ab();
			gpio_init_callback(&wheel_cb, wheel_isr,
					   BIT(wheel_a.pin) | BIT(wheel_b.pin));
			err = gpio_add_callback(wheel_a.port, &wheel_cb);
		}
		if (err == 0) {
			err = gpio_pin_interrupt_configure_dt(&wheel_a,
							      GPIO_INT_EDGE_BOTH);
		}
		if (err == 0) {
			err = gpio_pin_interrupt_configure_dt(&wheel_b,
							      GPIO_INT_EDGE_BOTH);
		}
		if (err) {
			printk("input: wheel encoder init failed (%d)\n", err);
		} else {
			ready++;
		}
	} else {
		printk("input: wheel gpio not ready\n");
	}

	/* 按键扩展 INT（D7）：输入 + 下降沿中断。
	 * 有了这条中断，睡眠态可以 k_sem_take(K_FOREVER)，不必再定时轮询按键。
	 */
	if (gpio_is_ready_dt(&btn_int)) {
		int err = gpio_pin_configure_dt(&btn_int, GPIO_INPUT);

		if (err) {
			printk("input: btn int init failed (%d)\n", err);
		} else {
			gpio_init_callback(&btn_int_cb, btn_int_isr, BIT(btn_int.pin));
			err = gpio_add_callback(btn_int.port, &btn_int_cb);
			if (err == 0) {
				err = gpio_pin_interrupt_configure_dt(&btn_int,
								      GPIO_INT_EDGE_FALLING);
			}
			if (err) {
				printk("input: btn int irq failed (%d)\n", err);
			}
			ready++;
		}
	} else {
		printk("input: btn int gpio not ready\n");
	}

	/* PCF8574 挂在 i2c22（D4/D5） */
	if (!device_is_ready(pcf_i2c)) {
		printk("input: i2c22 not ready\n");
	} else {
		uint8_t raw = 0;

		if (input_buttons_read(&raw) == 0) {
			printk("input: pcf8574 @0x%02X ok, initial raw=0x%02X\n",
			       PCF8574_ADDR, raw);
		} else {
			printk("input: pcf8574 @0x%02X read failed\n",
			       PCF8574_ADDR);
		}
		ready++;
	}

	return ready > 0 ? 0 : -ENODEV;
}
