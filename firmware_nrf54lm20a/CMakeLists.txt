# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(xiao-fw)

if(CONFIG_APP_ESB)
  # 2.4G ESB 变体：esb_ptx.c 实现 ble_hid.h 同一套 API，main.c 不感知差异
  target_sources(app PRIVATE
    src/main.c
    src/paw3395.c
    src/input.c
    src/battery.c
    src/esb_ptx.c
    src/cfg_link.c
  )
  # ESB 负载 dx/dy 已改成 int16：单包位移上限 ±32767，
  # 超出部分由 main.c 的 pend 机制留在下轮发，不丢位移
  zephyr_compile_definitions(HID_MOTION_MAX=32767)
else()
  target_sources(app PRIVATE
    src/main.c
    src/paw3395.c
    src/ble_hid.c
    src/input.c
    src/battery.c
    src/cfg_link.c
  )
endif()
