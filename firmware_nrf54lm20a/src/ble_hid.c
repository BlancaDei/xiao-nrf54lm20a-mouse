/*
 * BLE HID 鼠标（HID over GATT）+ LLPM
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <bluetooth/services/hids.h>
#if defined(CONFIG_BT_CTLR_SDC_LLPM)
#include <bluetooth/hci_vs_sdc.h>
#endif
#include <zephyr/settings/settings.h>

#include "ble_hid.h"

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define BASE_USB_HID_SPEC_VERSION 0x0101

/* 与官方 peripheral_hids_mouse 示例一致的上报布局 */
#define INPUT_REP_BUTTONS_LEN      3
#define INPUT_REP_MOVEMENT_LEN     3
#define INPUT_REP_BUTTONS_INDEX    0
#define INPUT_REP_MOVEMENT_INDEX   1
#define INPUT_REP_BUTTONS_ID       1
#define INPUT_REP_MOVEMENT_ID      2

/* Report ID 2 里 X/Y 各 12 bit，最大 ±2047 */
#define MOTION_MAX 0x07ff

/* Nordic 私有的 LLPM 1 ms 连接间隔值 */
#define INTERVAL_LLPM 0x0D01

BT_HIDS_DEF(hids_obj, INPUT_REP_BUTTONS_LEN, INPUT_REP_MOVEMENT_LEN);

static struct bt_conn *cur_conn;
static struct k_work adv_work;

/* adv_suppressed：主动睡眠（ble_hid_stop）期间置位，让 connected/disconnected
 * 回调不自动重播，避免与 SLEEP 状态冲突；ble_hid_start 时清掉。
 * adv_running：广播是否已经在跑，用于 ble_hid_start 做幂等（不重复起广播）。
 */
static bool adv_suppressed;
static bool adv_running;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static void adv_start(void)
{
	int err;

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		printk("ble: advertising start failed (%d)\n", err);
		return;
	}
	adv_running = true;
	printk("ble: advertising started as \"%s\"\n", DEVICE_NAME);
}

