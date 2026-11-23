# Hardware

[中文说明](README.zh-CN.md) | English

The current hardware revision is a **carrier board for the Seeed XIAO nRF54LM20A**
module. The module provides the MCU, radio, 3.3 V rail, charger and fuel gauge; the
carrier board carries the PAW3395 sensor, the switches, the encoder, the I²C key
expander and the antenna.

## Contents

| Path | Description |
| --- | --- |
| `easyeda/` | 嘉立创EDA专业版 (EasyEDA Pro) project — schematic and PCB layout |
| `../docs/` | Design notes and the legacy nRF52840 prototype guide |

The EasyEDA Pro project is the **single source of truth** for the schematic and the
layout. Gerbers, BOM and pick-and-place files should be generated from it rather than
edited by hand.

## Opening the project

1. Install **嘉立创EDA专业版 / EasyEDA Pro**.
2. `文件 → 打开` and pick the `.epro` project file in `easyeda/`
   (or `文件 → 导入` if you received a `.zip` export).
3. Open the schematic and the PCB, then run `设计 → 检查DRC` before ordering.

## Fabricating

The project is designed for JLCPCB-compatible fabrication. Before placing an order:

- Re-run DRC and review the stackup in the project.
- Export Gerber, BOM and pick-and-place from the **same** project revision.
- Verify the PAW3395 supply: **`VDD` = 1.9 V, `VDDIO` = 3.3 V**. Never feed 3.3 V
  into `VDD` — it exceeds the absolute maximum rating and destroys the sensor.
- Keep the SPI traces short and the antenna keep-out area free of copper.
- Match the pinout against
  [`firmware_nrf54lm20a/app.overlay`](../firmware_nrf54lm20a/app.overlay); the
  firmware is written for that exact wiring.

## Enclosure

The mouse shell is a third-party open model, not part of this repository:

- <https://www.thingiverse.com/thing:6187528>

The model is distributed by its original author on Thingiverse. **Its license is
defined by that page**, not by this repository's MIT license — check it before
redistributing or selling printed shells, and credit the original author.

If you change the PCB, verify the shell's internal clearances (sensor height, switch
posts, battery bay, USB opening) before printing.
