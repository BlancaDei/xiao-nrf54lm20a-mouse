/*
 * PAW3395 SPI 驱动实现
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/printk.h>
#include <errno.h>
#include <string.h>

#include "paw3395.h"

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

static const struct device *const spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi23));

static const struct gpio_dt_spec ncs =
	GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, paw_ncs_gpios);
static const struct gpio_dt_spec nreset =
	GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, paw_nreset_gpios);
static const struct gpio_dt_spec motion =
	GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, paw_motion_gpios);

/* PixArt 4-wire SPI：mode 3（CPOL=1/CPHA=1），MSB first，8 bit。
 * 频率取 8 MHz —— 手册上限 10 MHz，留 20% 余量保证信号质量。
 * （1 kHz 上报时每次读位移都在这条总线上，2 MHz 的几十微秒会明显挤占循环时间。）
 */
static const struct spi_config spi_cfg = {
	.frequency = 8000000,
	.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB |
		     SPI_MODE_CPOL | SPI_MODE_CPHA,
	.slave = 0,
};

/* ncs 是 GPIO_ACTIVE_LOW：set(1) = 拉低 = 选中 */
static inline void cs_assert(void)
{
	gpio_pin_set_dt(&ncs, 1);
}

static inline void cs_release(void)
{
	gpio_pin_set_dt(&ncs, 0);
}

static int spi_tx(const uint8_t *buf, size_t len)
{
	struct spi_buf tx = { .buf = (void *)buf, .len = len };
	struct spi_buf_set txs = { .buffers = &tx, .count = 1 };

	return spi_write(spi_dev, &spi_cfg, &txs);
}

static int spi_rx(uint8_t *buf, size_t len)
{
	struct spi_buf rx = { .buf = buf, .len = len };
	struct spi_buf_set rxs = { .buffers = &rx, .count = 1 };

	return spi_read(spi_dev, &spi_cfg, &rxs);
}

int paw3395_write_reg(uint8_t reg, uint8_t val)
{
	uint8_t cmd[2] = { (uint8_t)(reg | 0x80), val };
	int err;

	cs_assert();
	err = spi_tx(cmd, sizeof(cmd));
	/* tSCLK-NCS(write) >= 1us，写操作 CS 必须在时钟后保持至少 1us 再抬高 */
	k_busy_wait(2);
	cs_release();
	/* tSWW >= 5us：两条写命令之间 */
	k_busy_wait(6);

	return err;
}

uint8_t paw3395_read_reg(uint8_t reg)
{
	uint8_t addr = reg & 0x7F;
	uint8_t val = 0;

	cs_assert();
	(void)spi_tx(&addr, 1);
	/* tSRAD >= 2us：地址字节最后一个上升沿到数据第一位 */
	k_busy_wait(2);
	(void)spi_rx(&val, 1);
	cs_release();
	/* tSRW/tSRR >= 2us：读命令之后到下一命令 */
	k_busy_wait(3);

	return val;
}

int paw3395_probe(uint8_t *product_id, uint8_t *revision_id)
{
	if (!device_is_ready(spi_dev)) {
		return -ENODEV;
	}

	if (product_id) {
		*product_id = paw3395_read_reg(PAW3395_REG_PRODUCT_ID);
	}
	if (revision_id) {
		*revision_id = paw3395_read_reg(PAW3395_REG_REVISION_ID);
	}

	return 0;
}

int paw3395_spi_loopback(uint8_t *rx)
{
	uint8_t tx = 0xA5;
	uint8_t buf = 0;
	struct spi_buf txb = { .buf = &tx, .len = 1 };
	struct spi_buf rxb = { .buf = &buf, .len = 1 };
	struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };
	int err;

	/* 保持 NCS 不选中，纯测 MCU 侧 D9<->D10 是否被短接 */
	cs_release();
	err = spi_transceive(spi_dev, &spi_cfg, &txs, &rxs);
	if (rx) {
		*rx = buf;
	}

	return err;
}

