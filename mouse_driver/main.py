"""鼠标驱动（配置端）主程序。

界面按截图布局：左侧 DPI + 回报率 + 防误触发，右侧 系统模式 / 运动同步 / 静默高度。
当前接的是模拟设备（MockTransport），配置写入本地 JSON；
固件补齐厂商 HID 通道后，把 build_transport() 换成 HidTransport 即可。
"""

from __future__ import annotations

import sys

from PyQt5.QtCore import QPointF, Qt
from PyQt5.QtGui import QColor, QLinearGradient, QPainter, QRadialGradient
from PyQt5.QtWidgets import (
    QApplication,
    QHBoxLayout,
    QLabel,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from device import (
    LIFT_OFF_LEVELS,
    MAX_DPI_SLOTS,
    MAX_REPORT_RATE,
    REPORT_RATES,
    HidTransport,
    MockTransport,
    MouseConfig,
)
from widgets import (
    FONT_FAMILY,
    Card,
    DpiSlot,
    SegmentedGroup,
    StopSlider,
    TogglePill,
)

TIP_SYSTEM_MODE = (
    "竞技模式相比高速模式提高了传感器扫描鼠标移动的帧率以及无线发射的功率，"
    "带来更精准稳定的定位体验，但是相比高速模式续航时间会缩短近一半。"
)
TIP_MOTION_SYNC = (
    "打开运动同步能够让电脑更稳定获取鼠标传感器最新的移动数据，"
    "从而使得移动轨迹更加平滑可控，但是会增加微弱的移动延迟。"
)
TIP_REPORT_RATE = (
    "请注意，高于1000Hz的轮询速率需要更多的CPU资源，并且可能导致游戏过程中帧速率下降。\n"
    "本硬件受接收器全速 USB 限制，实际最高 1 kHz。"
)
TIP_DEBOUNCE = (
    "防误触发功能优化在抬鼠复位时因按键灵敏度过高导致的按键信号误触发。"
    "此功能默认低档位，如果抬鼠距离高或复位时鼠标着面力度较大依然导致误触，可调其它档位。"
)


def small_label(text: str, alpha: int = 225) -> QLabel:
    label = QLabel(text)
    label.setStyleSheet(
        f"color: rgba(255, 255, 255, {alpha}); font-family: '{FONT_FAMILY}'; font-size: 12px;"
    )
    return label


class DpiCard(Card):
    def __init__(self, transport, parent=None):
        super().__init__(parent, glow=True)
        self._transport = transport
        self._config = MouseConfig()
        self._slots: list[DpiSlot] = []
        self._columns: list[QWidget] = []

        header = self.add_title("DPI")

        self.slot_spin = QSpinBox(header)
        self.slot_spin.setRange(1, MAX_DPI_SLOTS)
        self.slot_spin.setAlignment(Qt.AlignCenter)
        self.slot_spin.setButtonSymbols(QSpinBox.NoButtons)
        self.slot_spin.setFixedSize(44, 26)
        self.slot_spin.setStyleSheet(
            f"""
            QSpinBox {{
                background: #ffffff;
                border: none;
                border-radius: 4px;
                color: #4a4a6e;
                font-family: '{FONT_FAMILY}';
                font-size: 12px;
                font-weight: bold;
            }}
            """
        )

        header_lay = header.layout()
        header_lay.addWidget(small_label("有效挡位", 225))
        header_lay.addWidget(self.slot_spin)

        self.body.addStretch(1)

        slots_row = QWidget(self)
        slots_lay = QHBoxLayout(slots_row)
        slots_lay.setContentsMargins(0, 0, 0, 0)
        slots_lay.setSpacing(14)
        slots_lay.setAlignment(Qt.AlignLeft)

        for i in range(MAX_DPI_SLOTS):
            column = QWidget(slots_row)
            column_lay = QVBoxLayout(column)
            column_lay.setContentsMargins(0, 0, 0, 0)
            column_lay.setSpacing(4)

            caption = small_label(f"挡位{i + 1}", 215)
            caption.setAlignment(Qt.AlignHCenter)
            column_lay.addWidget(caption)

            slot = DpiSlot(i, self._config.dpi_levels[i], parent=column)
            slot.value_changed.connect(self._on_slot_value)
            slot.activated.connect(self._on_slot_activated)
            column_lay.addWidget(slot, 0, Qt.AlignHCenter)

            self._slots.append(slot)
            self._columns.append(column)
            slots_lay.addWidget(column)

        self.body.addWidget(slots_row, 0, Qt.AlignLeft)
        self.body.addStretch(1)

        self.slot_spin.valueChanged.connect(self._on_slot_count)

    def load(self, config: MouseConfig):
        self._config = config
        self.slot_spin.blockSignals(True)
        self.slot_spin.setValue(config.active_slots)
        self.slot_spin.blockSignals(False)
        for i, slot in enumerate(self._slots):
            slot.set_value(config.dpi_levels[i])
            self._columns[i].setVisible(i < config.active_slots)
        self._apply_active_marker()

    def _apply_active_marker(self):
        for i, slot in enumerate(self._slots):
            slot.set_active(i == self._config.active_slot)

    def _on_slot_value(self, index: int, value: int):
        values = list(self._config.dpi_levels)
        values[index] = value
        self._config.dpi_levels = values
        self._transport.set("dpi_levels", values)

    def _on_slot_activated(self, index: int):
        if index == self._config.active_slot:
            return
        self._config.active_slot = index
        self._apply_active_marker()
        self._transport.set("active_slot", index)

    def _on_slot_count(self, count: int):
        self._config.active_slots = count
        if self._config.active_slot >= count:
            self._config.active_slot = count - 1
        for i, column in enumerate(self._columns):
            column.setVisible(i < count)
        self._apply_active_marker()
        self._transport.set("active_slots", count)


class MainWindow(QWidget):
    def __init__(self, transport):
        super().__init__()
        self._transport = transport
        self.setWindowTitle("鼠标驱动 - 3-Mode Mouse")
        self.resize(740, 520)
        self.setMinimumSize(740, 520)
        self.setFocusPolicy(Qt.StrongFocus)

        root = QHBoxLayout(self)
        root.setContentsMargins(28, 28, 28, 28)
        root.setSpacing(14)

        left = QVBoxLayout()
        left.setSpacing(12)
        self.dpi_card = DpiCard(transport, self)
        left.addWidget(self.dpi_card, 300)

        bottom = QHBoxLayout()
        bottom.setSpacing(12)

        rate_card = Card(self)
        rate_card.add_title("回报率", TIP_REPORT_RATE)
        rate_card.body.addStretch(1)
        self.rate_group = SegmentedGroup(
            [(f"{r // 1000}K", r) for r in REPORT_RATES],
            rate_card,
            disabled_values=tuple(r for r in REPORT_RATES if r > MAX_REPORT_RATE),
            disabled_tip="本硬件不支持：接收器为全速 USB（1 ms 一帧），端到端上限 1 kHz",
        )
        self.rate_group.changed.connect(lambda v: transport.set("report_rate", v))
        rate_card.body.addWidget(self.rate_group)

        hint = small_label("硬件上限 1 kHz，2K 以上不可用", 150)
        hint.setWordWrap(True)
        rate_card.body.addWidget(hint)
        rate_card.body.addStretch(1)
        bottom.addWidget(rate_card, 274)

        debounce_card = Card(self)
        debounce_card.add_title("防误触发", TIP_DEBOUNCE)
        debounce_card.body.addStretch(1)
        self.debounce_group = SegmentedGroup(
            [("低", "low"), ("中", "mid"), ("高", "high")], debounce_card
        )
        self.debounce_group.changed.connect(lambda v: transport.set("debounce", v))
        debounce_card.body.addWidget(self.debounce_group)
        bottom.addWidget(debounce_card, 216)

        left.addLayout(bottom, 152)
        root.addLayout(left, 494)

        right = QVBoxLayout()
        right.setSpacing(10)

        mode_card = Card(self)
        mode_card.add_title("系统模式", TIP_SYSTEM_MODE)
        mode_card.body.addStretch(1)
        self.mode_pill = TogglePill(
            "高速模式", "竞技模式", checked=True, parent=mode_card, knob_first=True
        )
        self.mode_pill.toggled.connect(
            lambda on: transport.set("system_mode", "high" if on else "competitive")
        )
        mode_card.body.addWidget(self.mode_pill, 0, Qt.AlignLeft)
        mode_card.body.addStretch(1)
        self.mode_card = mode_card
        right.addWidget(mode_card, 160)

        sync_card = Card(self)
        sync_card.add_title("运动同步", TIP_MOTION_SYNC)
        sync_card.body.addStretch(1)
        self.sync_pill = TogglePill("开", "关", checked=True, parent=sync_card)
        self.sync_pill.toggled.connect(lambda on: transport.set("motion_sync", on))
        sync_card.body.addWidget(self.sync_pill, 0, Qt.AlignLeft)
        sync_card.body.addStretch(1)
        self.sync_card = sync_card
        right.addWidget(sync_card, 152)

        lift_card = Card(self)
        lift_card.add_title("静默高度")
        lift_card.body.addStretch(1)
        self.lift_slider = StopSlider(["低", "中", "高"], index=1, parent=lift_card)
        self.lift_slider.changed.connect(
            lambda i: transport.set("lift_off", LIFT_OFF_LEVELS[i])
        )
        lift_card.body.addWidget(self.lift_slider)
        lift_card.body.addStretch(1)
        self.lift_card = lift_card
        right.addWidget(lift_card, 132)

        root.addLayout(right, 176)

        transport.config_ready.connect(self._load_config)
        transport.status.connect(self._on_status)
        self._load_config(MouseConfig())
        transport.open()

    def _on_status(self, text: str):
        self.setWindowTitle(f"鼠标驱动 - 3-Mode Mouse（{text}）")

    def showEvent(self, event):
        super().showEvent(event)
        # 避免窗口打开时焦点自动落到数值框、出现编辑光标
        self.setFocus()

    def _load_config(self, config: MouseConfig):
        self.dpi_card.load(config)
        self.rate_group.set_value(min(config.report_rate, MAX_REPORT_RATE))
        self.debounce_group.set_value(config.debounce)
        self.mode_pill.setChecked(config.system_mode == "high", animate=False)
        self.sync_pill.setChecked(config.motion_sync, animate=False)
        self.lift_slider.set_index(LIFT_OFF_LEVELS.index(config.lift_off))

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)

        base = QLinearGradient(0, 0, self.width(), self.height())
        base.setColorAt(0.0, QColor("#696bc3"))
        base.setColorAt(0.5, QColor("#a99bd4"))
        base.setColorAt(1.0, QColor("#9c8cca"))
        p.fillRect(self.rect(), base)

        vignette = QRadialGradient(
            QPointF(self.width(), self.height()), self.width() * 0.62
        )
        vignette.setColorAt(0.0, QColor(22, 15, 131, 255))
        vignette.setColorAt(0.55, QColor(22, 15, 131, 90))
        vignette.setColorAt(1.0, QColor(22, 15, 131, 0))
        p.fillRect(self.rect(), vignette)


def build_transport(mock: bool = False):
    """默认连真机（dongle 的厂商 HID 通道），--mock 时用本地模拟设备。"""
    return MockTransport() if mock else HidTransport()


def main():
    QApplication.setAttribute(Qt.AA_EnableHighDpiScaling, True)
    QApplication.setAttribute(Qt.AA_UseHighDpiPixmaps, True)
    app = QApplication(sys.argv)
    app.setStyleSheet(
        f"""
        QToolTip {{
            background-color: #2f2d4f;
            color: #ffffff;
            border: 1px solid rgba(255, 255, 255, 0.14);
            border-radius: 8px;
            padding: 9px 11px;
            font-family: '{FONT_FAMILY}';
            font-size: 12px;
        }}
        QWidget {{ font-family: '{FONT_FAMILY}'; }}
        """
    )

    window = MainWindow(build_transport("--mock" in sys.argv))
    window.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
