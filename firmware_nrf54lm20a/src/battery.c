/*
 * 电池电量：走板载 nPM1300 PMIC 的 charger sensor API 读 VBAT，
 * 再按单节锂电的「电压 -> 电量」近似曲线折算成百分比。
 *
 * XIAO nRF54LM20A 的 PMIC（npm1300 @ 0x6B）挂在板级 DTS 的 gpio-i2c
 * （位带）总线上，charger 驱动（nordic,npm1300-charger）由板级默认
 * 使能，应用侧只需要 CONFIG_SENSOR=y。
 *
 * 注意：单次 sensor_sample_fetch 会触发 4 路 ADC 并阻塞约 1 ms
 * （250 us x 4），调用方控制在秒级周期即可，不要放进高频轮询。
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/sys/printk.h>

#include "battery.h"

#define CHARGER_NODE DT_NODELABEL(pmic_charger)

static const struct device *const charger = DEVICE_DT_GET(CHARGER_NODE);

/* 单节锂电「电压 -> 电量」近似点（轻载，够分三档显示用）。
 * 曲线两端夹紧：低于 3.30 V 一律 0%，高于 4.20 V 一律 100%。
 * 插着 USB 充电时 VBAT 会被抬到 ~4.2 V，显示 85~100% 档，符合直觉。
 */
static const struct {
	uint32_t uv;
	int pct;
} soc_table[] = {
	{ 3300000,   0 },
	{ 3500000,   5 },
	{ 3700000,  25 },
	{ 3900000,  60 },
	{ 4050000,  85 },
	{ 4200000, 100 },
};

int battery_init(void)
{
	int pct = -1;
	int err;

	if (!device_is_ready(charger)) {
		printk("battery: charger not ready\n");
		return -ENODEV;
	}

	err = battery_percent_get(&pct);
	if (err == 0) {
		printk("battery: vbat ok, soc=%d%%\n", pct);
	} else {
		printk("battery: first read failed (%d)\n", err);
	}

	return err;
}

int battery_percent_get(int *pct)
{
	struct sensor_value v;
	int32_t uv;
	int ret;

	if (pct == NULL) {
		return -EINVAL;
	}

	ret = sensor_sample_fetch(charger);
	if (ret < 0) {
		return ret;
	}

	ret = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &v);
	if (ret < 0) {
		return ret;
	}

	/* 驱动用 sensor_value_from_milli() 填充：val1 = 整数伏，val2 = 微伏小数 */
	uv = (int32_t)v.val1 * 1000000 + v.val2;

	if (uv <= soc_table[0].uv) {
		*pct = soc_table[0].pct;
		return 0;
	}

	for (size_t i = 1; i < ARRAY_SIZE(soc_table); i++) {
		if (uv <= soc_table[i].uv) {
			int64_t num = (int64_t)(uv - soc_table[i - 1].uv) *
				      (soc_table[i].pct - soc_table[i - 1].pct);

			*pct = soc_table[i - 1].pct +
			       (int)(num / (soc_table[i].uv - soc_table[i - 1].uv));
			return 0;
		}
	}

	*pct = soc_table[ARRAY_SIZE(soc_table) - 1].pct;
	return 0;
}

/* VBUS present：插着 USB（充电中/充满都算），用于"只在充电时亮灯"。
 * 这条属性不依赖 sensor_sample_fetch，直接读 PMIC 的状态寄存器。
 */
int battery_charging_get(bool *charging)
{
	struct sensor_value v;
	int ret;

	if (charging == NULL) {
		return -EINVAL;
	}

	ret = sensor_attr_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS,
			      SENSOR_ATTR_NPM13XX_CHARGER_VBUS_PRESENT, &v);
	if (ret < 0) {
		return ret;
	}

	*charging = (v.val1 != 0);
	return 0;
}