int paw3395_set_cpi(uint16_t cpi)
{
	uint16_t lsb;   /* 分辨率单位 50 cpi/LSB（datasheet §8.2） */

	if (cpi < PAW3395_CPI_MIN) {
		cpi = PAW3395_CPI_MIN;
	}
	if (cpi > PAW3395_CPI_MAX) {
		cpi = PAW3395_CPI_MAX;
	}

	lsb = (uint16_t)((cpi + 25) / 50);
	if (lsb == 0) {
		lsb = 1;
	}

	/* 手册要求：先写 X 低/高，再写 Y 低/高，最后写 0x47=0x01 才生效 */
	int err = paw3395_write_reg(PAW3395_REG_RES_X_L, (uint8_t)(lsb & 0xff));

	if (!err) {
		err = paw3395_write_reg(PAW3395_REG_RES_X_H, (uint8_t)(lsb >> 8));
	}
	if (!err) {
		err = paw3395_write_reg(PAW3395_REG_RES_Y_L, (uint8_t)(lsb & 0xff));
	}
	if (!err) {
		err = paw3395_write_reg(PAW3395_REG_RES_Y_H, (uint8_t)(lsb >> 8));
	}
	if (!err) {
		err = paw3395_write_reg(PAW3395_REG_SET_RESOLUTION, 0x01);
	}

	printk("paw3395 cpi=%u -> res 0x%04X (= %u cpi)\n",
	       cpi, lsb, (unsigned int)lsb * 50U);

	return err;
}

void paw3395_scaler_init(struct paw3395_scaler *s, uint16_t target_cpi)
{
	if (target_cpi < PAW3395_CPI_MIN) {
		target_cpi = PAW3395_CPI_MIN;
	}
	if (target_cpi > PAW3395_CPI_MAX) {
		target_cpi = PAW3395_CPI_MAX;
	}

	s->num = target_cpi;
	s->rem = 0;
}

int16_t paw3395_scaler_apply(struct paw3395_scaler *s, int16_t raw)
{
	int32_t t;
	int32_t out;

	/* 目标等于实测基准时直通，省掉一次没意义的乘除与量化 */
	if (s->num == PAW3395_CPI_MEASURED) {
		return raw;
	}

	t = (int32_t)raw * (int32_t)s->num + s->rem;
	/* C 的整除/取余都向 0 截断，正负两侧自洽 */
	s->rem = t % (int32_t)PAW3395_CPI_MEASURED;
	out = t / (int32_t)PAW3395_CPI_MEASURED;

	/* 放大档（目标 > 基准）时单次折算可能超出 int16，先饱和再交给上层；
	 * 否则截断回绕会让指针朝反方向跳。
	 */
	if (out > INT16_MAX) {
		out = INT16_MAX;
	}
	if (out < INT16_MIN) {
		out = INT16_MIN;
	}

	return (int16_t)out;
}

int paw3395_read_burst_raw(int16_t *dx, int16_t *dy, uint8_t raw[PAW3395_BURST_LEN])
{
	uint8_t addr = PAW3395_REG_MOTION_BURST; /* 0x16，不带 0x80 */
	uint8_t buf[PAW3395_BURST_LEN] = { 0 };
	int err;

	/* datasheet v0.8 §5.7.2：
	 *   1) NCS 拉低
	 *   2) 等 tNCS-SCLK(120ns)
	 *   3) 发地址 0x16，之后 MOSI 保持静态（这里发完就只收）
	 *   4) 等 tSRAD(2us)
	 *   5) 连续读最多 12 字节
	 *   6) NCS 拉高 >= tBEXIT(500ns) 结束 burst
	 * 注意：读 0x16 本身就进入 burst，**不需要**先写 0x16=0x00
	 * （旧代码那一步是错的，很可能就是之前读回全 0xFF 的原因）。
	 */
	cs_assert();
	k_busy_wait(1);        /* tNCS-SCLK */
	err = spi_tx(&addr, 1);
	if (!err) {
		k_busy_wait(2);    /* tSRAD */
		err = spi_rx(buf, sizeof(buf));
	}
	cs_release();
	k_busy_wait(1);        /* tBEXIT */

	if (err) {
		return err;
	}

	if (raw) {
		memcpy(raw, buf, sizeof(buf));
	}

	*dx = (int16_t)((buf[PAW3395_BURST_OFF_DX + 1] << 8) |
			buf[PAW3395_BURST_OFF_DX]);
	*dy = (int16_t)((buf[PAW3395_BURST_OFF_DY + 1] << 8) |
			buf[PAW3395_BURST_OFF_DY]);

	return 0;
}

