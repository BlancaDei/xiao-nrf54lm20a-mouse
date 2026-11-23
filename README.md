# XIAO nRF54LM20A Wireless Gaming Mouse

[中文说明](README.zh-CN.md) | English

A from-scratch, open-source wireless gaming mouse built around the **Seeed XIAO nRF54LM20A**
module and the **PixArt PAW3395** optical sensor, with a **ProMicro nRF52840** USB dongle
acting as either a 2.4 GHz ESB receiver or a BLE central.

Everything here — mouse firmware, dongle firmware, PC configuration tool and flashing
utilities — was developed and validated on real hardware.

> [!WARNING]
> **PAW3395 `VDD` must be supplied with 1.9 V, never 3.3 V.** Only `VDDIO` may be tied to
> 3.3 V. Feeding 3.3 V into `VDD` exceeds the absolute maximum rating and destroys the sensor.
> See [docs/hardware-design-guide.zh-CN.md](docs/hardware-design-guide.zh-CN.md) before wiring.

---

## Features

- **1 kHz wireless polling** — event-driven main loop, position deltas batched per
  connection event so no motion is lost when the radio is congested.
- **Two link modes** (mutually exclusive, the radio is shared):
  - **BLE HID** — HID over GATT, LLPM (1 ms connection interval) attempted first,
    falls back to the interval negotiated by the host. Works with Windows directly.
  - **2.4 GHz ESB** — Nordic Enhanced ShockBurst, PTX on the mouse, PRX on the dongle,
    lower and more deterministic latency.
- **PAW3395 driver** with motion-burst reads, SQUAL surface-quality reporting,
  lift detection and a 1 kHz read cadence.
- **Software CPI scaling** — the PAW3395 hardware CPI registers are read-only on this
  part, so CPI is applied in software on top of the measured 5150 cpi base, with
  remainder accumulation so slow movements are not truncated away.
- **6 DPI slots**, debounce / lift-off / motion-sync settings, configurable from the
  PC tool over a vendor HID feature report.
- **8-entry wheel queue** so reversing direction never swallows the first detent.
- **Robust buttons** — 20 ms heartbeat re-send, 300 ms latched press, stuck-key release
  if the I²C key expander stops responding for 300 ms.
- **Power management** — 3 s long-press power on/off, deep-idle shutdown woken by the
  power key / wheel / charger interrupts, charging breath LED, battery level reported
  over BLE BAS.
- **Bonding persisted in NVS** — no re-pairing after a power cycle.

---

## Hardware

| Part | Role |
| --- | --- |
| Seeed XIAO nRF54LM20A | Mouse MCU (BLE / ESB radio, Cortex-M33) |
| PixArt PAW3395DM | Optical sensor (SPI, 1.9 V core / 3.3 V I/O) |
| PCF8574T | I²C GPIO expander for the side buttons |
| 5-pin mechanical encoder | Scroll wheel |
| nPM1300 PMIC (on-module) | 3.3 V rail, charger, fuel gauge |
| ProMicro nRF52840 | USB dongle (UF2 bootloader) |
| Custom mainboard | Switch / sensor / LDO / antenna carrier |

The sensor and module are plugged onto a custom mouse mainboard. The board-level design
notes (in Chinese) live in
[docs/hardware-design-guide.zh-CN.md](docs/hardware-design-guide.zh-CN.md).

### Pinout (XIAO header → PAW3395 / peripherals)

| Signal | XIAO pin | nRF54 pin | Notes |
| --- | --- | --- | --- |
| SPI SCK | D8 | P1.04 | `spi23` |
| SPI MISO | D9 | P1.05 | `spi23` |
| SPI MOSI | D10 | P1.06 | `spi23` |
| NCS | D0 | P1.00 | Driven manually, must stay low across a burst |
| MOTION | D1 | P1.31 | Sensor motion interrupt |
| NRESET | D6 | P1.08 | Sensor reset |
| WHEEL_A | D2 | P1.30 | Encoder, pull-up |
| WHEEL_B | D3 | P1.29 | Encoder, pull-up |
| BTN_INT | D7 | P1.09 | PCF8574 INT, open-drain, pull-up |
| I²C SDA | D4 | P1.03 | `i2c22`, 100 kHz |
| I²C SCL | D5 | P1.07 | `i2c22`, 100 kHz |

