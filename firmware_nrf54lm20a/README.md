# 鼠标固件（XIAO nRF54LM20A / Zephyr）

鼠标端固件，支持两种编译变体：BLE HID 与 2.4G ESB。两者共用 `main.c`，
链路层通过 `CONFIG_APP_ESB` 在编译期切换。

## 源文件

| 文件 | 说明 |
| --- | --- |
| `src/main.c` | 主循环、1 kHz 上报调度、LED、开关机、抬升/丢帧保护 |
| `src/paw3395.c/.h` | PAW3395 驱动：burst 读取、CPI、SQUAL、复位 |
| `src/input.c/.h` | 微动、滚轮编码器、PCF8574 按键扩展 |
| `src/battery.c/.h` | 经 nPM1300 读取电量并上报 BAS |
| `src/ble_hid.c/.h` | BLE HID over GATT 实现（变体 A） |
| `src/esb_ptx.c` | 2.4G ESB 发送端实现，对外仍是 `ble_hid.h` 接口（变体 B） |
| `src/cfg_link.c/.h` | 厂商配置通道（DPI / 回报率等） |
| `app.overlay` | 传感器、编码器、按键的引脚与总线配置 |

## 编译

先进入 NCS 环境：

```powershell
nrfutil toolchain-manager launch --ncs-version v3.4.1 -- powershell
```

BLE 变体：

```powershell
west build -p always -b xiao_nrf54lm20a/nrf54lm20a/cpuapp -d build . -- -DBOARD_ROOT=..
```

ESB 变体：

```powershell
west build -p always -b xiao_nrf54lm20a/nrf54lm20a/cpuapp -d build-esb . -- -DBOARD_ROOT=.. -DCONF_FILE=prj_esb.conf
```

`-DBOARD_ROOT` 指向本仓库根目录（其中含 `boards/arm/xiao_nrf54lm20a`）。
路径不要带空格，否则 Zephyr 构建会失败。

## 烧录

```powershell
python ../tools/xiao_flash.py flash --hex build/xiao-fw/zephyr/zephyr.hex
```

## 设计要点

- **NCS 不走 SPI 控制器的片选**：PAW3395 的 NCS 由 GPIO 手动控制，因为 motion burst
  要求 CS 在整个读序列内保持低电平。
- **CPI 用软件缩放**：该批次 PAW3395 的硬件 CPI 寄存器只读，基准为实测 5150 cpi，
  带余数累加避免慢速位移丢失。
- **合并发送**：每个连接事件只提交最新位移，避免 1 kHz 下发送池打满导致卡顿或丢帧。
- **丢帧保护**：只有单帧位移绝对值之和超过 128 counts 才判定为抬手/跳变并丢弃，
  慢速微调不会被误伤。
- **关机**：关闭射频与传感器后进入深度空闲，仅靠电源键 / 滚轮 / 充电插拔中断唤醒，
  不使用 System OFF（在本板上会导致复位）。