int paw3395_read_motion(int16_t *dx, int16_t *dy, bool *overflow)
{
	uint8_t m = paw3395_read_reg(PAW3395_REG_MOTION);

	if (overflow) {
		/*
		 * Motion 寄存器 bit7 = MOT（自上次读取以来有位移）。
		 * 溢出标志位在公开手册里没有明确给出 bit 位置，
		 * burst 读的 12 字节里也没有留出位，暂时不判定。
		 */
		*overflow = false;
	}

	if ((m & 0x80) == 0) {
		*dx = 0;
		*dy = 0;
		return 0;
	}

	uint8_t xl = paw3395_read_reg(PAW3395_REG_DELTA_X_L);
	uint8_t xh = paw3395_read_reg(PAW3395_REG_DELTA_X_H);
	uint8_t yl = paw3395_read_reg(PAW3395_REG_DELTA_Y_L);
	uint8_t yh = paw3395_read_reg(PAW3395_REG_DELTA_Y_H);

	*dx = (int16_t)((xh << 8) | xl);
	*dy = (int16_t)((yh << 8) | yl);

	return 0;
}

void paw3395_reg_dump(void)
{
	printk("paw3395 regs 0x00..0x3F:\n");

	for (uint8_t base = 0; base < 0x40; base += 8) {
		char line[8 * 3 + 6];
		int n = 0;

		n += snprintf(line + n, sizeof(line) - n, "  %02X:", base);
		for (uint8_t i = 0; i < 8; i++) {
			n += snprintf(line + n, sizeof(line) - n, " %02X",
				      paw3395_read_reg(base + i));
		}
		printk("%s\n", line);
		/* 控制台 115200，连续打印太快会被日志后端丢消息，逐行让出 */
		k_msleep(40);
	}
}

void paw3395_cpi_probe(void)
{
	static const uint8_t cands[] = { 0x0D, 0x0E, 0x0F, 0x10, 0x11 };

	printk("paw3395 CPI register write-probe:\n");

	for (size_t i = 0; i < ARRAY_SIZE(cands); i++) {
		uint8_t r = cands[i];
		uint8_t old = paw3395_read_reg(r);
		uint8_t a;
		uint8_t b;

		(void)paw3395_write_reg(r, 0x40);
		a = paw3395_read_reg(r);
		(void)paw3395_write_reg(r, 0x50);
		b = paw3395_read_reg(r);
		(void)paw3395_write_reg(r, old);   /* 恢复原值 */

		printk("  reg 0x%02X: old=0x%02X write40->0x%02X write50->0x%02X %s\n",
		       r, old, a, b,
		       (a == 0x40 && b == 0x50) ? "WRITABLE" : "not-writable");
	}
}

/* 复位后收尾：写 Power_Up_Reset、等待、丢弃复位后的前几帧位移、设 CPI、
 * 进入 burst。开机初始化与唤醒都要走这一步，故抽成内部函数，避免两处复制。
 */
/* ---- 厂商上电初始化序列（datasheet v0.8 §6.2，共 122 条写入） ----
 * 每条是 {bank, reg, val}：bank != 0 时先写 0x7F=bank，写完切回 0。
 * 原文的变量/等待步骤（§6.2 第 138/139 步）在 paw3395_power_up_init() 里单独实现。
 */
struct paw_init_entry {
	uint8_t bank;
	uint8_t reg;
	uint8_t val;
};

