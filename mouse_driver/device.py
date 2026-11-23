"""鼠标配置设备层。

- MouseConfig：与界面一一对应的配置模型
- MouseTransport：传输抽象（Qt 信号驱动，界面只依赖它）
- HidTransport：真实设备，走 USB Feature Report（HIDAPI）
- MockTransport：无硬件时用的模拟设备（本地 JSON）

配置协议与固件 dongle_nrf52840/src/hid_cfg.h 严格一致：
    Feature Report ID 0x06，32 字节
    写：[0]=0xA5 [1]=cmd [2]=index [3..4]=value(u16 LE)
    读：[0]=0x5A [1]=版本 [2]=有效挡位 [3]=当前挡位 [4..5]=回报率
        [6]=系统模式 [7]=运动同步 [8]=防误触发 [9]=静默高度
        [10..21]=6 档 DPI(u16 LE) [22]=链路(0 无/1 ESB/2 BLE)
"""

from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field
from pathlib import Path

from PyQt5.QtCore import QObject, QTimer, pyqtSignal

# ---- 参数取值范围（与 firmware/include/config.h 保持一致）----
DPI_MIN = 100
DPI_MAX = 16000
DPI_STEP = 100
MAX_DPI_SLOTS = 6
DEFAULT_DPI_LEVELS = [1600, 3200, 6400, 8000, 12000, 16000]

REPORT_RATES = [1000, 2000, 4000, 8000]
# 本硬件实际能做到的上限：接收器（nRF52840）只有全速 USB，HID 中断端点
# 最小 bInterval = 1 ms，端到端最多 1000 报告/s。2K 以上的选项仅协议层收得下，
# 界面里置灰，避免用户以为调上去就能跑满。
MAX_REPORT_RATE = 1000
SYSTEM_MODES = ("high", "competitive")      # 高速模式 / 竞技模式
DEBOUNCE_LEVELS = ("low", "mid", "high")    # 防误触发 低/中/高
LIFT_OFF_LEVELS = ("low", "mid", "high")    # 静默高度 低/中/高

CONFIG_PATH = Path(os.getenv("APPDATA", str(Path.home()))) / "MouseDriver" / "config.json"


@dataclass
class MouseConfig:
    dpi_levels: list = field(default_factory=lambda: list(DEFAULT_DPI_LEVELS))
    active_slots: int = 1      # 有效挡位数量
    active_slot: int = 0       # 当前生效挡位下标
    report_rate: int = 1000
    system_mode: str = "high"
    motion_sync: bool = True
    debounce: str = "low"
    lift_off: str = "low"

    def to_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict) -> "MouseConfig":
        cfg = cls()
        for key, value in data.items():
            if hasattr(cfg, key):
                setattr(cfg, key, value)
        cfg.dpi_levels = (list(cfg.dpi_levels) + DEFAULT_DPI_LEVELS)[:MAX_DPI_SLOTS]
        cfg.active_slots = max(1, min(MAX_DPI_SLOTS, int(cfg.active_slots)))
        cfg.active_slot = max(0, min(cfg.active_slots - 1, int(cfg.active_slot)))
        return cfg


def load_local_config() -> MouseConfig:
    if CONFIG_PATH.exists():
        try:
            return MouseConfig.from_dict(json.loads(CONFIG_PATH.read_text("utf-8")))
        except (ValueError, OSError):
            pass
    return MouseConfig()


def save_local_config(config: MouseConfig) -> None:
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_PATH.write_text(
        json.dumps(config.to_dict(), ensure_ascii=False, indent=2), "utf-8"
    )


class MouseTransport(QObject):
    """界面与鼠标之间的唯一通道。"""

    config_ready = pyqtSignal(object)     # MouseConfig
    applied = pyqtSignal(str, object)     # 字段名, 新值
    status = pyqtSignal(str)              # 设备状态文案

    def _set_status(self, text: str) -> None:
        """状态只在变化时上报，避免轮询把界面刷屏。"""
        if text != getattr(self, "_status_text", None):
            self._status_text = text
            self.status.emit(text)

    def open(self) -> None:
        raise NotImplementedError

    def set(self, key: str, value) -> None:
        raise NotImplementedError


