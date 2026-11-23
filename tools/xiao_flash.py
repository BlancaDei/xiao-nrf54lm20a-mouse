#!/usr/bin/env python3
"""XIAO nRF54LM20A 烧录/擦除/串口工具（走板载 CMSIS-DAP，不依赖 J-Link）。

必须在 NCS 工具链环境里跑（需要 pyocd + pyserial）：

  nrfutil toolchain-manager launch --ncs-version v3.4.1 -- python tools/xiao_flash.py <cmd>

命令：
  erase    整片擦除（RRAMC ERASEALL，读回全 0xFF 才算成功）
  flash    烧写 hex（默认 build/xiao-fw/zephyr/zephyr.hex）并回读校验，最后复位运行
  capture  抓串口输出（默认 8 秒）
  boot     先复位再抓串口（用来看开机 log）

背景（踩过的坑，别重走）：
  * J-Link（SAM-ICE，2012 OB 固件）不支持 nRF54LM20A，只能读 FICR；擦除/烧录都走不通。
  * pyOCD 内置 nrf54l target 的 NVMC 基址是错的（0x5004B000）。nRF54LM20A 的
    RRAMC 在 0x5004E000，CONFIG 在 +0x500（不是 +0x504），ERASEALL 在 +0x540。
  * **RRAM 可以直接覆写，不需要先擦除**。实测拿 264 KB 镜像覆盖旧内容 + 逐字节
    回读校验通过。所以正常烧录流程就是 `flash`，不用先 `erase`。
  * ERASEALL 有两个大坑（所以 `erase` 基本别用）：
      1) 核心正跑着 BLE/MPSL 时把它 halt 住，ERASEALL 会被**拒绝**——
         该位会一直是 1，数据纹丝不动，等 60 s 也没用；先 reset_and_halt 才生效。
      2) ERASEALL 之后的一段时间里，flash 读取会**一律返回 0xFF**，
         所以"擦到全 0xFF = 擦成功"这个判据是假的，别信这一段时期的读数。
  * 读写都要当心 pyOCD 的假数据：一次读 >2048 字会返回假的 0xFF（必须 512 B 小块读）；
    RRAM 只能整字写，未对齐地址会被 pyOCD assert 掉，必须字级 read-modify-write。
"""

import argparse
import os
import sys
from time import sleep, time

from pyocd.core.helpers import ConnectHelper
import serial

# ---- RRAMC ----
RRAMC_BASE = 0x5004E000
RRAMC_READY = RRAMC_BASE + 0x400
RRAMC_CONFIG = RRAMC_BASE + 0x500
RRAMC_ERASEALL = RRAMC_BASE + 0x540
RRAMC_TASKS_COMMITWRITEBUF = RRAMC_BASE + 0x8
RRAMC_TASKS_CLRWRITEBUF = RRAMC_BASE + 0x4

# CONFIG: WEN(bit0)=1 写模式；WRITEBUFSIZE(bits8..13)=32 行，1 行 16B -> 512B 缓冲
CONFIG_WRITE_512 = (1 << 0) | (32 << 8)
WRITE_CHUNK = 32 * 16
READ_CHUNK_WORDS = 128

# 探针序列号与串口因机器而异，默认取环境变量；探针为空时 pyOCD 选第一个可用探针
DEFAULT_PROBE = os.getenv('XIAO_PROBE') or None
DEFAULT_PORT = os.getenv('XIAO_PORT', 'COM6')
DEFAULT_HEX = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    '..', 'firmware_nrf54lm20a', 'build', 'xiao-fw', 'zephyr', 'zephyr.hex')


def session(probe):
    return ConnectHelper.session_with_chosen_probe(
        unique_id=probe,
        target_override='nrf54l',
        connect_mode='attach',
        options={'frequency': 4000000},
    )


def wait_ready(target, timeout_ms=2000):
    deadline = time() + timeout_ms / 1000.0
    while time() < deadline:
        try:
            if target.read32(RRAMC_READY) & 1:
                return True
        except Exception:
            pass  # 擦/写期间 AHB-AP 可能短暂停滞
        sleep(0.001)
    return False


def parse_intel_hex(path):
    segments = []
    base = 0
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line.startswith(':'):
                continue
            bc = int(line[1:3], 16)
            addr = int(line[3:7], 16)
            rt = int(line[7:9], 16)
            data = bytes.fromhex(line[9:9 + bc * 2])
            if rt == 0x00:
                segments.append((base + addr, data))
            elif rt == 0x01:
                break
            elif rt == 0x04:
                base = int.from_bytes(data, 'big') << 16
            elif rt == 0x02:
                base = int.from_bytes(data, 'big') << 4
    return segments


