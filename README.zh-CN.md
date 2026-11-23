# XIAO nRF54LM20A 无线游戏鼠标

中文 | [English](README.md)

基于 **Seeed XIAO nRF54LM20A** 模块与 **原相 PAW3395** 光学传感器从零自制的无线游戏鼠标，
配套 **ProMicro nRF52840** USB 接收器，可工作于 2.4G ESB 或 BLE 两种链路。

鼠标固件、接收器固件、上位机配置工具与烧录脚本全部开源，均已在实机上验证。

> [!WARNING]
> **PAW3395 的 `VDD` 必须接 1.9V，绝对不能接 3.3V**，只有 `VDDIO` 可以接 3.3V。
> 给 `VDD` 上 3.3V 会超出绝对最大额定值直接烧毁传感器。
> 接线或下单打板前请先看 [hardware/README.zh-CN.md](hardware/README.zh-CN.md)。

---

## 功能特性

- **1 kHz 无线回报率**：事件驱动主循环，每个连接事件只发最新位移，拥塞时不丢位移。
- **两种链路**（共用同一颗射频，二选一）：
  - **BLE HID**：HID over GATT，优先协商 LLPM（1ms 连接间隔），失败时回退到主机协商值，
    可直接连 Windows。
  - **2.4G ESB**：Nordic Enhanced ShockBurst，鼠标端 PTX、接收器端 PRX，延迟更低更稳定。
- **PAW3395 驱动**：motion burst 读取、SQUAL 表面质量输出、抬升检测、1 kHz 读取节拍。
- **软件 CPI 缩放**：该批次 PAW3395 的硬件 CPI 寄存器只读，因此在实测 5150 cpi 基准上
  做软件缩放，并带余数累加，避免慢速微调被截断。
- **6 挡 DPI**，配合防误触发、静默高度、运动同步等设置，可由上位机通过厂商 HID 通道下发。
- **8 格滚轮事件队列**：正反转切换时第一格不会被累加器吞掉。
- **按键可靠性**：20ms 心跳补发、300ms 锁存补发；I²C 按键扩展连续 300ms 无响应时强制释放。
- **电源管理**：长按 3 秒开关机，深度空闲关机可由电源键 / 滚轮 / 充电插拔中断唤醒；
  充电呼吸灯，电量经 BLE BAS 上报。
- **绑定信息写入 NVS**：断电重启无需重新配对。

---

## 硬件

| 器件 | 作用 |
| --- | --- |
| Seeed XIAO nRF54LM20A | 鼠标主控（BLE / ESB 射频） |
| 原相 PAW3395DM | 光学传感器（SPI，1.9V 核心 / 3.3V IO） |
| PCF8574T | I²C 扩展 IO，接侧键 |
| 5 脚机械编码器 | 滚轮 |
| nPM1300 PMIC（模块板载） | 3.3V 供电、充电、电量计 |
| ProMicro nRF52840 | USB 接收器（UF2 bootloader） |
| 自制载板 | 传感器、微动、编码器与天线，原理图与 PCB 见 `hardware/` |

模块与传感器插在自制载板上。原理图与 PCB layout 以两个 **嘉立创EDA专业版** 工程的形式
维护在 [hardware/easyeda/](hardware/easyeda/) 下：`Board1`（主板）与 `SideKeys`（侧键小板）。
元件选型、打样检查与外壳说明见 [hardware/README.zh-CN.md](hardware/README.zh-CN.md)。