static const struct paw_init_entry paw_init_seq[] = {
	{ 0x07, 0x40, 0x41 }, { 0x00, 0x40, 0x80 }, { 0x0E, 0x55, 0x0D },
	{ 0x0E, 0x56, 0x1B }, { 0x0E, 0x58, 0xD5 }, { 0x14, 0x42, 0xBC },
	{ 0x14, 0x43, 0x74 }, { 0x14, 0x4B, 0x20 }, { 0x14, 0x4D, 0x00 },
	{ 0x14, 0x53, 0x0E }, { 0x05, 0x44, 0x04 }, { 0x05, 0x4D, 0x06 },
	{ 0x05, 0x51, 0x40 }, { 0x05, 0x53, 0x40 }, { 0x05, 0x55, 0xCA },
	{ 0x05, 0x5B, 0xEA }, { 0x05, 0x61, 0x31 }, { 0x05, 0x62, 0x64 },
	{ 0x05, 0x6D, 0xB8 }, { 0x05, 0x6E, 0x0F }, { 0x05, 0x70, 0x02 },
	{ 0x05, 0x4A, 0x2A }, { 0x05, 0x60, 0x26 }, { 0x06, 0x6D, 0x70 },
	{ 0x06, 0x6E, 0x60 }, { 0x06, 0x6F, 0x04 }, { 0x06, 0x53, 0x02 },
	{ 0x06, 0x55, 0x11 }, { 0x06, 0x7A, 0x01 }, { 0x06, 0x7D, 0x51 },
	{ 0x07, 0x41, 0x10 }, { 0x07, 0x42, 0x32 }, { 0x07, 0x43, 0x00 },
	{ 0x08, 0x71, 0x4F }, { 0x09, 0x62, 0x1F }, { 0x09, 0x63, 0x1F },
	{ 0x09, 0x65, 0x03 }, { 0x09, 0x66, 0x03 }, { 0x09, 0x67, 0x1F },
	{ 0x09, 0x68, 0x1F }, { 0x09, 0x69, 0x03 }, { 0x09, 0x6A, 0x03 },
	{ 0x09, 0x6C, 0x1F }, { 0x09, 0x6D, 0x1F }, { 0x09, 0x51, 0x04 },
	{ 0x09, 0x53, 0x20 }, { 0x09, 0x54, 0x20 }, { 0x09, 0x71, 0x0C },
	{ 0x09, 0x72, 0x07 }, { 0x09, 0x73, 0x07 }, { 0x0A, 0x4A, 0x14 },
	{ 0x0A, 0x4C, 0x14 }, { 0x0A, 0x55, 0x19 }, { 0x14, 0x4B, 0x30 },
	{ 0x14, 0x4C, 0x03 }, { 0x14, 0x61, 0x0B }, { 0x14, 0x62, 0x0A },
	{ 0x14, 0x63, 0x02 }, { 0x14, 0x4C, 0x02 }, { 0x14, 0x56, 0x02 },
	{ 0x14, 0x41, 0x91 }, { 0x14, 0x4D, 0x0A }, { 0x0C, 0x4A, 0x10 },
	{ 0x0C, 0x4C, 0x40 }, { 0x0C, 0x41, 0x25 }, { 0x0C, 0x55, 0x18 },
	{ 0x0C, 0x56, 0x14 }, { 0x0C, 0x49, 0x0A }, { 0x0C, 0x42, 0x00 },
	{ 0x0C, 0x43, 0x2D }, { 0x0C, 0x44, 0x0C }, { 0x0C, 0x54, 0x1A },
	{ 0x0C, 0x5A, 0x0D }, { 0x0C, 0x5F, 0x1E }, { 0x0C, 0x5B, 0x05 },
	{ 0x0C, 0x5E, 0x0F }, { 0x0D, 0x48, 0xDD }, { 0x0D, 0x4F, 0x03 },
	{ 0x0D, 0x52, 0x49 }, { 0x0D, 0x51, 0x00 }, { 0x0D, 0x54, 0x5B },
	{ 0x0D, 0x53, 0x00 }, { 0x0D, 0x56, 0x64 }, { 0x0D, 0x55, 0x00 },
	{ 0x0D, 0x58, 0xA5 }, { 0x0D, 0x57, 0x02 }, { 0x0D, 0x5A, 0x29 },
	{ 0x0D, 0x5B, 0x47 }, { 0x0D, 0x5C, 0x81 }, { 0x0D, 0x5D, 0x40 },
	{ 0x0D, 0x71, 0xDC }, { 0x0D, 0x70, 0x07 }, { 0x0D, 0x73, 0x00 },
	{ 0x0D, 0x72, 0x08 }, { 0x0D, 0x75, 0xDC }, { 0x0D, 0x74, 0x07 },
	{ 0x0D, 0x77, 0x00 }, { 0x0D, 0x76, 0x08 }, { 0x10, 0x4C, 0xD0 },
	{ 0x00, 0x4F, 0x63 }, { 0x00, 0x4E, 0x00 }, { 0x00, 0x52, 0x63 },
	{ 0x00, 0x51, 0x00 }, { 0x00, 0x54, 0x54 }, { 0x00, 0x5A, 0x10 },
	{ 0x00, 0x77, 0x4F }, { 0x00, 0x47, 0x01 }, { 0x00, 0x5B, 0x40 },
	{ 0x00, 0x64, 0x60 }, { 0x00, 0x65, 0x06 }, { 0x00, 0x66, 0x13 },
	{ 0x00, 0x67, 0x0F }, { 0x00, 0x78, 0x01 }, { 0x00, 0x79, 0x9C },
	{ 0x00, 0x40, 0x00 }, { 0x00, 0x55, 0x02 }, { 0x00, 0x23, 0x70 },
	{ 0x00, 0x22, 0x01 },
};