static void adv_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	adv_start();
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		printk("ble: connection to %s failed 0x%02x %s\n", addr, err,
		       bt_hci_err_to_str(err));
		/* 主动睡眠期间不重启广播 */
		if (!adv_suppressed) {
			k_work_submit(&adv_work);
		}
		return;
	}

	printk("ble: connected %s\n", addr);

	cur_conn = bt_conn_ref(conn);

	int e = bt_hids_connected(&hids_obj, conn);

	if (e) {
		printk("ble: hids_connected failed (%d)\n", e);
	}

	/* 只在当前间隔比 7.5 ms 慢时才请求更快参数（Windows 默认给 15 ms）。
	 * dongle 建链时已经用 7.5 ms，此时再请求一次会和 dongle 稍后下发的
	 * LLPM 参数更新（1~2 ms）打架，把间隔顶回 7.5 ms。
	 * 6 = 6 x 1.25 ms；latency 0；timeout 400 x 10 ms。 */
	struct bt_conn_info info;
	uint16_t cur_us = 0;

	if (bt_conn_get_info(conn, &info) == 0 && info.type == BT_CONN_TYPE_LE) {
		cur_us = info.le.interval_us;
		printk("ble: interval %u us\n", cur_us);
	}

	if (cur_us == 0 || cur_us > 7500) {
		const struct bt_le_conn_param fast = BT_LE_CONN_PARAM_INIT(6, 6, 0, 400);
		int pu = bt_conn_le_param_update(conn, &fast);

		if (pu) {
			printk("ble: param update request failed (%d)\n", pu);
		}
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("ble: disconnected %s, reason 0x%02x %s\n", addr, reason,
	       bt_hci_err_to_str(reason));

	(void)bt_hids_disconnected(&hids_obj, conn);

	if (cur_conn) {
		bt_conn_unref(cur_conn);
		cur_conn = NULL;
	}

	/* 主动睡眠（ble_hid_stop）时不重播，避免与 SLEEP 状态冲突 */
	if (!adv_suppressed) {
		k_work_submit(&adv_work);
	}
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(conn);

	/* LLPM 间隔在 HCI 里编码成 0x0Dxx：低 8 位是毫秒数 */
	if ((interval & 0xff00) == 0x0d00) {
		printk("ble: connection interval updated -> LLPM %u ms, latency %u\n",
		       (unsigned int)(interval & 0xff), latency);
	} else {
		printk("ble: connection interval updated: %u us, latency %u\n",
		       (unsigned int)interval * 1250U, latency);
	}
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		printk("ble: security failed (%s level %u, err %d %s)\n", addr, level,
		       err, bt_security_err_to_str(err));
		/* 对端（dongle）安全失败时会清掉自己的 bond 再重连重配。
		 * 本端如果保留旧 key，SMP 的 update_keys_check() 会对新配对回
		 * AUTH_REQUIREMENTS(0x04)，两边就无限"连上-失败-断开"。
		 * 这里同步删掉本端这条过期 bond，下一次连接才能走全新配对。 */
		(void)bt_unpair(BT_ID_DEFAULT, bt_conn_get_dst(conn));
	} else {
		printk("ble: security changed (%s level %u)\n", addr, level);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_updated = le_param_updated,
	.security_changed = security_changed,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("ble: pairing complete (%s, bonded %d)\n", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("ble: pairing failed (%s, reason %d %s)\n", addr, reason,
	       bt_security_err_to_str(reason));
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

static void hid_init(void)
{
	struct bt_hids_init_param init = { 0 };
	struct bt_hids_inp_rep *rep;
	static const uint8_t motion_mask[DIV_ROUND_UP(INPUT_REP_MOVEMENT_LEN, 8)] = { 0 };

	/* 与官方示例逐字节一致，避免描述符写错导致主机识别不了 */
	static const uint8_t report_map[] = {
		0x05, 0x01,       /* Usage Page (Generic Desktop) */
		0x09, 0x02,       /* Usage (Mouse) */

		0xA1, 0x01,       /* Collection (Application) */

		/* Report ID 1: 按键 + 滚轮 + 横向拨动 */
		0x85, 0x01,       /* Report Id 1 */
		0x09, 0x01,       /* Usage (Pointer) */
		0xA1, 0x00,       /* Collection (Physical) */
		0x95, 0x05,       /* Report Count (5) */
		0x75, 0x01,       /* Report Size (1) */
		0x05, 0x09,       /* Usage Page (Buttons) */
		0x19, 0x01,       /* Usage Minimum (01) */
		0x29, 0x05,       /* Usage Maximum (05) */
		0x15, 0x00,       /* Logical Minimum (0) */
		0x25, 0x01,       /* Logical Maximum (1) */
		0x81, 0x02,       /* Input (Data, Variable, Absolute) */
		0x95, 0x01,       /* Report Count (1) */
		0x75, 0x03,       /* Report Size (3) */
		0x81, 0x01,       /* Input (Constant) for padding */
		0x75, 0x08,       /* Report Size (8) */
		0x95, 0x01,       /* Report Count (1) */
		0x05, 0x01,       /* Usage Page (Generic Desktop) */
		0x09, 0x38,       /* Usage (Wheel) */
		0x15, 0x81,       /* Logical Minimum (-127) */
		0x25, 0x7F,       /* Logical Maximum (127) */
		0x81, 0x06,       /* Input (Data, Variable, Relative) */
		0x05, 0x0C,       /* Usage Page (Consumer) */
		0x0A, 0x38, 0x02, /* Usage (AC Pan) */
		0x95, 0x01,       /* Report Count (1) */
		0x81, 0x06,       /* Input (Data, Variable, Relative) */
		0xC0,             /* End Collection (Physical) */

		/* Report ID 2: 12 bit X/Y 位移 */
		0x85, 0x02,       /* Report Id 2 */
		0x09, 0x01,       /* Usage (Pointer) */
		0xA1, 0x00,       /* Collection (Physical) */
		0x75, 0x0C,       /* Report Size (12) */
		0x95, 0x02,       /* Report Count (2) */
		0x05, 0x01,       /* Usage Page (Generic Desktop) */
		0x09, 0x30,       /* Usage (X) */
		0x09, 0x31,       /* Usage (Y) */
		0x16, 0x01, 0xF8, /* Logical Minimum (-2047) */
		0x26, 0xFF, 0x07, /* Logical Maximum (2047) */
		0x81, 0x06,       /* Input (Data, Variable, Relative) */
		0xC0,             /* End Collection (Physical) */
		0xC0              /* End Collection (Application) */
	};

	init.rep_map.data = report_map;
	init.rep_map.size = sizeof(report_map);

	init.info.bcd_hid = BASE_USB_HID_SPEC_VERSION;
	init.info.b_country_code = 0x00;
	init.info.flags = (BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE);

	rep = &init.inp_rep_group_init.reports[0];
	rep->size = INPUT_REP_BUTTONS_LEN;
	rep->id = INPUT_REP_BUTTONS_ID;
	init.inp_rep_group_init.cnt++;

	rep++;
	rep->size = INPUT_REP_MOVEMENT_LEN;
	rep->id = INPUT_REP_MOVEMENT_ID;
	rep->rep_mask = motion_mask;
	init.inp_rep_group_init.cnt++;

	init.is_mouse = true;

	int err = bt_hids_init(&hids_obj, &init);

	if (err) {
		printk("ble: HIDS init failed (%d)\n", err);
	}
}

static void llpm_enable(void)
{
#if defined(CONFIG_BT_CTLR_SDC_LLPM)
	sdc_hci_cmd_vs_llpm_mode_set_t cmd = { .enable = true };
	int err = hci_vs_sdc_llpm_mode_set(&cmd);

	if (err) {
		printk("ble: LLPM mode enable failed (%d)\n", err);
	} else {
		printk("ble: LLPM mode enabled\n");
	}
#endif
}

int ble_hid_init(void)
{
	int err;

	/* HIDS 要在 bt_enable 之前注册 */
	hid_init();

	err = bt_enable(NULL);
	if (err) {
		printk("ble: bt_enable failed (%d)\n", err);
		return err;
	}

	printk("ble: bluetooth initialized\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		err = settings_load();
		if (err) {
			printk("ble: settings_load failed (%d)\n", err);
		}
		printk("ble: settings loaded\n");
	}

	if (IS_ENABLED(CONFIG_BT_SMP)) {
		err = bt_conn_auth_info_cb_register(&auth_info_cb);
		if (err) {
			printk("ble: auth info cb register failed (%d)\n", err);
		}
	}

	llpm_enable();

	k_work_init(&adv_work, adv_work_handler);
	adv_start();

	return 0;
}

bool ble_hid_connected(void)
{
	return cur_conn != NULL;
}

int ble_hid_send_motion(int16_t dx, int16_t dy)
{
	if (!cur_conn || (dx == 0 && dy == 0)) {
		return 0;
	}

	uint8_t buffer[INPUT_REP_MOVEMENT_LEN];
	uint8_t x_buf[2];
	uint8_t y_buf[2];
	int16_t x = CLAMP(dx, -MOTION_MAX, MOTION_MAX);
	int16_t y = CLAMP(dy, -MOTION_MAX, MOTION_MAX);

	BUILD_ASSERT(sizeof(buffer) == 3, "只支持 2 轴 x 12 bit");

	sys_put_le16((uint16_t)x, x_buf);
	sys_put_le16((uint16_t)y, y_buf);

	buffer[0] = x_buf[0];
	buffer[1] = (y_buf[0] << 4) | (x_buf[1] & 0x0f);
	buffer[2] = (y_buf[1] << 4) | (y_buf[0] >> 4);

	return bt_hids_inp_rep_send(&hids_obj, cur_conn, INPUT_REP_MOVEMENT_INDEX,
				    buffer, sizeof(buffer), NULL);
}

int ble_hid_send_buttons(uint8_t buttons, int8_t wheel, int8_t pan)
{
	/* 未连接时必须返回错误，不能返回 0：调用方把 0 当"已送达"，会把
	 * btn_sent 更新成按下状态、把 wheel_pending 清掉，于是移动唤醒后
	 * 重连完成前的那些点击就被静默吞掉了（表现：唤醒后按键没反应）。
	 */
	if (!cur_conn) {
		return -ENOTCONN;
	}

	uint8_t buffer[INPUT_REP_BUTTONS_LEN] = {
		(uint8_t)(buttons & 0x1f),
		(uint8_t)wheel,
		(uint8_t)pan,
	};

	return bt_hids_inp_rep_send(&hids_obj, cur_conn, INPUT_REP_BUTTONS_INDEX,
				    buffer, sizeof(buffer), NULL);
}

void ble_hid_stop(void)
{
	/* 先置抑制标志：否则下面 stop/断链触发的 disconnected 回调会立刻又把
	 * 广播拉起来，和睡眠状态冲突。软件协议栈保持开启（不用 bt_disable）。
	 */
	adv_suppressed = true;

	/* 忽略返回值：可能本来就没在广播 */
	(void)bt_le_adv_stop();
	adv_running = false;

	if (cur_conn) {
		(void)bt_conn_disconnect(cur_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

void ble_hid_start(void)
{
	/* 幂等：清掉抑制标志让回调恢复正常行为；已在广播则直接返回不重复起 */
	adv_suppressed = false;

	if (adv_running) {
		return;
	}

	adv_start();
}

void ble_hid_set_system_mode(uint8_t mode)
{
	/* BLE 变体没有配置通道（dongle 走 BLE 时只会把设置存在自己那边），
	 * 发射功率由 SoftDevice Controller 调度，这里不改。 */
	printk("ble: system mode %u ignored (tx power owned by controller)\n", mode);
}