鼠标外壳是 **第三方开源模型**，发布在 Thingiverse
（[thing:6187528](https://www.thingiverse.com/thing:6187528)）。载板就是按该外壳设计的，
原版 PCB 可直接装入；其许可证以原页面为准，不适用本仓库的 MIT 协议。

### 引脚分配（XIAO 排针 → PAW3395 / 外设）

| 信号 | XIAO 脚 | nRF54 引肢 | 说明 |
| --- | --- | --- | --- |
| SPI SCK | D8 | P1.04 | `spi23` |
| SPI MISO | D9 | P1.05 | `spi23` |
| SPI MOSI | D10 | P1.06 | `spi23` |
| NCS | D0 | P1.00 | 软件手动控制，burst 期间需保持低电平 |
| MOTION | D1 | P1.31 | 传感器中断 |
| NRESET | D6 | P1.08 | 传感器复位 |
| WHEEL_A | D2 | P1.30 | 编码器，上拉 |
| WHEEL_B | D3 | P1.29 | 编码器，上拉 |
| BTN_INT | D7 | P1.09 | PCF8574 中断，开漏上拉 |
| I²C SDA | D4 | P1.03 | `i2c22`，100 kHz |
| I²C SCL | D5 | P1.07 | `i2c22`，100 kHz |

`D6` / `D7` 被改作普通 GPIO，因此 [firmware_nrf54lm20a/app.overlay](firmware_nrf54lm20a/app.overlay)
里关闭了 `uart21`；控制台仍在 `uart20`（板载 SAMD11 USB-UART 桥）。

---

## 架构

```
                2.4G ESB  或  BLE HID
 ┌──────────────────┐   ─────────────────────►   ┌──────────────────┐
 │  鼠标            │                            │  接收器          │
 │  XIAO nRF54LM20A │                            │  ProMicro 52840  │
 │  PAW3395 + 按键  │                            │  ESB PRX / BLE   │
 └──────────────────┘                            │  主机            │
                                                 └────────┬─────────┘
                                                          │ USB HID (1 kHz)
                                                          ▼
                                                        电脑
```

两套固件共用同一份应用代码，链路在编译期由 `CONFIG_APP_ESB` 选择：

- `CONFIG_APP_ESB=n` → `ble_hid.c`（HID over GATT，也可直连 Windows）
- `CONFIG_APP_ESB=y` → 鼠标端 `esb_ptx.c`，接收器端 `esb_rx.c`

两种实现对外暴露同一套 `ble_hid.h` 接口，`main.c` 无需区分。详见
[firmware_nrf54lm20a/README.md](firmware_nrf54lm20a/README.md) 与
[dongle_nrf52840/README.md](dongle_nrf52840/README.md)。

---

## 目录结构

```
.
├── firmware_nrf54lm20a/     鼠标固件（Zephyr / NCS）
│   ├── src/                 main、PAW3395、输入、电池、BLE HID、ESB PTX、配置通道
│   ├── app.overlay          传感器 / 编码器 / 按键的设备树接线
│   ├── prj.conf             BLE HID 变体
│   └── prj_esb.conf         2.4G ESB 变体
├── dongle_nrf52840/         接收器固件（Zephyr / NCS）
│   ├── src/                 main、BLE 主机、ESB PRX、HID 配置通道
│   ├── prj.conf             BLE 主机变体
│   └── prj_esb.conf         ESB PRX 变体
├── mouse_driver/            PyQt5 上位机配置工具
├── tools/                   烧录、抓串口、RAW 采样与连接校验工具
├── hardware/                嘉立创EDA专业版原理图 / PCB 工程与外壳说明
├── boards/                  Seeed XIAO nRF54LM20A 板级定义（Apache-2.0，随仓库提供）
└── docs/                    设计说明，含归档的 nRF52840 原型指南
```

---

## 编译

### 环境准备

- **nRF Connect SDK v3.4.1** 及其工具链（通过 `nrfutil toolchain-manager` 安装）
- Python 3.10+
- 烧录 XIAO 需要 `pyocd` 与 `pyserial`

> [!IMPORTANT]
> Zephyr 的构建系统遇到含空格的路径会失败。请把仓库和 NCS 工作区放在无空格路径下，
> 例如用目录 junction 映射：
> `mklink /J C:\xiao-mouse "E:\trae commmunicationtest"`

先进入 NCS 环境，确保 `ZEPHYR_BASE` 与工具链在 `PATH` 上：

```powershell
nrfutil toolchain-manager launch --ncs-version v3.4.1 -- powershell
```

### 鼠标固件

BLE HID 变体：

```powershell
cd firmware_nrf54lm20a
west build -p always -b xiao_nrf54lm20a/nrf54lm20a/cpuapp -d build . -- -DBOARD_ROOT=<本仓库根目录>
```

2.4G ESB 变体：

```powershell
west build -p always -b xiao_nrf54lm20a/nrf54lm20a/cpuapp -d build-esb . -- -DBOARD_ROOT=<本仓库根目录> -DCONF_FILE=prj_esb.conf
```

> `-DBOARD_ROOT` 必须指向**仓库根目录**（其下才有 `boards/arm/...`）。构建走 sysbuild，
> 产物通常在 `build/xiao-fw/zephyr/zephyr.hex`。

### 接收器固件

BLE 主机变体：

```powershell
cd dongle_nrf52840
west build -p always -b promicro_nrf52840/nrf52840/uf2 -d build .
```

ESB PRX 变体：

```powershell
west build -p always -b promicro_nrf52840/nrf52840/uf2 -d build-esb . -- -DCONF_FILE=prj_esb.conf
```

### 烧录

**鼠标** —— 直接用板载 SAMD11 CMSIS-DAP，无需 J-Link：

```powershell
python tools/xiao_flash.py flash --hex firmware_nrf54lm20a/build/xiao-fw/zephyr/zephyr.hex
python tools/xiao_flash.py capture --seconds 8     # 可选：抓串口日志
```

接了多个探针时用 `--probe <CMSIS-DAP-ID>` 指定。XIAO nRF54LM20A 是 RRAM，可直接覆写，
**不要**随意执行 `erase`（ERASEALL 的坑见
[tools/xiao_flash.py](tools/xiao_flash.py) 文件头注释）。

**接收器** —— 双击复位进入 UF2 bootloader，然后拷贝镜像：

```powershell
Copy-Item dongle_nrf52840\build\zephyr\zephyr.uf2 <UF2 盘符>:\
```

> 鼠标与接收器必须烧**同一种**变体（BLE 对 BLE，ESB 对 ESB），混烧无法建立链路。

---

## 上位机配置工具

```powershell
cd mouse_driver
pip install -r requirements.txt
python main.py
```

界面可调 DPI 挡位、回报率、系统模式、运动同步、防误触发与静默高度。它通过 HID Feature
Report `0x06` 与接收器通信（协议见
[dongle_nrf52840/src/hid_cfg.h](dongle_nrf52840/src/hid_cfg.h)）；未接接收器时回退到
模拟设备，配置保存到 `%APPDATA%\MouseDriver\config.json`。

## 工具说明

| 工具 | 用途 |
| --- | --- |
| `tools/xiao_flash.py` | 通过 CMSIS-DAP 擦除 / 烧录 / 抓串口 |
| `tools/rawinput_counts.py` | RAW Input 采样，用于 CPI 标定 |
| `tools/verify-swd.ps1` | SWD / CMSIS-DAP 连接自检 |
| `tools/batt_log.ps1` | 电池放电记录 |
| `tools/pm_capture.ps1` | 功耗测量抓取辅助 |
| `tools/install-jlink-driver.ps1` | J-Link 驱动安装（旧方案保留） |

---

## 已知限制

- **BLE 与 2.4G ESB 不能同时工作**：同一颗射频，编译期二选一。
- 回报率受接收器全速 USB HID 端点限制，实际最高 **1 kHz**，射频本身可以更快。
- **CPI 由软件实现**：该批次 PAW3395 硬件 CPI 寄存器只读，实测基准为 5150 cpi。
- USB **VID/PID 为开发用值**（`0x1915` / `0x4242`），量产前请替换为自己的。
- 绑定信息存在 NVS，改动了存储布局的固件需要重新配对。

## 许可证

[MIT](LICENSE) © 2026 BlancaDei。

`boards/arm/xiao_nrf54lm20a/` 来自
[Seeed-Studio/seeed-boards-zephyr](https://github.com/Seeed-Studio/seeed-boards-zephyr)，
采用 Apache-2.0，详见 [boards/NOTICE.md](boards/NOTICE.md)。

## 致谢

- Nordic Semiconductor：nRF Connect SDK、HIDS 与 ESB 支持。
- Seeed Studio：XIAO nRF54LM20A 模块与板级定义。
- 原相科技：PAW3395 数据手册。
- 老项：鼠标外壳模型（Thingiverse，CC BY）。
