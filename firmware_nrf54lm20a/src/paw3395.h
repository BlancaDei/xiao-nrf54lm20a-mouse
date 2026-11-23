/*
 * PAW3395 光学鼠标传感器驱动（SPI）
 *
 * 传输协议依据 PixArt 4-wire SPI（详见 .c 内注释）：
 *   - SPI mode 3（CPOL=1, CPHA=1），MSB first，8 bit，SCLK <= 10 MHz
 *   - 写：先发 [reg | 0x80]，再发数据
 *   - 读：先发 [reg & 0x7F]，等待 tSRAD(>=2us)，再读 1 字节
 *   - Motion Burst：直接读 reg 0x16 即进入 burst，一个 CS 窗口连续取回
 *     12 字节（含 ΔX/ΔY），不需要先写 0x16/0x50
 *
 * 注意：寄存器地址取自 PixArt 高端家族（PMW3360/PMW3389）公开的映射。
 * PAW3395 厂商版"上电初始化寄存器表"在 NDA 附录里，未包含在公开 datasheet，
 * 因此本驱动的上电序列只做公开部分（Power_Up_Reset 等），
 * 标 TODO 的项需拿到寄存器附录后补全。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PAW3395_H
#define PAW3395_H

#include <stdint.h>
#include <stdbool.h>

#include <zephyr/drivers/gpio.h>

/* ---- 寄存器地址（PixArt 高端家族通用） ---- */
#define PAW3395_REG_PRODUCT_ID      0x00
#define PAW3395_REG_REVISION_ID     0x01
#define PAW3395_REG_MOTION          0x02
#define PAW3395_REG_DELTA_X_L       0x03
#define PAW3395_REG_DELTA_X_H       0x04
#define PAW3395_REG_DELTA_Y_L       0x05
#define PAW3395_REG_DELTA_Y_H       0x06
#define PAW3395_REG_SQUAL           0x07
/* 0x0D 是可写可读的旧式单字节分辨率候选，但改它位移量不变，不是本芯片的
 * 有效分辨率寄存器。真正生效的是 datasheet §8.2 的 0x48~0x4B（16 bit，
 * 50 cpi/LSB）+ 0x47 写 1 确认，见 paw3395_set_cpi()。
 */
#define PAW3395_REG_RESOLUTION      0x0D
#define PAW3395_REG_CPI_L_LEGACY    0x0E /* 待验证：16bit 分辨率低字节 */
#define PAW3395_REG_CPI_H_LEGACY    0x0F /* 待验证：16bit 分辨率高字节 */
#define PAW3395_REG_CONFIG2         0x10
#define PAW3395_REG_ANGLE_TUNE      0x11
#define PAW3395_REG_POWER_UP_RESET  0x3A
#define PAW3395_REG_SHUTDOWN        0x3B
/* 以下四个来自 datasheet v0.8 的寄存器总表（8.1）：
 *   0x40 Performance：bit7 AWAKE = 1 时禁用 Rest Mode（帧率保持 run 档）
 *   0x47 Set_Resolution：写完 0x48/0x49/0x4A/0x4B 后写 0x01 才生效
 *   0x48/0x49 Resolution_X（16bit，单位 50 cpi/LSB，默认 0x0063 = 4950 cpi）
 *   0x4A/0x4B Resolution_Y
 */
#define PAW3395_REG_PERFORMANCE     0x40
#define PAW3395_REG_SET_RESOLUTION  0x47
#define PAW3395_REG_RES_X_L        0x48
#define PAW3395_REG_RES_X_H        0x49
#define PAW3395_REG_RES_Y_L        0x4A
#define PAW3395_REG_RES_Y_H        0x4B
#define PAW3395_PERF_AWAKE         BIT(7)
/* 待验证：0x17 在 PMW3360/PMW3389 公开映射里是 Frame Rate（帧周期，值越小
 * 帧率越高）。PAW3395 的真实寄存器表在 NDA 附录里，这里把它作为**实验开关**：
 * 写入后回读校验，并在 SQUAL 连续为 0（传感器不再成像）时自动恢复原值。
 */
#define PAW3395_REG_FRAME_RATE      0x17
/* 排查开关：1 = 开机写 0x17 做帧率实验；0 = 完全不碰这个寄存器。
 * 结论已经明确：0x17 不是可写的配置寄存器（写 0x00/0x5C 都读不回来，
 * 且每次开机读到的值都不一样），所以出货配置必须是 0。
 */
#define PAW3395_FRAME_RATE_EXPERIMENT 0
#define PAW3395_FRAME_RATE_VALUE    0x00