static int paw_write_bank(uint8_t bank, uint8_t reg, uint8_t val)
{
	int err = 0;

	if (bank != 0) {
		err = paw3395_write_reg(PAW3395_REG_BANK, bank);
	}
	if (!err) {
		err = paw3395_write_reg(reg, val);
	}
	if (bank != 0) {
		(void)paw3395_write_reg(PAW3395_REG_BANK, 0x00);
	}

	return err;
}

/* ---- datasheet §7.5.3：抬升检测的"通用 1 mm"配置 ----
 * 我们为了保住追踪，把 §6.2 的 122 步上电序列整体跳过了，导致这些 bank 0x0C
 * 的抬升参数一直停在复位值上，抬升检测实际上是不完整的（实测：鼠标抬起来
 * 仍能推动光标）。这一组就是手册在 §7.5.1 失败回退、§7.5.3 关标定时写的
 * "revert to default universal 1 mm lift cut off setting"。
 * 0x4E(LIFT_CONFIG) 的 bit[1:0] = LIFT：0 = 1 mm，2 = 2 mm（其余位保持 0x08）。
 * 0x41/0x43/0x44 是非标定表面的默认 VarA/VarC/VarB（0x25/0x2D/0x0C）。
 */
static const struct paw_init_entry paw_lift_1mm_seq[] = {
	{ 0x0C, 0x41, 0x25 }, { 0x0C, 0x43, 0x2D }, { 0x0C, 0x44, 0x0C },
	{ 0x0C, 0x4A, 0x10 }, { 0x0C, 0x4B, 0x0C }, { 0x0C, 0x4C, 0x40 },
	{ 0x0C, 0x53, 0x16 }, { 0x0C, 0x54, 0x1A },
	{ 0x0C, 0x55, 0x18 }, { 0x0C, 0x56, 0x14 }, { 0x0C, 0x5A, 0x0D },
	{ 0x0C, 0x5B, 0x05 }, { 0x0C, 0x5F, 0x1E }, { 0x0C, 0x66, 0x30 },
	{ 0x05, 0x6E, 0x0F }, { 0x09, 0x71, 0x0C }, { 0x09, 0x72, 0x07 },
};