`D6` / `D7` are repurposed as GPIO, so `uart21` is disabled in
[firmware_nrf54lm20a/app.overlay](firmware_nrf54lm20a/app.overlay). The console stays on
`uart20` (the on-board SAMD11 USB-UART bridge).

---

## Architecture

```
                2.4 GHz ESB  or  BLE HID
 ┌──────────────────┐   ─────────────────────►   ┌──────────────────┐
 │  Mouse           │                            │  Dongle          │
 │  XIAO nRF54LM20A │                            │  ProMicro 52840  │
 │  PAW3395 + keys  │                            │  ESB PRX / BLE   │
 └──────────────────┘                            │  central         │
                                                 └────────┬─────────┘
                                                          │ USB HID (1 kHz)
                                                          ▼
                                                        Host PC
```

Both firmware images share the same application code. The link layer is chosen at build
time by `CONFIG_APP_ESB`:

- `CONFIG_APP_ESB=n` → `ble_hid.c` (HID over GATT, also connects to Windows directly)
- `CONFIG_APP_ESB=y` → `esb_ptx.c` on the mouse, `esb_rx.c` on the dongle

The two implementations expose the same `ble_hid.h` API, so `main.c` is unaware of the
difference. See [firmware_nrf54lm20a/README.md](firmware_nrf54lm20a/README.md) and
[dongle_nrf52840/README.md](dongle_nrf52840/README.md).

---

## Repository layout

```
.
├── firmware_nrf54lm20a/     Mouse firmware (Zephyr / NCS)
│   ├── src/                 main, PAW3395, input, battery, BLE HID, ESB PTX, config link
│   ├── app.overlay          Devicetree wiring for sensor, encoder, keys
│   ├── prj.conf             BLE HID variant
│   └── prj_esb.conf         2.4 GHz ESB variant
├── dongle_nrf52840/         USB dongle firmware (Zephyr / NCS)
│   ├── src/                 main, BLE central, ESB PRX, HID config channel
│   ├── prj.conf             BLE central variant
│   └── prj_esb.conf         ESB PRX variant
├── mouse_driver/            PyQt5 PC configuration tool
├── tools/                   Flashing, capture, RAW-input and verification utilities
├── boards/                  Vendored Seeed XIAO nRF54LM20A board definitions (Apache-2.0)
└── docs/                    Hardware design guide
```

---

## Building

### Prerequisites

- **nRF Connect SDK v3.4.1** and its toolchain (install via `nrfutil toolchain-manager`)
- Python 3.10+
- `pyocd` + `pyserial` for flashing the XIAO board

> [!IMPORTANT]
> Zephyr's build system breaks on paths that contain spaces. Put the repository (and
> your NCS workspace) under a space-free path, e.g. map it with a directory junction:
> `mklink /J C:\xiao-mouse "E:\trae commmunicationtest"`.

Open an NCS shell first so `ZEPHYR_BASE` and the toolchain are on `PATH`:

```powershell
nrfutil toolchain-manager launch --ncs-version v3.4.1 -- powershell
```

### Mouse firmware

BLE HID variant:

```powershell
cd firmware_nrf54lm20a
west build -p always -b xiao_nrf54lm20a/nrf54lm20a/cpuapp -d build . -- -DBOARD_ROOT=<path-to-this-repo>
```

2.4 GHz ESB variant:

```powershell
west build -p always -b xiao_nrf54lm20a/nrf54lm20a/cpuapp -d build-esb . -- -DBOARD_ROOT=<path-to-this-repo> -DCONF_FILE=prj_esb.conf
```