/* 0x16 = Motion_Burst（不是 PMW3360 的 0x50） */
#define PAW3395_REG_MOTION_BURST    0x16
/* bank 选择寄存器：写 MSB 后访问低字节地址，用完写回 0 */
#define PAW3395_REG_BANK            0x7F
#define PAW3395_REG_LIFT_CONFIG     0x4E /* bank 0x0C */

#define PAW3395_CPI_MIN      50
#define PAW3395_CPI_MAX      26000
#define PAW3395_CPI_DEFAULT  1600

/* 实测：不动任何寄存器时，这颗传感器实际分辨率 ≈ 5150 cpi
 * （净推 20.0 cm 得 40,711 counts；10.0 cm 那次折算 5145 cpi，两者一致）。
 * 芯片上没有找到可用的分辨率寄存器，所以 DPI 只能在软件层按这个基准折算。
 */
#define PAW3395_CPI_MEASURED 5150

/* 上电复位写入值 */
#define PAW3395_POWER_UP_RESET_MAGIC  0x5A
/* 进入 shutdown 的写入值。0x3B 是"魔术字"寄存器：只有写 0xB6 才会真正
 * 关掉内部所有模拟块（含 LED 驱动），写 0x00 等其他值一律无效。 */
#define PAW3395_SHUTDOWN_MAGIC        0xB6

/* Motion burst 返回长度（与 PMW3360 一致：motion + observation + ΔX + ΔY + ...） */
#define PAW3395_BURST_LEN  12
#define PAW3395_BURST_OFF_DX  2
#define PAW3395_BURST_OFF_DY  4

/**
 * @brief 初始化传感器（GPIO + SPI 上电序列）。
 * @return 0 成功，负值为 errno
 */
int paw3395_init(void);

/**
 * @brief 读 Product_ID / Revision_ID，用于判断 SPI 是否真的通。
 * @return 0 成功（寄存器已被读取，即使数值可疑）；负值表示总线错误
 */
int paw3395_probe(uint8_t *product_id, uint8_t *revision_id);

/**
 * @brief SPI 回环自检：把 D9(MISO) 与 D10(MOSI) 短接，全双工发 0xA5 应收 0xA5。
 *
 * 用途：把「MCU 侧 SPI 外设/引脚是否正常」与「传感器侧接线/供电是否正常」
 * 彻底切开。@p rx 传出收到的字节，调用方判断是否等于 0xA5 即为 PASS。
 * 没接跳线时读到的是浮空值（通常 0x00 或 0xFF），不代表失败。
 * @return 0 成功，负值为总线错误
 */
int paw3395_spi_loopback(uint8_t *rx);

/**
 * @brief MOTION 引脚当前是否有效（传感器有未读位移）。
 *
 * DT 里 paw-motion-gpios 是 GPIO_ACTIVE_LOW：有未读位移时传感器拉低该脚，
 * 经 DT 转换后逻辑值为 1（true）。用途：空闲（无位移）时跳过 SPI 读，
 * 省掉无意义的 SPI 流量；因为传感器内部自己累加 counts，跳读不丢位移。
 */
bool paw3395_motion_asserted(void);

/**
 * @brief 开关 MOTION 引脚中断（用于休眠时用"移动"唤醒 MCU，免掉轮询）。
 *
 * 传感器有未读位移时会把 MOTION 拉低（active-low），所以这里用**下降沿**。
 * 使能一次即可常驻；唤醒路径里关掉，正常轮询期间不会再进 ISR。
 * @param cb      调用者提供的回调结构体（生命周期由调用者保证）
 * @param handler 中断处理函数
 * @param enable  true = 使能，false = 关闭
 * @return 0 成功，负值为错误
 */
int paw3395_motion_irq_configure(struct gpio_callback *cb,
				 gpio_callback_handler_t handler, bool enable);

/**
 * @brief 开关 Rest 模式（0x40 Performance 的 bit7 AWAKE）。
 *
 * AWAKE = 1：禁用 Rest，传感器保持 run 档（典型 1.7 mA，零唤醒延迟）。
 * AWAKE = 0：允许按"静止 1 s / 10 s / 600 s"逐级降到 Rest1/Rest2/Rest3，
 *            对应唤醒延迟约 2 ms / 200 ms / 1 s（datasheet §7.6 Table 11）。
 *
 * 推荐用法：只在确定能容忍唤醒延迟的空闲期关 AWAKE；一旦 MOTION 有效，
 * 必须**先写回 AWAKE = 1 再读 burst**，否则读到的是 Rest 档的旧帧。
 * @return 0 成功，负值为总线错误
 */
int paw3395_set_awake(bool awake);

