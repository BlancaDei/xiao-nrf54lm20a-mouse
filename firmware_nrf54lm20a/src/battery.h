/*
 * 电池电量读取：V(BAT) 来自板载 nPM1300 PMIC 的 ADC（charger sensor API），
 * 按单节锂电近似曲线折算成 0~100%。
 *
 * battery_percent_get() 读取失败返回负值；成功时 *pct = 0..100。
 */
#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>

int battery_init(void);
int battery_percent_get(int *pct);

/* 是否插着 USB 供电（nPM1300 的 VBUS present 位）。
 * 用于"只在充电时亮灯"，成功返回 0 并写 *charging。 */
int battery_charging_get(bool *charging);

#endif /* BATTERY_H */
