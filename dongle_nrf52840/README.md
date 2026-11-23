# 接收器固件（ProMicro nRF52840 / Zephyr）

USB 接收器固件，向上枚举为 HID 鼠标 + CDC 串口复合设备，向下支持两种链路变体：

- **BLE 主机（默认）**：主动连接鼠标并订阅 HOG 通知，再转成本机 USB HID。
- **2.4G ESB PRX**：接收鼠标 PTX 的 2.4G 数据包，转成本机 USB HID。

编译期由 `CONFIG_APP_ESB` 切换，两套实现共用 `main.c` 的 USB HID 提交逻辑。

## 源文件

| 文件 | 说明 |
| --- | --- |
| `src/main.c` | USB HID 提交、报告队列、LED、启动流程 |
| `src/ble_ctr.c/.h` | BLE 主机：扫描、连接、订阅 HID 通知（变体 A） |
| `src/esb_rx.c/.h` | ESB PRX 收包与按键/位移解析（变体 B） |
| `src/hid_cfg.c/.h` | 厂商 HID Feature Report `0x06` 配置通道 |
| `app.overlay` | HID 设备实例（1 ms 轮询）与 LED 极性修正 |

## 编译

```powershell
nrfutil toolchain-manager launch --ncs-version v3.4.1 -- powershell
```

BLE 主机变体：

```powershell
west build -p always -b promicro_nrf52840/nrf52840/uf2 -d build .
```

ESB PRX 变体：

```powershell
west build -p always -b promicro_nrf52840/nrf52840/uf2 -d build-esb . -- -DCONF_FILE=prj_esb.conf
```

## 烧录

双击接收器复位键进入 UF2 bootloader（出现 U 盘），然后：

```powershell
Copy-Item build\zephyr\zephyr.uf2 <UF2 盘符>:\
```

## 设计要点

- **必须与鼠标端烧同一种变体**，否则无法建立链路。
- **键盘/按键状态随每个位移包一起提交**：ESB 下每个 6 字节包都会被当成完整 HID 报告，
  若位移包不带按键位会立刻“松开”正在按住的键。
- **USB 全速限制 1 kHz**：HID 中断端点最小 `bInterval = 1 ms`，更高的回报率只能停留在协议层。
- **串口日志会抢占带宽**：CDC 与 HID 共用一颗 USB，阻塞式日志会拖慢 HID 提交，
  正式固件已关闭周期性状态打印。
- **配对持久化**：LTK 存在 NVS，避免每次上电重新配对导致鼠标侧拒绝。
