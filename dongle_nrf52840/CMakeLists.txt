# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dongle_nrf52840)

# 复用 Zephyr 的 sample_usbd 初始化辅助（新 USB 设备栈的描述符/配置装配）
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

if(CONFIG_APP_ESB)
  # 2.4G ESB 变体：esb_rx.c 收到鼠标 PTX 的包后塞 rep_q，main.c 照常提交 HID
  target_sources(app PRIVATE
    src/main.c
    src/esb_rx.c
    src/hid_cfg.c
  )
else()
  target_sources(app PRIVATE
    src/main.c
    src/ble_ctr.c
    src/hid_cfg.c
  )
endif()
