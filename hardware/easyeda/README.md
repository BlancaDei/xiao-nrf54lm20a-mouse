# EasyEDA Pro projects / 嘉立创EDA专业版工程

Two **嘉立创EDA专业版 (EasyEDA Pro)** projects, exported as `.epro2`.
Open them with `文件 → 打开` (use `文件 → 导入` if you received a `.zip` bundle).

| File | Board |
| --- | --- |
| `Board1.epro2` | Main board: XIAO nRF54LM20A carrier, PAW3395, switches, encoder, I²C expander, antenna |
| `SideKeys.epro2` | Side-key daughter board: two side switches (project title: 侧键) |

## Component selection / 元件选型

The same note is embedded in each project's `description` field, so it shows up in the
EDA project information panel.

> **此工程文件中按键微动与编码器只是借用封装。其具体配置如下：**
>
> - 左右按键：欧姆龙 **D2FP-FN2** 光微动
> - 中键：华诺 HUANO **7.8mm 绿点长直脚**微动
> - 编码器：**TTC 9mm 金轮**
> - 侧键：两颗 华诺 HUANO **两脚右弯带定位柱** 侧键微动
>
> The switches and encoder in these projects use **borrowed footprints only**. The actual
> parts are the ones listed above.

## Fabrication

Export Gerber, BOM and pick-and-place from the **same** revision of the project before
ordering. See [../README.md](../README.md) for the pre-fabrication checklist.