static int paw3395_apply_lift_cfg(uint8_t lift_cfg)
{
	int err = 0;

	for (size_t i = 0; i < ARRAY_SIZE(paw_lift_1mm_seq); i++) {
		err = paw_write_bank(paw_lift_1mm_seq[i].bank,
				     paw_lift_1mm_seq[i].reg,
				     paw_lift_1mm_seq[i].val);
		if (err) {
			printk("paw3395: lift cfg step %u failed (%d)\n",
			       (unsigned int)i, err);
			return err;
		}
	}

	/* LIFT_CONFIG 单独写：bit[1:0] 决定 1 mm / 2 mm */
	err = paw_write_bank(0x0C, PAW3395_REG_LIFT_CONFIG, lift_cfg);
	if (err) {
		return err;
	}

	/* LIFT_CONFIG 在 bank 0x0C，回读要切 bank */
	(void)paw3395_write_reg(PAW3395_REG_BANK, 0x0C);
	uint8_t lift = paw3395_read_reg(PAW3395_REG_LIFT_CONFIG);

	(void)paw3395_write_reg(PAW3395_REG_BANK, 0x00);

	printk("paw3395 lift cut-off: %s (0x4E=0x%02X)\n",
	       (lift_cfg & 0x03) == 0 ? "1mm" : "2mm", lift);

	return 0;
}

int paw3395_lift_cutoff_set(uint8_t level)
{
	/* LIFT[1:0]：0 = 1mm（低），2 = 2mm（中/高，硬件只有两档） */
	uint8_t lift_cfg = (level == 0) ? 0x08 : 0x0A;

	return paw3395_apply_lift_cfg(lift_cfg);
}

/* 跑完 §6.2：表 + 138/139 步（等 1 ms，1 ms 间隔读 0x6C 直到 0x80）+ 140~144 步 */
static int paw3395_power_up_init(void)
{
	int err = 0;

	for (size_t i = 0; i < ARRAY_SIZE(paw_init_seq); i++) {
		err = paw_write_bank(paw_init_seq[i].bank, paw_init_seq[i].reg,
				     paw_init_seq[i].val);
		if (err) {
			printk("paw3395: init step %u failed (%d)\n", (unsigned int)i, err);
			return err;
		}
	}

	k_msleep(1);

	bool fw_ok = false;

	for (int i = 0; i < 60; i++) {
		if (paw3395_read_reg(0x6C) == 0x80) {
			fw_ok = true;
			break;
		}
		k_msleep(1);
	}

	if (!fw_ok) {
		/* 手册规定的兜底动作：清 0x6C（bank 0x14） */
		printk("paw3395: 0x6C fw check timeout, fallback\n");
		(void)paw_write_bank(0x14, 0x6C, 0x00);
	}

	(void)paw_write_bank(0x00, 0x22, 0x00);
	(void)paw_write_bank(0x00, 0x55, 0x00);
	(void)paw_write_bank(0x07, 0x40, 0x40);

	return 0;
}

/* ---- 0x17 帧率实验 ----
 * 公开映射里 0x17 是 Frame Rate（帧周期，值越小帧率越高），但 PAW3395 的
 * 真实寄存器表在 NDA 附录里，所以这里按"实验"对待：
 *   1) 写入 PAW3395_FRAME_RATE_VALUE，回读校验是否可写；
 *   2) 若写完后 SQUAL 连续两次为 0（传感器不再成像），立刻恢复原值。
 * 效果要靠主循环的 send Hz 判断 —— 帧率若不是瓶颈，上报率不会有任何变化。
 */
