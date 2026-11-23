# Third-party board definitions

The board definition under `boards/arm/xiao_nrf54lm20a/` is copied from the
Seeed Studio Zephyr board support package:

- Upstream: <https://github.com/Seeed-Studio/seeed-boards-zephyr>
- Copyright (c) 2025 Seeed Technology Co., Ltd.
- License: Apache-2.0 (the `SPDX-License-Identifier` header is kept in every file)

It is vendored here only so that this project builds out of the box with
`-DBOARD_ROOT=<repo root>`. If you upgrade NCS or the board support package,
replace this directory with the upstream version.
