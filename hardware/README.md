# Hardware

[中文说明](README.zh-CN.md) | English

The current hardware revision is a **carrier board for the Seeed XIAO nRF54LM20A**
module. The module provides the MCU, radio, 3.3 V rail, charger and fuel gauge; the
carrier board carries the PAW3395 sensor, the switches, the encoder, the I²C key
expander and the antenna.

## Contents

| Path | Description |
| --- | --- |
| `easyeda/Board1.epro2` | 嘉立创EDA专业版 (EasyEDA Pro) main board — schematic and PCB layout |
| `easyeda/SideKeys.epro2` | Side-key daughter board — schematic and PCB layout |
| `../docs/` | Design notes and the legacy nRF52840 prototype guide |

The EasyEDA Pro projects are the **single source of truth** for the schematics and the
layout. Gerbers, BOM and pick-and-place files should be generated from them rather than
edited by hand.

## Component selection

The switches and encoder in the EDA projects use **borrowed footprints** — they are not
the real part numbers. The actual parts fitted are:

| Position | Part |
| --- | --- |
| Left / right click | Omron **D2FP-FN2** optical switch |
| Middle click | HUANO **7.8 mm green-dot** long straight-leg switch |
| Encoder | **TTC 9 mm gold wheel** |
| Side buttons | 2× HUANO **two-pin right-bend with positioning post** |

The same note is stored in each project's `description` field, so it is visible in the
EDA project information panel.

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

- **VGN F1 series Viper Mini SE shell (Remix)** by **老项** on Thingiverse
- <https://www.thingiverse.com/thing:6187528>
- License: **CC BY** (Creative Commons Attribution), as declared on the Thingiverse page.

This license is separate from this repository's MIT license. Keep the author credit when
redistributing the model or selling printed shells.

The carrier board was designed **around this shell**, so the stock PCB fits the model
without modification. If you change the PCB, re-check the internal clearances (sensor
height, switch posts, battery bay, USB opening) before printing.
