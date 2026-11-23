/*
 * 输入层：滚轮编码器（正交）+ PCF8574T 按键扩展
 *
 * 本模块只负责“取硬件状态”，不做按键到位映射（哪一位对应哪个键未知，
 * 映射放在 main.c）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief 初始化编码器 GPIO 中断、按键 INT 引脚与 PCF8574 所在的 i2c22。
 *        任一部分就绪即返回 0；具体失败会打日志说明。
 * @return 0 有任一部分可用，负值为 errno
 */
int input_init(void);

/**
 * @brief 取走并清零自上次调用以来累积的编码器正交步数。
 *        注意：一个机械刻度通常是 4 个正交状态，除以 4 才是 1 格滚轮，
 *        换算由调用方（main.c）完成。
 * @return 累积步数（可为负）
 */
int input_wheel_take(void);

/**
 * @brief 读一次 PCF8574 端口原始值，并更新内部去抖状态。
 *        流程：先写 1 字节 0xFF（准双向口置为输入），再读 1 字节端口值。
 * @param raw 非空时回填原始端口值
 * @return 0 成功，负值为 errno
 */
int input_buttons_read(uint8_t *raw);

/**
 * @brief 取去抖后“已确认”的端口值（连续多次相同的读值才被接受）。
 * @param debounced 非空时回填
 * @return 0 成功
 */
int input_buttons_get(uint8_t *debounced);

/** @brief 累计 I2C 读失败次数（用于诊断/超时释放）。 */
uint32_t input_button_read_errors(void);

/**
 * @brief 强制把按键确认状态复位为"全松开"。
 *
 * 用于 I2C 连续读失败、状态可能卡在按下时做安全释放；
 * 下一次成功读会重新按去抖次数确认。
 */
void input_buttons_force_release(void);

/**
 * @brief 按键扩展 INT 线是否有效（D7，PCF8574 有变化时拉低）。
 * @return true 表示已拉低
 */
bool input_btn_int_asserted(void);

/**
 * @brief 注册"有输入动作"回调（滚轮边沿 / 按键 INT 都会调用）。
 *
 * 用于把滚轮与按键从定时轮询改成中断唤醒。回调在 **ISR 上下文**执行，
 * 必须极短 —— 典型用法是给唤醒信号量 k_sem_give()。
 */
void input_set_wake_notify(void (*fn)(void));

/**
 * @brief 设置按键去抖需要的"连续相同读值"次数（防误触发档位）。
 *
 * 次数越多，抬鼠复位时的机械抖动越不容易被当成一次点击，代价是按键
 * 响应多几毫秒。调用方要同步把补读窗口拉长（main.c 的 settle 时间）。
 *
 * @param n 2..10，超出范围会被夹住
 */
void input_set_debounce_count(uint8_t n);

/** @brief 当前去抖次数（主循环据此决定补读窗口）。 */
uint8_t input_debounce_count(void);

#endif /* INPUT_H */