#if PAW3395_FRAME_RATE_EXPERIMENT
static void paw3395_apply_frame_rate(void)
{
	uint8_t old = paw3395_read_reg(PAW3395_REG_FRAME_RATE);
	uint8_t rb;
	int err;

	/* 已经是目标值就什么都不做（回退到默认值时就是这种情形） */
	if (old == PAW3395_FRAME_RATE_VALUE) {
		printk("paw3395 fr 0x17: unchanged (0x%02X)\n", old);
		return;
	}

	err = paw3395_write_reg(PAW3395_REG_FRAME_RATE, PAW3395_FRAME_RATE_VALUE);

	rb = paw3395_read_reg(PAW3395_REG_FRAME_RATE);

	if (rb != PAW3395_FRAME_RATE_VALUE) {
		printk("paw3395 fr 0x17: NOT writable (old=0x%02X rb=0x%02X err=%d)\n",
		       old, rb, err);
		return;
	}

	k_msleep(20);
	if (paw3395_read_reg(PAW3395_REG_SQUAL) == 0) {
		k_msleep(20);
		if (paw3395_read_reg(PAW3395_REG_SQUAL) == 0) {
			(void)paw3395_write_reg(PAW3395_REG_FRAME_RATE, old);
			printk("paw3395 fr 0x17=0x%02X killed imaging -> restored 0x%02X\n",
			       PAW3395_FRAME_RATE_VALUE, old);
			return;
		}
	}

	printk("paw3395 fr 0x17: 0x%02X -> 0x%02X (SQUAL 0x%02X)\n",
	       (unsigned int)old, PAW3395_FRAME_RATE_VALUE,
	       paw3395_read_reg(PAW3395_REG_SQUAL));
}
#endif /* PAW3395_FRAME_RATE_EXPERIMENT */

static int paw3395_reset_and_config(void)
{
	int err;

	/* Power_Up_Reset：写 0x3A = 0x5A */
	err = paw3395_write_reg(PAW3395_REG_POWER_UP_RESET,
				PAW3395_POWER_UP_RESET_MAGIC);
	if (err) {
		printk("paw3395: power-up reset write failed (%d)\n", err);
		return err;
	}
	/* 手册：复位后 50ms 才有有效位移 */
	k_msleep(50);

	/* 注意：datasheet §6.2 的完整上电初始化序列（paw3395_power_up_init）在本板
	 * 实测会让传感器停止出数（SQUAL 恒 0、无 counts），疑似初始化表里 LED/快门
	 * 相关项与本板光路不匹配。代码保留备查，但**默认不执行**。
	 * 目前沿用复位后的出厂默认配置 + 软件缩放。 */
#if 0
	err = paw3395_power_up_init();
	if (err) {
		return err;
	}
#endif

	err = paw3395_set_cpi(PAW3395_CPI_DEFAULT);
	if (err) {
		return err;
	}

	/* 抬升检测：先按通用 1 mm 配置，随后 main 会按保存的档位再覆盖 */
	err = paw3395_apply_lift_cfg(0x08);
	if (err) {
		return err;
	}

	/* 0x40 bit7 AWAKE = 1：复位后先回 run 档，保证追踪与 1 kHz 上报正常。
	 * datasheet §7.6 + Table 11：AWAKE=0 时闲置 1s/10s/600s 会依次降到
	 * Rest1/Rest2/Rest3，再动鼠标响应时间 2ms/200ms/1000ms。
	 * 什么时候允许降档由上层空闲策略决定（见 paw3395_set_awake）。
	 */
	err = paw3395_set_awake(true);
	if (err) {
		return err;
	}

#if PAW3395_FRAME_RATE_EXPERIMENT
	paw3395_apply_frame_rate();
#endif

	/* 丢弃复位/初始化期间的位移（手册 §5 第 7 步） */
	(void)paw3395_read_reg(PAW3395_REG_MOTION);
	(void)paw3395_read_reg(PAW3395_REG_DELTA_X_L);
	(void)paw3395_read_reg(PAW3395_REG_DELTA_X_H);
	(void)paw3395_read_reg(PAW3395_REG_DELTA_Y_L);
	(void)paw3395_read_reg(PAW3395_REG_DELTA_Y_H);

	return 0;
}

