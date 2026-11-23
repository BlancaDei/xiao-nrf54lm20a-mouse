"""界面基础控件：卡片、信息按钮、分段选择、开关胶囊、档位滑条、DPI 挡位。"""

from __future__ import annotations

from PyQt5.QtCore import (
    QEasingCurve,
    QPointF,
    QPropertyAnimation,
    QRectF,
    Qt,
    pyqtProperty,
    pyqtSignal,
)
from PyQt5.QtGui import QColor, QFont, QLinearGradient, QPainter, QPen, QRadialGradient
from PyQt5.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSizePolicy,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

ACCENT = QColor("#1e22aa")
RED_MARK = QColor("#b01f24")
TEXT = QColor("#ffffff")
TEXT_DIM = QColor(255, 255, 255, 165)

CARD_RADIUS = 16

FONT_FAMILY = "Microsoft YaHei UI"


def tip_html(text: str) -> str:
    return f"<div style='width:236px'>{text}</div>"


class Card(QWidget):
    """半透明玻璃卡片，可选左上高光。"""

    def __init__(self, parent=None, glow: bool = False):
        super().__init__(parent)
        self._glow = glow
        self.body = QVBoxLayout(self)
        self.body.setContentsMargins(20, 16, 20, 18)
        self.body.setSpacing(0)

    def add_title(self, text: str, tip: str | None = None) -> QWidget:
        row = QWidget(self)
        lay = QHBoxLayout(row)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(8)

        label = QLabel(text, row)
        label.setStyleSheet(
            f"color: #ffffff; font-family: '{FONT_FAMILY}'; font-size: 14px; font-weight: bold;"
        )
        lay.addWidget(label)
        lay.addStretch(1)

        if tip:
            lay.addWidget(InfoButton(tip, row))
            label.setToolTip(tip_html(tip))
        self.body.addWidget(row)
        return row

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        rect = QRectF(self.rect()).adjusted(0, 0, -0.5, -0.5)

        grad = QLinearGradient(0, 0, 0, rect.height())
        grad.setColorAt(0.0, QColor(255, 255, 255, 52))
        grad.setColorAt(1.0, QColor(255, 255, 255, 30))
        p.setPen(Qt.NoPen)
        p.setBrush(grad)
        p.drawRoundedRect(rect, CARD_RADIUS, CARD_RADIUS)

        if self._glow:
            glow = QRadialGradient(
                QPointF(rect.width() * 0.62, rect.height() * 0.16), rect.width() * 0.45
            )
            glow.setColorAt(0.0, QColor(255, 255, 255, 22))
            glow.setColorAt(1.0, QColor(255, 255, 255, 0))
            p.setBrush(glow)
            p.drawRoundedRect(rect, CARD_RADIUS, CARD_RADIUS)


class InfoButton(QWidget):
    """圆圈 i 图标，悬停显示说明。"""

    def __init__(self, tip: str, parent=None):
        super().__init__(parent)
        self.setFixedSize(18, 18)
        self.setCursor(Qt.PointingHandCursor)
        self.setToolTip(tip_html(tip))

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.setPen(QPen(QColor(255, 255, 255, 200), 1.3))
        p.setBrush(Qt.NoBrush)
        p.drawEllipse(QRectF(0.5, 0.5, 17, 17))

        p.setPen(Qt.NoPen)
        p.setBrush(QColor(255, 255, 255, 200))
        p.drawEllipse(QPointF(9, 5.4), 1.15, 1.15)
        p.drawRoundedRect(QRectF(8.15, 8.0, 1.7, 5.4), 0.8, 0.8)