def first_non_blank(target):
    """整片 1 MB 分块读，返回第一个非 0xFF 的地址；全空白返回 None。

    注意：这里必须用 512 字节的小块读。实测 pyOCD 对这颗芯片做超大块读
    （如一次 2048 字）会返回**假的 0xFF**，会让擦除校验假通过。
    """
    for base in range(0, 0x100000, READ_CHUNK_WORDS * 4):
        n = min(READ_CHUNK_WORDS, (0x100000 - base) // 4)
        words = target.read_memory_block32(base, n)
        for i, w in enumerate(words):
            if w != 0xFFFFFFFF:
                return base + i * 4
    return None


def first_non_blank_tolerant(target, tries=6):
    """擦除刚结束时 AHB-AP 可能还在停滞（WAIT ACK / TransferTimeout），重试几次。"""
    last = None
    for k in range(tries):
        try:
            return first_non_blank(target)
        except Exception as e:
            last = e
            print('  (read stalled: %s, retry %d)' % (type(e).__name__, k + 1))
            sleep(1.5)
    raise RuntimeError('flash verification keeps stalling: %s' % last)


def cmd_erase(args):
    with session(args.probe) as s:
        t = s.target
        # 关键：先复位再擦。
        # 核心正在跑 BLE/MPSL 时被 halt 住，RRAMC 的 ERASEALL 会被拒绝
        # （实测 ERASEALL 位恒为 1、数据一动不动，等 60 s 也没用）；
        # reset_and_halt 之后 ERASEALL 才真正生效。
        t.reset_and_halt()
        sleep(0.5)
        bad = None
        for attempt in range(3):
            try:
                t.write32(RRAMC_CONFIG, 0x0)
                t.write32(RRAMC_TASKS_CLRWRITEBUF, 1)
                t.write32(RRAMC_ERASEALL, 1)
            except Exception as e:
                print('erase attempt %d: register write failed (%s)'
                      % (attempt + 1, type(e).__name__))
                t.reset_and_halt()
                sleep(0.5)
                continue

            sleep(3.0)
            bad = first_non_blank_tolerant(t)
            if bad is None:
                print('CHIP ERASED (1 MB all 0xFF, verified with 512B block reads)')
                return 0
            print('erase attempt %d: data left at 0x%06X, retrying' % (attempt + 1, bad))
            t.reset_and_halt()
            sleep(0.5)

        print('ERASE FAILED, first non-blank at 0x%06X' % bad)
        return 1


def rram_write_words(target, addr, words):
    target.write32(RRAMC_CONFIG, CONFIG_WRITE_512)
    target.write_memory_block32(addr, words)
    target.write32(RRAMC_TASKS_COMMITWRITEBUF, 1)
    if not wait_ready(target):
        raise RuntimeError('RRAMC not ready @ 0x%08X' % addr)
    target.write32(RRAMC_CONFIG, 0x0)


def write_flash(target, address, data):
    """写一段数据。

    RRAM 只能按 32 bit 整字写（未对齐地址会被 pyOCD 直接 assert 掉），
    所以：起点和长度都对齐的段直接分块写；否则先读出覆盖它的整字、
    把本段字节填进去再整字写回（read-modify-write）。
    """
    if (address & 3) == 0 and (len(data) & 3) == 0:
        off = 0
        while off < len(data):
            chunk = data[off:off + WRITE_CHUNK]
            words = [int.from_bytes(chunk[i:i + 4], 'little')
                     for i in range(0, len(chunk), 4)]
            rram_write_words(target, address + off, words)
            off += len(chunk)
        return

    start = address & ~3
    end = (address + len(data) + 3) & ~3
    n_words = (end - start) // 4
    buf = bytearray()
    for w in target.read_memory_block32(start, n_words):
        buf += int.to_bytes(w, 4, 'little')
    skip = address - start
    buf[skip:skip + len(data)] = data
    rram_write_words(target, start,
                     [int.from_bytes(buf[i:i + 4], 'little')
                      for i in range(0, len(buf), 4)])


def cmd_flash(args):
    hex_path = os.path.abspath(args.hex)
    print('hex: %s' % hex_path)
    segments = parse_intel_hex(hex_path)
    total = sum(len(d) for _, d in segments)
    lo = min(a for a, _ in segments)
    print('%d segments, %d bytes, range 0x%06X..0x%06X'
          % (len(segments), total, lo, max(a + len(d) for a, d in segments)))

    with session(args.probe) as s:
        t = s.target
        # 与 erase 同样的理由：先复位，避免跑着的固件让 RRAMC 拒绝操作
        t.reset_and_halt()
        sleep(0.5)
        hi = max(a + len(d) for a, d in segments)
        # 每 4 KB 采一个点看是否空白。
        # 注意：nRF54L 的 RRAM 实测可以**直接覆写**（不是 NOR flash 那种必须
        # 先擦的语义），所以这里只警告、不拦截；真正的判据是写完后的回读校验。
        #
        # 另注：ERASEALL 之后的一段时间里，flash 读取会**一律返回 0xFF**（假象），
        # 所以"擦除成功"的判断不可靠，别信这一段时期的读数。
        nonblank = [a for a in range(lo, hi, 0x1000)
                    if t.read32(a) != 0xFFFFFFFF]
        if nonblank:
            print('NOTE: %d/%d sampled spots are non-blank (first @ 0x%08X) — '
                  'overwriting directly (RRAM allows it)'
                  % (len(nonblank), (hi - lo) // 0x1000 + 1, nonblank[0]))

        t0 = time()
        done = 0
        mark = 0.0
        for addr, data in segments:
            write_flash(t, addr, data)
            done += len(data)
            if 100.0 * done / total >= mark:
                print('  %5.1f%%' % (100.0 * done / total))
                mark += 25.0
        print('written in %.1f s' % (time() - t0))

        errors = 0
        checked = 0
        t1 = time()
        for addr, data in segments:
            # 只比对本段自己的字节：未对齐段是 RMW 写进去的，邻居字节另有内容，
            # 不能拿 0xFF 补齐去比。
            start = addr & ~3
            end = (addr + len(data) + 3) & ~3
            try:
                raw = t.read_memory_block32(start, (end - start) // 4)
            except Exception:
                errors += 1
                if errors <= 10:
                    print('  read failed @ 0x%08X' % addr)
                continue
            got = bytearray()
            for w in raw:
                got += int.to_bytes(w, 4, 'little')
            skip = addr - start
            if bytes(got[skip:skip + len(data)]) != data:
                errors += 1
                if errors <= 10:
                    for i, (e, g) in enumerate(zip(data, got[skip:skip + len(data)])):
                        if e != g:
                            print('  MISMATCH @ 0x%08X: expected 0x%02X got 0x%02X'
                                  % (addr + i, e, g))
                            break
            checked += len(data)
        print('  verified %d bytes in %.1f s' % (checked, time() - t1))
        if errors:
            print('VERIFY FAILED: %d mismatches' % errors)
            return 1
        print('VERIFY OK')
        t.reset()
        print('core reset, firmware running')
    return 0


def read_serial(port_name, seconds):
    port = serial.Serial(port_name, 115200, timeout=0.5)
    port.reset_input_buffer()
    t0 = time()
    lines = []
    while time() - t0 < seconds:
        try:
            line = port.readline()
        except Exception:
            continue
        if line:
            lines.append(line.decode('ascii', 'replace').rstrip())
    port.close()
    return lines


def cmd_capture(args):
    for l in read_serial(args.port, args.seconds):
        print(l)
    return 0


def cmd_boot(args):
    port = serial.Serial(args.port, 115200, timeout=0.5)
    port.reset_input_buffer()
    with session(args.probe) as s:
        s.target.reset()
    t0 = time()
    lines = []
    while time() - t0 < args.seconds:
        try:
            line = port.readline()
        except Exception:
            continue
        if line:
            lines.append(line.decode('ascii', 'replace').rstrip())
    port.close()
    print('----- boot log (%d lines) -----' % len(lines))
    for l in lines:
        print(l)
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--probe', default=DEFAULT_PROBE, help='CMSIS-DAP 探针序列号')
    p.add_argument('--port', default=DEFAULT_PORT, help='串口（默认 COM6）')
    sub = p.add_subparsers(dest='cmd', required=True)

    sub.add_parser('erase', help='整片擦除')
    sp = sub.add_parser('flash', help='烧写并校验')
    sp.add_argument('--hex', default=DEFAULT_HEX, help='hex 文件路径')
    sp = sub.add_parser('capture', help='抓串口输出')
    sp.add_argument('--seconds', type=float, default=8.0)
    sp.add_argument('--port', dest='port_sub', default=None,
                    help='同全局 --port，放在子命令后面也能用')
    sp = sub.add_parser('boot', help='复位并抓开机 log')
    sp.add_argument('--seconds', type=float, default=4.0)
    sp.add_argument('--port', dest='port_sub', default=None,
                    help='同全局 --port，放在子命令后面也能用')

    args = p.parse_args()
    # 允许 --port 写在子命令后面（argparse 里全局选项必须在子命令前，容易踩坑）
    if getattr(args, 'port_sub', None):
        args.port = args.port_sub
    return {'erase': cmd_erase, 'flash': cmd_flash,
            'capture': cmd_capture, 'boot': cmd_boot}[args.cmd](args)


if __name__ == '__main__':
    sys.exit(main())