class MockTransport(MouseTransport):
    """模拟设备：读写本地 JSON，用短延时模拟上报回执。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._config = MouseConfig()

    def open(self) -> None:
        self._config = load_local_config()
        self._set_status("模拟设备")
        QTimer.singleShot(120, lambda: self.config_ready.emit(self._config))

    def set(self, key: str, value) -> None:
        if not hasattr(self._config, key):
            return
        setattr(self._config, key, value)
        if key == "dpi_levels":
            self._config.dpi_levels = [int(v) for v in value]
        save_local_config(self._config)
        QTimer.singleShot(30, lambda: self.applied.emit(key, value))


class HidTransport(MouseTransport):
    """真实设备：通过 HIDAPI 读写厂商 Feature Report。

    - 未插接收器时界面照常可用，改动先存本地，插上后整包下发
    - 连上后 2 s 轮询一次状态（顺带刷新链路类型）
    - 设备上的设置优先于本地缓存：连上先读回来再覆盖界面
    """

    VENDOR_ID = 0x1915          # Nordic（开发阶段惯例，见 dongle prj.conf）
    PRODUCT_ID = 0x4242         # CONFIG_SAMPLE_USBD_PID
    REPORT_ID = 0x06
    REPORT_LEN = 32

    MAGIC_SET = 0xA5
    MAGIC_GET = 0x5A

    CMD_GET_STATE = 0x01
    CMD_SET_DPI = 0x02
    CMD_SET_SLOTS = 0x03
    CMD_SET_ACTIVE = 0x04
    CMD_SET_RATE = 0x05
    CMD_SET_MODE = 0x06
    CMD_SET_SYNC = 0x07
    CMD_SET_DEBOUNCE = 0x08
    CMD_SET_LIFTOFF = 0x09

    VENDOR_USAGE_PAGE = 0xFF00

    def __init__(self, parent=None):
        super().__init__(parent)
        self._dev = None
        self._config = load_local_config()
        self._dirty = False          # 离线期间改过设置，连上后要先下发
        self._link = 0
        self._timer = QTimer(self)
        self._timer.setInterval(2000)
        self._timer.timeout.connect(self._poll)

    # ---- 生命周期 ----
    def open(self) -> None:
        self._set_status("未连接")
        QTimer.singleShot(0, lambda: self.config_ready.emit(self._config))
        self._connect()
        self._timer.start()

    def close(self) -> None:
        self._timer.stop()
        self._disconnect()

    # ---- 连接管理 ----
    def _connect(self) -> None:
        if self._dev is not None:
            return

        try:
            import hid
        except ImportError:
            self._set_status("缺少 hidapi（pip install hidapi）")
            return

        try:
            infos = hid.enumerate(self.VENDOR_ID, self.PRODUCT_ID)
        except OSError as exc:
            self._set_status(f"枚举设备失败：{exc}")
            return

        if not infos:
            self._set_status("未检测到接收器")
            return

        # 鼠标本体和接收器可能共用 VID/PID（实测都是 1915:4242），
        # 所以按"厂商集合优先、再逐个探测 magic"来找真正的配置通道。
        infos = sorted(
            infos, key=lambda info: info.get("usage_page") != self.VENDOR_USAGE_PAGE
        )
        local_config = self._config

        for info in infos:
            dev = hid.device()
            try:
                dev.open_path(info["path"])
            except OSError:
                continue

            self._dev = dev
            if self._read_state(emit=False):
                break

            dev.close()
            self._dev = None

        if self._dev is None:
            self._set_status("未找到支持配置通道的接收器")
            return

        if self._dirty:
            # 离线期间的改动以本机为准：探测时读到的设备值只是探测用，不能覆盖
            self._config = local_config
            self._push_all()
            self._dirty = False
            self._read_state(emit=True)
        else:
            # 设备上的设置优先，直接刷到界面
            self.config_ready.emit(self._config)

        self._emit_status()

    def _disconnect(self) -> None:
        if self._dev is not None:
            try:
                self._dev.close()
            except OSError:
                pass
            self._dev = None
        self._link = 0

    def _poll(self) -> None:
        if self._dev is None:
            self._connect()
            return

        if not self._read_state(emit=True):
            self._disconnect()
            self._set_status("设备已断开")

    # ---- 协议读写 ----
    def _set_feature(self, cmd: int, index: int = 0, value: int = 0) -> bool:
        if self._dev is None:
            return False

        report = bytearray(self.REPORT_LEN)
        report[0] = self.MAGIC_SET
        report[1] = cmd
        report[2] = index
        report[3] = value & 0xFF
        report[4] = (value >> 8) & 0xFF

        try:
            sent = self._dev.send_feature_report(bytes([self.REPORT_ID]) + bytes(report))
        except OSError as exc:
            self._set_status(f"下发失败：{exc}")
            return False

        # 各平台返回值口径不一（含/不含 Report ID），>0 即视为已下发
        return sent > 0

    def _read_state(self, emit: bool) -> bool:
        if self._dev is None:
            return False

        try:
            data = self._dev.get_feature_report(self.REPORT_ID, self.REPORT_LEN + 1)
        except OSError:
            return False

        if not data or len(data) < self.REPORT_LEN:
            return False

        # Windows 会把 Report ID 放在首字节，Linux 有时不带，两种都兼容
        buf = data[1:] if data[0] == self.REPORT_ID else data
        if len(buf) < self.REPORT_LEN or buf[0] != self.MAGIC_GET:
            return False

        config = MouseConfig(
            dpi_levels=[buf[10 + i * 2] | (buf[11 + i * 2] << 8) for i in range(MAX_DPI_SLOTS)],
            active_slots=buf[2],
            active_slot=buf[3],
            report_rate=buf[4] | (buf[5] << 8),
            system_mode=SYSTEM_MODES[buf[6]] if buf[6] < 2 else "high",
            motion_sync=bool(buf[7]),
            debounce=DEBOUNCE_LEVELS[buf[8]] if buf[8] < 3 else "low",
            lift_off=LIFT_OFF_LEVELS[buf[9]] if buf[9] < 3 else "mid",
        )
        config.active_slots = max(1, min(MAX_DPI_SLOTS, config.active_slots))
        config.active_slot = max(0, min(config.active_slots - 1, config.active_slot))

        changed = config.to_dict() != self._config.to_dict()
        link_changed = buf[22] != self._link

        self._config = config
        self._link = buf[22]

        if changed:
            save_local_config(config)
            if emit:
                self.config_ready.emit(config)
        if link_changed:
            self._emit_status()

        return True

    def _emit_status(self) -> None:
        link = {0: "接收器未连上鼠标", 1: "2.4G 已连接", 2: "蓝牙已连接"}.get(
            self._link, "已连接"
        )
        self._set_status(link)

    # ---- 下发 ----
    def _push_all(self) -> None:
        cfg = self._config

        for i in range(MAX_DPI_SLOTS):
            self._set_feature(self.CMD_SET_DPI, i, cfg.dpi_levels[i])
        self._set_feature(self.CMD_SET_SLOTS, 0, cfg.active_slots)
        self._set_feature(self.CMD_SET_ACTIVE, 0, cfg.active_slot)
        self._set_feature(self.CMD_SET_RATE, 0, cfg.report_rate)
        self._set_feature(self.CMD_SET_MODE, 0, 0 if cfg.system_mode == "high" else 1)
        self._set_feature(self.CMD_SET_SYNC, 0, 1 if cfg.motion_sync else 0)
        self._set_feature(self.CMD_SET_DEBOUNCE, 0, DEBOUNCE_LEVELS.index(cfg.debounce))
        self._set_feature(self.CMD_SET_LIFTOFF, 0, LIFT_OFF_LEVELS.index(cfg.lift_off))

    def set(self, key: str, value) -> None:
        cfg = self._config
        sent = True

        if key == "dpi_levels":
            values = [int(v) for v in value]
            for i, v in enumerate(values):
                if cfg.dpi_levels[i] != v:
                    cfg.dpi_levels[i] = v
                    sent &= self._set_feature(self.CMD_SET_DPI, i, v)
        elif key == "active_slots":
            cfg.active_slots = int(value)
            sent = self._set_feature(self.CMD_SET_SLOTS, 0, cfg.active_slots)
        elif key == "active_slot":
            cfg.active_slot = int(value)
            sent = self._set_feature(self.CMD_SET_ACTIVE, 0, cfg.active_slot)
        elif key == "report_rate":
            cfg.report_rate = int(value)
            sent = self._set_feature(self.CMD_SET_RATE, 0, cfg.report_rate)
        elif key == "system_mode":
            cfg.system_mode = value
            sent = self._set_feature(self.CMD_SET_MODE, 0, 0 if value == "high" else 1)
        elif key == "motion_sync":
            cfg.motion_sync = bool(value)
            sent = self._set_feature(self.CMD_SET_SYNC, 0, 1 if cfg.motion_sync else 0)
        elif key == "debounce":
            cfg.debounce = value
            sent = self._set_feature(self.CMD_SET_DEBOUNCE, 0, DEBOUNCE_LEVELS.index(value))
        elif key == "lift_off":
            cfg.lift_off = value
            sent = self._set_feature(self.CMD_SET_LIFTOFF, 0, LIFT_OFF_LEVELS.index(value))
        else:
            return

        save_local_config(cfg)

        if self._dev is None:
            self._dirty = True
            self._set_status("未连接，设置已暂存")
            return

        if sent:
            self.applied.emit(key, value)
            return

        # 下发失败：可能是接收器被拔了，标记为未连接并暂存
        self._dirty = True
        self._disconnect()
        self._set_status("下发失败，设置已暂存")