class SegmentedGroup(QWidget):
    """一排互斥选项（回报率 / 防误触发）。

    disabled_values 里的选项置灰不可点，用来表示"硬件做不到"的档位。
    """

    changed = pyqtSignal(object)

    def __init__(self, options, parent=None, spacing: int = 10, height: int = 38,
                 disabled_values=(), disabled_tip: str = ""):
        super().__init__(parent)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(spacing)
        self._buttons: list[QPushButton] = []
        self._values: list[object] = []

        for label, value in options:
            btn = QPushButton(label, self)
            btn.setCheckable(True)
            btn.setCursor(Qt.PointingHandCursor)
            btn.setFixedHeight(height)
            btn.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
            btn.setStyleSheet(
                f"""
                QPushButton {{
                    background: rgba(255, 255, 255, 0.28);
                    color: rgba(255, 255, 255, 0.68);
                    border: none;
                    border-radius: 5px;
                    font-family: '{FONT_FAMILY}';
                    font-size: 13px;
                    font-weight: bold;
                }}
                QPushButton:hover {{ background: rgba(255, 255, 255, 0.36); }}
                QPushButton:checked {{ background: #1e22aa; color: #ffffff; }}
                QPushButton:disabled {{
                    background: rgba(255, 255, 255, 0.10);
                    color: rgba(255, 255, 255, 0.32);
                }}
                """
            )
            if value in disabled_values:
                btn.setEnabled(False)
                btn.setCursor(Qt.ForbiddenCursor)
                if disabled_tip:
                    btn.setToolTip(disabled_tip)

            btn.clicked.connect(lambda _=False, v=value: self._on_click(v))
            lay.addWidget(btn)
            self._buttons.append(btn)
            self._values.append(value)

    def _on_click(self, value):
        self.set_value(value)
        self.changed.emit(value)

    def set_value(self, value):
        for btn, val in zip(self._buttons, self._values):
            btn.setChecked(val == value)

    def value(self):
        for btn, val in zip(self._buttons, self._values):
            if btn.isChecked():
                return val
        return self._values[0]


class TogglePill(QWidget):
    """带文字的胶囊开关。

    knob_first=True：开启时圆钮在左、文字在右（系统模式）
    knob_first=False：开启时圆钮在右、文字在左（运动同步）
    """

    toggled = pyqtSignal(bool)

    def __init__(self, on_text: str, off_text: str, checked: bool = True,
                 parent=None, width: int = 108, height: int = 30,
                 knob_first: bool = False):
        super().__init__(parent)
        self._on_text = on_text
        self._off_text = off_text
        self._checked = checked
        self._knob_first = knob_first
        self._pos = 1.0 if checked else 0.0
        self.setFixedSize(width, height)
        self.setCursor(Qt.PointingHandCursor)

        self._anim = QPropertyAnimation(self, b"knobPos", self)
        self._anim.setDuration(140)
        self._anim.setEasingCurve(QEasingCurve.InOutQuad)

    def get_knob_pos(self):
        return self._pos

    def set_knob_pos(self, value):
        self._pos = value
        self.update()

    knobPos = pyqtProperty(float, fget=get_knob_pos, fset=set_knob_pos)

    def isChecked(self) -> bool:
        return self._checked

    def setChecked(self, checked: bool, animate: bool = True):
        if checked == self._checked:
            return
        self._checked = checked
        target = 1.0 if checked else 0.0
        if animate:
            self._anim.stop()
            self._anim.setStartValue(self._pos)
            self._anim.setEndValue(target)
            self._anim.start()
        else:
            self.set_knob_pos(target)
        self.update()

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.LeftButton and self.rect().contains(event.pos()):
            self.setChecked(not self._checked)
            self.toggled.emit(self._checked)

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        h, w = self.height(), self.width()
        radius = h / 2

        p.setPen(Qt.NoPen)
        p.setBrush(ACCENT if self._checked else QColor(255, 255, 255, 64))
        p.drawRoundedRect(QRectF(0, 0, w, h), radius, radius)

        knob_d = h - 10
        travel = w - 10 - knob_d
        slider = self._pos if not self._knob_first else 1.0 - self._pos
        knob_x = 5 + slider * travel
        p.setBrush(QColor("#ffffff"))
        p.drawEllipse(QRectF(knob_x, 5, knob_d, knob_d))

        # 文字始终待在圆钮的对面，按目标状态排版，避免动画期间跳动
        knob_left = self._checked == self._knob_first
        text_left = 5 + knob_d + 9 if knob_left else 13

        p.setPen(QColor(255, 255, 255, 240 if self._checked else 228))
        font = QFont(FONT_FAMILY, 9)
        font.setBold(True)
        p.setFont(font)
        text = self._on_text if self._checked else self._off_text
        p.drawText(QRectF(text_left, 0, w - text_left - 8, h), Qt.AlignVCenter | Qt.AlignLeft, text)


