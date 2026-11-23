# EasyEDA Pro project

Place the exported **嘉立创EDA专业版 (EasyEDA Pro)** project here. The project is the
source of truth for the schematic and the PCB layout.

Expected contents:

| File | Description |
| --- | --- |
| `*.epro` | EasyEDA Pro project: schematic + PCB, opens with `文件 → 打开` |
| `gerber/*.zip` | Gerber + drill export from the same revision |
| `bom.csv` | BOM exported from the same revision |
| `cpl.csv` | Pick-and-place / centroid file |

Keep the project revision and the fabrication exports in sync. If a `.zip` project
bundle is uploaded instead of `.epro`, import it with `文件 → 导入`.