/**
 * @brief 让传感器进入 shutdown（写 0x3B = 0xB6），省掉它的大头电流。
 *
 * 0x3B 是魔术字寄存器，只有 0xB6 才真正关掉内部模拟块；进入后传感器
 * 不再响应正常读，唤醒必须重新走 Power_Up_Reset（见 paw3395_wake）。
 * @return 0 成功，负值为总线错误
 */
int paw3395_sleep(void);

/**
 * @brief 从 shutdown 唤醒：重走 Power_Up_Reset + 复位后收尾（设 CPI、进 burst）。
 *
 * 复用 paw3395_init 里同一段复位后处理，唤醒后配置与开机一致。
 * @return 0 成功，负值为错误
 */
int paw3395_wake(void);

/** @brief 设置分辨率（CPI，50~26000，按 50 cpi/LSB 量化）。
 *
 * 这是**硬件**写入：0x48/0x49/0x4A/0x4B 写 16 bit，最后写 0x47=0x01 生效。
 * 软件缩放（paw3395_scaler_*）仍保留，用于标定误差修正。
 */
int paw3395_set_cpi(uint16_t cpi);

/**
 * @brief 软件缩放：把原始 counts（≈ PAW3395_CPI_MEASURED cpi）折算成目标 cpi。
 *
 * 用它是为了在硬件分辨率寄存器定位到之前也能设 DPI。内部带余数累加，
 * 否则低速时整数截断会把小数部分反复丢掉、位移变少、手感发飘。
 * 每个轴要各自一个实例（余数是按轴保存的）。
 */
struct paw3395_scaler {
	uint16_t num; /* 目标 cpi */
	int32_t rem;  /* 上次折算剩下的余数 */
};

/** @brief 初始化缩放器（target_cpi 会被夹到 50~26000）。 */
void paw3395_scaler_init(struct paw3395_scaler *s, uint16_t target_cpi);

/** @brief 折算一个轴的原始 counts。返回折算后的 counts（可能为 0）。 */
int16_t paw3395_scaler_apply(struct paw3395_scaler *s, int16_t raw);

/**
 * @brief 从 motion burst 读一次位移（datasheet §5.7.2）。
 *
 * 读 0x16 本身就进入 burst，一次 CS 窗口读回 12 字节，实测约 180 us；
 * 逐寄存器读（paw3395_read_motion）要 ~700 us，所以正常路径走这个。
 * raw 可选，传出 12 个原始字节：[00]=Motion [01]=Observation
 * [02..03]=ΔX [04..05]=ΔY [06..11]=其余。
 *
 * 注意：MOT 位为 0（无待读位移）时 delta 区会读成 0xFFFFFFFF，上层必须
 * 按 MOT 位判定，否则会得到假的 -1,-1 位移。
 */
int paw3395_read_burst_raw(int16_t *dx, int16_t *dy, uint8_t raw[PAW3395_BURST_LEN]);

/**
 * @brief 单寄存器方式读位移（非 burst，调试用）。
 * @param overflow 传出：本次是否发生溢出（无法判定时置 false）
 */
int paw3395_read_motion(int16_t *dx, int16_t *dy, bool *overflow);

/** @brief 调试用：直接读写寄存器。 */
int paw3395_write_reg(uint8_t reg, uint8_t val);
uint8_t paw3395_read_reg(uint8_t reg);

/**
 * @brief 打印 0x00~0x3F 全部寄存器内容。
 *
 * 用途：PAW3395 的寄存器表在 NDA 附录里，公开 datasheet 没有。上电默认
 * 分辨率通常是 400 cpi，所以 dump 出来若看到某个寄存器是 0x0190(=400) 或
 * 0x04(=400/100)，基本就能定位 CPI 寄存器。
 */
void paw3395_reg_dump(void);

/**
 * @brief 设置抬升检测（静默高度）档位。
 *
 * LIFT_CONFIG 的 bit[1:0] 只有两档：0 = 1 mm，2 = 2 mm。
 * 这里映射为：低档 = 1 mm，中/高档 = 2 mm。
 *
 * @param level 0 低 / 1 中 / 2 高
 * @return 0 成功，其他负值为总线错误
 */
int paw3395_lift_cutoff_set(uint8_t level);

/**
 * @brief 对 CPI 候选寄存器做「写-回读」探针，找出哪些是可写的。
 *
 * 只碰 0x0D/0x0E/0x0F/0x10/0x11 这几个（分别是 PMW3360 单字节分辨率、
 * PMW3389 16bit 分辨率、Config2、Angle_Tune 的候选位置），写完会恢复原值。
 */
void paw3395_cpi_probe(void);

#endif /* PAW3395_H */