int paw3395_init(void)
{
	int err;

	if (!device_is_ready(spi_dev)) {
		printk("paw3395: spi23 not ready\n");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&ncs) || !gpio_is_ready_dt(&nreset) ||
	    !gpio_is_ready_dt(&motion)) {
		printk("paw3395: gpio not ready\n");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&ncs, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}
	err = gpio_pin_configure_dt(&nreset, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}
	err = gpio_pin_configure_dt(&motion, GPIO_INPUT);
	if (err) {
		return err;
	}

	/* 1) 硬复位：NRESET 拉低 -> 释放 */
	gpio_pin_set_dt(&nreset, 1);
	k_msleep(1);
	gpio_pin_set_dt(&nreset, 0);
	k_msleep(50);

	/* 2) NCS 先高后低（PixArt 上电要求） */
	gpio_pin_set_dt(&ncs, 0);
	k_busy_wait(40);
	gpio_pin_set_dt(&ncs, 1);
	k_busy_wait(40);

	/* 3) Power_Up_Reset 及复位后收尾（丢帧、设 CPI、进 burst） */
	return paw3395_reset_and_config();
}

bool paw3395_motion_asserted(void)
{
	/* MOTION 引脚是 GPIO_ACTIVE_LOW：传感器有未读位移时拉低该脚，经 DT 转换后
	 * 逻辑值为 1。为 1 表示"有位移待读"，可以做 SPI 读；为 0 表示空闲，
	 * 可跳过本次 SPI 读（传感器内部自己累加 counts，跳读不丢位移）。
	 */
	return gpio_pin_get_dt(&motion) != 0;
}

int paw3395_motion_irq_configure(struct gpio_callback *cb,
				 gpio_callback_handler_t handler, bool enable)
{
	int err;

	if (enable) {
		gpio_init_callback(cb, handler, BIT(motion.pin));

		err = gpio_add_callback(motion.port, cb);
		if (err) {
			return err;
		}

		/* active-low：有未读位移时拉低 -> 用下降沿唤醒 */
		return gpio_pin_interrupt_configure_dt(&motion, GPIO_INT_EDGE_FALLING);
	}

	err = gpio_pin_interrupt_configure_dt(&motion, GPIO_INT_DISABLE);
	(void)gpio_remove_callback(motion.port, cb);

	return err;
}

int paw3395_set_awake(bool awake)
{
	/* 厂商初始化表里 0x40 用过 0x00 / 0x40 / 0x41 / 0x80，这里只翻 bit7，
	 * 其余位保持复位默认值 0。 */
	int err = paw3395_write_reg(PAW3395_REG_PERFORMANCE,
				    awake ? PAW3395_PERF_AWAKE : 0x00);

	if (err) {
		printk("paw3395: set_awake(%d) failed (%d)\n", (int)awake, err);
	}

	return err;
}

int paw3395_sleep(void)
{
	/* PixArt 家族的 Shutdown 寄存器（0x3B）只认魔术字 0xB6：
	 * 写 0xB6 才真正关掉芯片内部所有模拟块（LED 驱动、成像阵列、SPI 逻辑），
	 * 电流在 500 us 内降到几十 uA；写 0x00 或任何其他值都是**空操作**，
	 * 芯片继续按正常帧率跑，LED 也照旧常亮。
	 * 进入 shutdown 后传感器不再响应正常读，唤醒必须重新走 Power_Up_Reset
	 * （datasheet 明确要求 shutdown 之后必须做一次 reset，见 paw3395_wake）。
	 */
	int err = paw3395_write_reg(PAW3395_REG_SHUTDOWN, PAW3395_SHUTDOWN_MAGIC);

	/* tSTDWN = 500 us：等模拟块真正断电后再返回，避免紧接着的
	 * 复位/重配置打在还没停稳的芯片上。 */
	if (err == 0) {
		k_msleep(1);
	}

	return err;
}

int paw3395_wake(void)
{
	/* shutdown 后传感器不再响应正常读，写 Power_Up_Reset 把它唤醒；
	 * 之后必须重走复位后收尾（等 50ms、丢帧、设 CPI、进 burst）。
	 */
	return paw3395_reset_and_config();
}