class StopSlider(QWidget):
    """离散档位滑条，档位文案居中显示在轨道下方。"""

    changed = pyqtSignal(int)

    def __init__(self, labels, index: int = 0, parent=None):
        super().__init__(parent)
        self._labels = list(labels)
        self._index = max(0, min(len(self._labels) - 1, index))
        self.setFixedHeight(52)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.setCursor(Qt.PointingHandCursor)

    def index(self) -> int:
        return self._index

    def set_index(self, index: int):
        index = max(0, min(len(self._labels) - 1, index))
        if index != self._index:
            self._index = index
            self.update()

    def _stop_x(self, i: int) -> float:
        track_left, track_right = 2.0, self.width() - 2.0
        if len(self._labels) == 1:
            return track_left
        return track_left + (track_right - track_left) * i / (len(self._labels) - 1)

    def mousePressEvent(self, event):
        self._pick(event.pos().x())

    def mouseMoveEvent(self, event):
        if event.buttons() & Qt.LeftButton:
            self._pick(event.pos().x())

    def _pick(self, x: float):
        track_left, track_right = 2.0, self.width() - 2.0
        ratio = (x - track_left) / max(1.0, track_right - track_left)
        index = round(ratio * (len(self._labels) - 1))
        index = max(0, min(len(self._labels) - 1, index))
        if index != self._index:
            self._index = index
            self.update()
            self.changed.emit(index)

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        track_h = 8.0
        y = 4.0
        track = QRectF(2, y, self.width() - 4, track_h)

        p.setPen(Qt.NoPen)
        p.setBrush(QColor(239, 239, 239))
        p.drawRoundedRect(track, track_h / 2, track_h / 2)

        handle_x = self._stop_x(self._index)
        filled = QRectF(track.left(), y, max(track_h, handle_x - track.left()), track_h)
        p.setBrush(ACCENT)
        p.drawRoundedRect(filled, track_h / 2, track_h / 2)

        font = QFont(FONT_FAMILY, 9)
        p.setFont(font)
        for i, label in enumerate(self._labels):
            cx = min(max(self._stop_x(i), 16.0), self.width() - 16.0)
            p.setPen(QColor(255, 255, 255) if i == self._index else QColor(255, 255, 255, 118))
            p.drawText(QRectF(cx - 20, y + track_h + 6, 40, 20), Qt.AlignHCenter | Qt.AlignTop, label)


class DpiSlot(QWidget):
    """单个 DPI 挡位：深蓝底座 + 白色数值框，生效挡位下方有红色标记。"""

    activated = pyqtSignal(int)
    value_changed = pyqtSignal(int, int)

    def __init__(self, index: int, value: int, active: bool = False, parent=None):
        super().__init__(parent)
        self._index = index
        self._active = active
        self.setFixedSize(64, 74)
        self.setCursor(Qt.PointingHandCursor)

        self.spin = QSpinBox(self)
        self.spin.setRange(100, 16000)
        self.spin.setSingleStep(100)
        self.spin.setValue(value)
        self.spin.setAlignment(Qt.AlignCenter)
        self.spin.setGeometry(5, 12, 54, 32)
        self.spin.setStyleSheet(
            f"""
            QSpinBox {{
                background: #ffffff;
                border: none;
                border-radius: 4px;
                color: #1e22aa;
                font-family: '{FONT_FAMILY}';
                font-size: 12px;
                font-weight: bold;
                padding: 0 2px;
            }}
            QSpinBox::up-button, QSpinBox::down-button {{ width: 0px; height: 0px; }}
            """
        )
        self.spin.valueChanged.connect(lambda v: self.value_changed.emit(self._index, v))
        self.spin.installEventFilter(self)

    def set_active(self, active: bool):
        if active != self._active:
            self._active = active
            self.update()

    def set_value(self, value: int):
        self.spin.blockSignals(True)
        self.spin.setValue(value)
        self.spin.blockSignals(False)

    def eventFilter(self, obj, event):
        if obj is self.spin and event.type() == event.MouseButtonPress:
            self.activated.emit(self._index)
        return super().eventFilter(obj, event)

    def mousePressEvent(self, event):
        self.activated.emit(self._index)

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.setPen(Qt.NoPen)
        p.setBrush(ACCENT)
        p.drawRoundedRect(QRectF(0, 0, 64, 56), 6, 6)

        if self._active:
            p.setBrush(RED_MARK)
            p.drawRoundedRect(QRectF(22, 63, 20, 3.2), 1.6, 1.6)