> `-DBOARD_ROOT` must point at the **repository root** (it contains `boards/arm/...`).
> The build is a sysbuild image, so the artifact is normally
> `build/xiao-fw/zephyr/zephyr.hex`.

### Dongle firmware

BLE central variant:

```powershell
cd dongle_nrf52840
west build -p always -b promicro_nrf52840/nrf52840/uf2 -d build .
```

ESB PRX variant:

```powershell
west build -p always -b promicro_nrf52840/nrf52840/uf2 -d build-esb . -- -DCONF_FILE=prj_esb.conf
```

### Flashing

**Mouse** — the on-board SAMD11 CMSIS-DAP probe is used directly; no J-Link required:

```powershell
python tools/xiao_flash.py flash --hex firmware_nrf54lm20a/build/xiao-fw/zephyr/zephyr.hex
python tools/xiao_flash.py capture --seconds 8     # optional serial capture
```

`--probe <CMSIS-DAP-id>` selects a specific probe when more than one is attached.
The XIAO nRF54LM20A uses RRAM, which can be overwritten directly — **do not** run
`erase` unless you really need to (see the header comment in
[tools/xiao_flash.py](tools/xiao_flash.py) for the ERASEALL pitfalls).

**Dongle** — double-tap reset to enter the UF2 bootloader, then copy the image:

```powershell
Copy-Item dongle_nrf52840\build\zephyr\zephyr.uf2 <UF2-DRIVE>:\
```

> The mouse and the dongle must be flashed with **matching** variants (BLE ↔ BLE,
> ESB ↔ ESB). Mixing them will simply not link.

---

## PC configuration tool

```powershell
cd mouse_driver
pip install -r requirements.txt
python main.py
```

The GUI exposes DPI slots, polling rate, system mode, motion sync, debounce and lift-off.
It talks to the dongle through HID feature report `0x06` (protocol documented in
[dongle_nrf52840/src/hid_cfg.h](dongle_nrf52840/src/hid_cfg.h)); without a dongle attached
it falls back to a mock transport that saves the configuration to
`%APPDATA%\MouseDriver\config.json`.

## Tools

| Tool | Purpose |
| --- | --- |
| `tools/xiao_flash.py` | Erase / flash / serial capture for the XIAO via CMSIS-DAP |
| `tools/rawinput_counts.py` | RAW-input capture used for CPI calibration |
| `tools/verify-swd.ps1` | Sanity check of the SWD/CMSIS-DAP connection |
| `tools/batt_log.ps1` | Battery discharge logging |
| `tools/pm_capture.ps1` | Power-measurement capture helper |
| `tools/install-jlink-driver.ps1` | J-Link driver install helper (legacy path) |

---

## Known limitations

- **BLE and 2.4 GHz ESB cannot run at the same time** — one radio, selected at build time.
- Polling rate is capped at **1 kHz** by the dongle's full-speed USB HID endpoint, even
  though the radio can go faster.
- **CPI is applied in software.** PAW3395 hardware CPI registers are read-only on this
  variant; the measured base is 5150 cpi.
- The USB **VID/PID are development values** (`0x1915` / `0x4242`). Replace them with
  your own before distributing hardware.
- Bonding is stored in NVS; flashing a build that changes the storage layout requires a
  re-pair.

## License

[MIT](LICENSE) © 2026 BlancaDei.

`boards/arm/xiao_nrf54lm20a/` is vendored from
[Seeed-Studio/seeed-boards-zephyr](https://github.com/Seeed-Studio/seeed-boards-zephyr)
under Apache-2.0 — see [boards/NOTICE.md](boards/NOTICE.md).

## Acknowledgements

- Nordic Semiconductor for the nRF Connect SDK, HIDS and ESB support.
- Seeed Studio for the XIAO nRF54LM20A module and board definitions.
- PixArt for the PAW3395 datasheet.
