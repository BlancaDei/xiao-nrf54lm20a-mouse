/*
 * BLE Central 接收层（dongle 侧）实现
 *
 * 流程：扫描 "XIAO Mouse" -> 连接 -> 提升到 L2 加密 -> 发现 HID 服务
 *       -> 发现 Report 特征 -> 逐个订阅 -> 解析通知 -> 投递 rep_q
 *
 * 报告解析（HID over GATT，特征值首字节是 Report ID）：
 *   ID 1: [1, btn, wheel, pan]          -> 按键 / 滚轮 / 横向
 *   ID 2: [2, x_lo, xhi_y_lo, y_hi]    -> X/Y 各 12 bit 有符号
 * 合成 dongle 的 4 字节 USB 报告：{btn, dx, dy, wheel}
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>
#if defined(CONFIG_BT_CTLR_SDC_LLPM)
#include <bluetooth/hci_vs_sdc.h>
#endif

#include "ble_ctr.h"

/* 与 main.c 共享的 USB HID 报告队列（6 字节：btn, dxL, dxH, dyL, dyH, wheel） */
extern struct k_msgq rep_q;

#define REP_LEN 6

static struct bt_conn *conn;
static atomic_t rx_total;
static atomic_t rx_err;
static atomic_t last_rx_ms;
static volatile bool subscribed;

/* GATT 发现状态 */
static struct bt_gatt_discover_params disc;
static uint16_t hid_start;
static uint16_t hid_end;
static uint16_t rep_handles[4];
static int rep_cnt;

/* 订阅参数：最多两个 Report 特征（按键 / 位移） */
static struct bt_gatt_subscribe_params subs[4];

/* 订阅结果/重试状态（订阅回调只能靠它判断 CCCD 写入是否真的成功） */
static atomic_t sub_failed;
static uint8_t sub_retries;

/* 每连接只做一次发现 / 一次订阅。
 * 重复调用会踩两个坑：一是发现状态机被同一个 disc 结构并发复用，
 * 二是 repeat 订阅会把协议栈里的订阅链表搞乱（见 subscribe_reports 的说明）。
 */
static bool disc_started;
static bool subs_started;

/* 累计收到的按键报告数（诊断用，非阻塞） */
static atomic_t btn_cnt;

/* ---- 电量（标准 Battery Service 0x180F / 0x2A19）----
 * 鼠标把电量挂到 BAS 上（见鼠标 prj.conf 的 CONFIG_BT_BAS），
 * dongle 连上后先读一次当前值，再订阅后续变化，用来驱动红灯做低电提示。
 * 这里先做声明，具体实现在 HID 订阅之后（避免 ATT 请求互相挤占）。
 */
static struct bt_gatt_discover_params bas_disc;
static struct bt_gatt_read_params bas_read;
static struct bt_gatt_subscribe_params bas_sub;
static uint16_t bas_value_handle;
static atomic_t batt_pct = ATOMIC_INIT(-1);

static void bas_work_handler(struct k_work *w);
K_WORK_DELAYABLE_DEFINE(bas_work, bas_work_handler);

/* 诊断计数（只打在这条 5 s 统计行里，避免被 CDC 控制台丢行）：
 *   sec_ok/sec_err：security_changed 的成功/失败次数
 *   disc_ok       ：特征发现的完成次数（on_char 收到 attr == NULL）
 *   sub_ok        ：bt_gatt_subscribe 返回 0 或 -EALREADY 的次数
 *   disc_evt      ：断链事件数
 */
static atomic_t sec_ok, sec_err, disc_ok, sub_ok, disc_evt;

/* BAS 诊断：发现成功、找到服务、找到特征、读成功、读失败 */
static atomic_t bas_disc_ok, bas_found, bas_char_ok, bas_read_ok, bas_read_err;
static uint8_t bas_tries;

/* 合成报告用的状态 */
static uint8_t cur_btn;
static int8_t cur_wheel;
static int8_t cur_pan;

static void start_scan(void);

/* LLPM（Nordic 私有低延迟模式）：两端都必须是 Nordic SDC 且都使能 LLPM。
 * 注意：LLPM 模式必须在**建链之前**使能（SDC 的行为），否则之后下发
 * 1 ms 间隔会返回 0x11 Unsupported Feature/Parameter。
 * 使能后可以用 vendor specific 的 conn_update 按**微秒**下发间隔，
 * 合法范围 1000..7000 us（1 ms 步进）。 */
static int llpm_mode_enable(void)
{
#if defined(CONFIG_BT_CTLR_SDC_LLPM)
	sdc_hci_cmd_vs_llpm_mode_set_t mode = { .enable = true };
	int err = hci_vs_sdc_llpm_mode_set(&mode);

	printk("ble: LLPM mode %s (%d)\n", err ? "failed" : "enabled", err);
	return err;
#else
	return -ENOTSUP;
#endif
}

static int llpm_try_interval(uint16_t handle, uint32_t interval_us)
{
	sdc_hci_cmd_vs_conn_update_t upd = {
		.conn_handle = handle,
		.conn_interval_us = interval_us,
		.conn_latency = 0,
		.supervision_timeout = 400,
	};

	int err = hci_vs_sdc_conn_update(&upd);

	printk("ble: LLPM %u us requested (handle %u) err %d\n",
	       interval_us, handle, err);
	return err;
}

static void llpm_apply(struct bt_conn *c)
{
#if defined(CONFIG_BT_CTLR_SDC_LLPM)
	uint16_t handle = 0;
	int err;

	if (bt_hci_get_conn_handle(c, &handle) != 0) {
		printk("ble: LLPM: no conn handle\n");
		return;
	}

	/* 先按标准参数更新走 LLPM 编码（0x0D00 + 毫秒数）：
	 * BT_CTLR_SDC_LLPM 会 select BT_CONN_PARAM_ANY，主机允许这种编码。
	 * 语法：interval_min/max 用 0x0D01 = 1 ms、0x0D02 = 2 ms。 */
	const struct bt_le_conn_param one_ms =
		BT_LE_CONN_PARAM_INIT(0x0D01, 0x0D01, 0, 400);

	err = bt_conn_le_param_update(c, &one_ms);
	printk("ble: LLPM 1 ms (0x0D01) param update request err %d\n", err);

	if (!err) {
		return;
	}

	/* 标准路径被拒再用 vendor specific 命令，实测这颗 SDC 接受 2 ms */
	if (llpm_try_interval(handle, 2000) == 0) {
		return;
	}

	if (llpm_try_interval(handle, 5000) == 0) {
		return;
	}

	(void)llpm_try_interval(handle, 7000);
#else
	ARG_UNUSED(c);
#endif
}

/* 连接建立瞬间鼠标自己会请求 7.5 ms（bt_conn_le_param_update），
 * 上一个参数更新过程没结束就再下发 1 ms，SDC 会返回 0x11。
 * 所以延后 500 ms 再切 LLPM。 */
static void llpm_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (conn) {
		llpm_apply(conn);
	}
}

K_WORK_DELAYABLE_DEFINE(llpm_work, llpm_work_handler);


/* 扫描重试：连接/加密失败后不要立刻重扫，否则同一时刻反复
 * bt_conn_le_create() 会因为上一个连接对象还没释放而返回 -EINVAL。 */
static void scan_retry_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	start_scan();
}

K_WORK_DELAYABLE_DEFINE(scan_retry_work, scan_retry_handler);

static void schedule_scan_retry(int delay_ms)
{
	(void)k_work_reschedule(&scan_retry_work, K_MSEC(delay_ms));
}


static void put_rep(uint8_t btn, int16_t dx, int16_t dy, int8_t wheel)
{
	uint8_t rep[REP_LEN];

	rep[0] = btn;
	rep[1] = (uint8_t)((uint16_t)dx & 0xFF);
	rep[2] = (uint8_t)(((uint16_t)dx >> 8) & 0xFF);
	rep[3] = (uint8_t)((uint16_t)dy & 0xFF);
	rep[4] = (uint8_t)(((uint16_t)dy >> 8) & 0xFF);
	rep[5] = (uint8_t)wheel;

	if (k_msgq_put(&rep_q, rep, K_NO_WAIT) != 0) {
		uint8_t drop[REP_LEN];

		(void)k_msgq_get(&rep_q, drop, K_NO_WAIT);
		(void)k_msgq_put(&rep_q, rep, K_NO_WAIT);
	}
}

/* 12 bit 有符号展开 */
static int16_t s12(uint16_t v)
{
	return (int16_t)((v & 0x0800) ? ((int32_t)v - 4096) : (int32_t)v);
}

static uint8_t on_notify(struct bt_conn *c, struct bt_gatt_subscribe_params *params,
			 const void *data, uint16_t length)
{
	ARG_UNUSED(c);

	if (!data || length == 0) {
		/* 订阅被注销 */
		return BT_GATT_ITER_STOP;
	}

	const uint8_t *d = (const uint8_t *)data;

	/* NCS 的 HIDS 发通知时 params.data = rep，**不带 Report ID**
	 * （hids.c 里 params.data = rep; params.len = rep->size）。
	 * 所以只能靠订阅时记录的 value_handle 区分两路报告：
	 *   rep_handles[0] = Report 1，载荷 [btn, wheel, pan]
	 *   rep_handles[1] = Report 2，载荷 [x_lo, y_lo/x_hi, y_hi]
	 */
	if (params->value_handle == rep_handles[0] && length >= 3) {
		uint8_t btn = (uint8_t)(d[0] & 0x1f);
		int8_t wheel = (int8_t)d[1];
		int8_t pan = (int8_t)d[2];

		cur_btn = btn;
		cur_wheel = wheel;
		cur_pan = pan;
		atomic_inc(&btn_cnt);
		/* 按键/滚轮变化单独发一包，位移为 0 */
		put_rep(btn, 0, 0, wheel);
	} else if (params->value_handle == rep_handles[1] && length >= 3) {
		uint16_t xr = (uint16_t)(d[0] | ((d[1] & 0x0F) << 8));
		uint16_t yr = (uint16_t)((d[1] >> 4) | (d[2] << 4));
		int16_t dx = s12(xr);
		int16_t dy = s12(yr);

		put_rep(cur_btn, dx, dy, 0);
	} else {
		atomic_inc(&rx_err);
		return BT_GATT_ITER_CONTINUE;
	}

	atomic_inc(&rx_total);
	atomic_set(&last_rx_ms, (atomic_val_t)k_uptime_get());
	return BT_GATT_ITER_CONTINUE;
}

static void subscribe_reports(void);

static uint8_t on_char(struct bt_conn *c, const struct bt_gatt_attr *attr,
		       struct bt_gatt_discover_params *params)
{
	if (!attr) {
		/* 特征发现结束 */
		atomic_inc(&disc_ok);
		printk("ble: HID report chars = %d\n", rep_cnt);
		subscribe_reports();
		return BT_GATT_ITER_STOP;
	}

	const struct bt_gatt_chrc *chrc = attr->user_data;

	if (bt_uuid_cmp(chrc->uuid, BT_UUID_HIDS_REPORT) == 0 &&
	    (chrc->properties & BT_GATT_CHRC_NOTIFY) && rep_cnt < ARRAY_SIZE(rep_handles)) {
		rep_handles[rep_cnt++] = chrc->value_handle;
	}

	return BT_GATT_ITER_CONTINUE;
}

static uint8_t on_primary(struct bt_conn *c, const struct bt_gatt_attr *attr,
			  struct bt_gatt_discover_params *params)
{
	if (!attr) {
		printk("ble: HID service not found\n");
		return BT_GATT_ITER_STOP;
	}

	const struct bt_gatt_service_val *svc = attr->user_data;

	hid_start = attr->handle;
	hid_end = svc->end_handle;
	printk("ble: HID service handles %u..%u\n", hid_start, hid_end);

	disc.uuid = NULL;
	disc.start_handle = hid_start + 1;
	disc.end_handle = hid_end;
	disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
	disc.func = on_char;

	int err = bt_gatt_discover(conn, &disc);

	if (err) {
		printk("ble: char discover failed %d\n", err);
	}

	return BT_GATT_ITER_STOP;
}

static void start_discovery(void)
{
	/* security_changed 可能在同一连接上连触发两次（L2/L3），
	 * 第二次再跑发现会与第一次共用 disc/rep_handles，把订阅搞成两份。 */
	if (disc_started) {
		return;
	}
	disc_started = true;

	rep_cnt = 0;
	sub_retries = 0;
	atomic_clear(&sub_failed);
	memset(&disc, 0, sizeof(disc));
	disc.uuid = BT_UUID_HIDS;
	disc.start_handle = 0x0001;
	disc.end_handle = 0xffff;
	disc.type = BT_GATT_DISCOVER_PRIMARY;
	disc.func = on_primary;

	int err = bt_gatt_discover(conn, &disc);

	if (err) {
		printk("ble: service discover failed %d\n", err);
	}
}

/* 订阅 CCCD 的最终结果只有这个回调能拿到：bt_gatt_subscribe() 返回 0
 * 只代表"请求已发出"。重连后鼠标正忙于 1 kHz 通知时，CCC 写入可能被对方
 * 拒绝，此时必须重试，否则那一路通知永远不会到达（实测：重连后按键全失效，
 * 位移正常，dongle 侧一条 btn 通知都收不到）。
 */
static void on_subscribed(struct bt_conn *c, uint8_t err,
			  struct bt_gatt_subscribe_params *params)
{
	ARG_UNUSED(c);

	printk("ble: sub result h=%u err=%u\n", params->value_handle, err);

	if (err) {
		/* 先把它从协议栈的订阅链表里摘掉：否则重试时 bt_gatt_subscribe()
		 * 会因"同一 params 已在链表里"直接返回 -EALREADY，CCC 永远不重写。 */
		(void)bt_gatt_unsubscribe(c, params);
		subs_started = false;
		atomic_set(&sub_failed, 1);
	}
}

static void sub_retry_handler(struct k_work *w)
{
	ARG_UNUSED(w);

	if (!conn || !atomic_get(&sub_failed)) {
		return;
	}
	if (sub_retries >= 8) {
		printk("ble: giving up resubscribe\n");
		return;
	}

	sub_retries++;
	atomic_clear(&sub_failed);
	printk("ble: resubscribe attempt %u\n", sub_retries);
	subscribe_reports();
}

K_WORK_DELAYABLE_DEFINE(sub_retry_work, sub_retry_handler);

static void subscribe_reports(void)
{
	/* 同一连接只订阅一次：重复订阅时第二次会命中 bt_gatt_subscribe() 的
	 * "同一 params 指针已在链表里"分支并直接返回 -EALREADY，而在此之前
	 * 清空 params 会把协议栈订阅链表的 next 指针打成 NULL，链表从此截断。 */
	if (subs_started) {
		return;
	}
	subs_started = true;

	for (int i = 0; i < rep_cnt; i++) {
		/* **不要 memset**：本对端是已绑定设备，断链时协议栈会保留订阅节点
		 * （remove_subscriptions 只清 SENT 标志），清零 node.next 会把链表截断，
		 * 结果是重连后一路通知还能分发、另一路永远收不到
		 * （实测：位移正常、按键全丢，且鼠标侧 bt_gatt_notify 仍返回 0）。
		 */
		subs[i].notify = on_notify;
		subs[i].subscribe = on_subscribed;
		subs[i].value_handle = rep_handles[i];
		/* HID Report 特征下 CCCD 紧跟特征值（Zephyr bt_hids 的描述符顺序） */
		subs[i].ccc_handle = rep_handles[i] + 1;
		subs[i].value = BT_GATT_CCC_NOTIFY;
		/* 标记为易失：断链时让协议栈把该订阅从链表里摘掉，
		 * 下次连接就由我们自己重新写 CCC，而不是靠协议栈"保留+自动重订阅"。
		 * 鼠标端的 CCC 是按连接重置的，这样每次都能确定地把两路都打开。
		 */
		atomic_set_bit(subs[i].flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

		int err = bt_gatt_subscribe(conn, &subs[i]);

		if (err && err != -EALREADY) {
			printk("ble: subscribe %d failed %d\n", i, err);
		} else {
			atomic_inc(&sub_ok);
			printk("ble: subscribed report %d (val %u)\n", i, rep_handles[i]);
		}
	}
	subscribed = true;
	/* 有结果回调报了错就过 300 ms 再试一次 */
	if (atomic_get(&sub_failed)) {
		k_work_reschedule(&sub_retry_work, K_MSEC(300));
	}

	/* HID 的 CCC 写完之后再去发现电量服务，避免两条 ATT 请求挤在一起 */
	(void)k_work_reschedule(&bas_work, K_MSEC(500));
}

/* ---- 电量（标准 Battery Service 0x180F / 0x2A19） ----
 * 鼠标把电量挂到 BAS 上（见鼠标 prj.conf 的 CONFIG_BT_BAS），
 * dongle 连上后先读一次当前值，再订阅后续变化，用来驱动红灯做低电提示。
 *
 * 注意：bas_sub 与 HID 的两路订阅一样不能 memset —— 已绑定对端断链时
 * 协议栈会保留订阅节点，清零会把链表截断（详见 subscribe_reports 的注释）。
 */
static uint8_t on_bas_notify(struct bt_conn *c,
			     struct bt_gatt_subscribe_params *params,
			     const void *data, uint16_t length)
{
	ARG_UNUSED(c);

	if (!data || length < 1) {
		return BT_GATT_ITER_STOP;
	}

	atomic_set(&batt_pct, ((const uint8_t *)data)[0]);
	atomic_inc(&bas_read_ok);
	return BT_GATT_ITER_CONTINUE;
}

static void on_bas_subscribed(struct bt_conn *c, uint8_t err,
			      struct bt_gatt_subscribe_params *params)
{
	ARG_UNUSED(c);

	printk("ble: bas sub h=%u err=%u\n", params->value_handle, err);
}

static uint8_t on_bas_read(struct bt_conn *c, uint8_t err,
			   struct bt_gatt_read_params *params,
			   const void *data, uint16_t length)
{
	ARG_UNUSED(c);
	ARG_UNUSED(params);

	if (err == 0 && data && length >= 1) {
		atomic_set(&batt_pct, ((const uint8_t *)data)[0]);
		atomic_inc(&bas_read_ok);
	} else {
		atomic_inc(&bas_read_err);
	}

	return BT_GATT_ITER_STOP;
}

static uint8_t on_bas_char(struct bt_conn *c, const struct bt_gatt_attr *attr,
			   struct bt_gatt_discover_params *params)
{
	if (!attr) {
		return BT_GATT_ITER_STOP;
	}

	const struct bt_gatt_chrc *chrc = attr->user_data;

	if (bt_uuid_cmp(chrc->uuid, BT_UUID_BAS_BATTERY_LEVEL) != 0) {
		return BT_GATT_ITER_CONTINUE;
	}

	bas_value_handle = chrc->value_handle;
	atomic_inc(&bas_char_ok);

	/* 先读一次：电量在连上后不一定马上变化，光订阅可能一直等不到通知 */
	memset(&bas_read, 0, sizeof(bas_read));
	bas_read.func = on_bas_read;
	bas_read.handle_count = 1;
	bas_read.single.handle = bas_value_handle;
	bas_read.single.offset = 0;
	(void)bt_gatt_read(c, &bas_read);

	if (chrc->properties & BT_GATT_CHRC_NOTIFY) {
		bas_sub.notify = on_bas_notify;
		bas_sub.subscribe = on_bas_subscribed;
		bas_sub.value_handle = bas_value_handle;
		bas_sub.ccc_handle = bas_value_handle + 1;
		bas_sub.value = BT_GATT_CCC_NOTIFY;
		atomic_set_bit(bas_sub.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

		int err = bt_gatt_subscribe(c, &bas_sub);

		if (err && err != -EALREADY) {
			printk("ble: bas subscribe failed %d\n", err);
		}
	}

	printk("ble: BAS level handle %u\n", bas_value_handle);
	return BT_GATT_ITER_STOP;
}

static uint8_t on_bas_primary(struct bt_conn *c, const struct bt_gatt_attr *attr,
			      struct bt_gatt_discover_params *params)
{
	if (!attr) {
		printk("ble: no BAS service\n");
		return BT_GATT_ITER_STOP;
	}

	const struct bt_gatt_service_val *svc = attr->user_data;

	atomic_inc(&bas_found);

	/* 与 HID 那一路保持一致：**不带 UUID 过滤**，列出该服务下所有特征，
	 * 再在回调里比对 UUID。实测带 uuid 过滤的特征发现会静默失败
	 * （bt_gatt_discover 直接返回错误、回调一次都没进来）。 */
	params->uuid = NULL;
	params->start_handle = attr->handle + 1;
	params->end_handle = svc->end_handle;
	params->type = BT_GATT_DISCOVER_CHARACTERISTIC;
	params->func = on_bas_char;

	int err = bt_gatt_discover(c, params);

	if (err) {
		printk("ble: bas char discover failed %d\n", err);
	}

	return BT_GATT_ITER_STOP;
}

static void bas_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);

	if (!conn) {
		return;
	}

	/* 已经读到电量就不用再折腾；最多试 8 次（每次间隔 3 s） */
	if (atomic_get(&batt_pct) >= 0 || bas_tries >= 8) {
		return;
	}
	bas_tries++;

	memset(&bas_disc, 0, sizeof(bas_disc));
	bas_disc.uuid = BT_UUID_BAS;
	bas_disc.start_handle = 0x0001;
	bas_disc.end_handle = 0xffff;
	bas_disc.type = BT_GATT_DISCOVER_PRIMARY;
	bas_disc.func = on_bas_primary;

	int err = bt_gatt_discover(conn, &bas_disc);

	if (err) {
		atomic_inc(&bas_read_err);
		printk("ble: bas discover failed %d\n", err);
	} else {
		atomic_inc(&bas_disc_ok);
	}

	/* 本轮没拿到值就 3 s 后再来一次（ATT 请求可能正忙） */
	(void)k_work_reschedule(&bas_work, K_MSEC(3000));
}

static void connected(struct bt_conn *conn_, uint8_t err)
{
	if (err) {
		printk("ble: connect failed 0x%02x\n", err);
		if (conn) {
			bt_conn_unref(conn);
			conn = NULL;
		}
		schedule_scan_retry(300);
		return;
	}

	/* bt_conn_le_create() 返回的对象引用已经由我们持有（存在 conn 里），
	 * 这里不能再 ref，否则 disconnected() 只 unref 一次会漏掉一个引用，
	 * 连接对象会卡在 disconnected 状态，之后 bt_conn_le_create() 一直 -EINVAL。 */
	if (!conn) {
		conn = bt_conn_ref(conn_);
	}
	/* 新连接：允许重新发现 / 重新订阅 */
	disc_started = false;
	subs_started = false;
	bas_tries = 0;
	printk("ble: connected\n");
	printk("ble: requesting L2 security\n");
	(void)bt_conn_set_security(conn, BT_SECURITY_L2);
}

static void disconnected(struct bt_conn *conn_, uint8_t reason)
{
	ARG_UNUSED(conn_);
	atomic_inc(&disc_evt);
	printk("ble: disconnected 0x%02x\n", reason);

	if (conn) {
		bt_conn_unref(conn);
		conn = NULL;
	}
	subscribed = false;
	/* 断链后电量未知：红灯保持灭，避免显示过期数值 */
	atomic_set(&batt_pct, -1);
	(void)k_work_cancel_delayable(&bas_work);
	/* 稍等再扫：等协议栈释放完上一个连接对象 */
	schedule_scan_retry(300);
}

static void security_changed(struct bt_conn *conn_, bt_security_t level, enum bt_security_err err)
{
	if (err) {
		printk("ble: security failed %d -> unpair, retry fresh\n", err);
		atomic_inc(&sec_err);
		/* 配对信息对不上（例如鼠标被重烧/被别的主机重新配对过）时，
		 * 清掉本端这条过期 bond 再重连，下次走全新配对，避免永远卡在加密阶段。 */
		(void)bt_unpair(BT_ID_DEFAULT, bt_conn_get_dst(conn_));
		(void)bt_conn_disconnect(conn_, BT_HCI_ERR_AUTH_FAIL);
		return;
	}
	printk("ble: security level %d\n", level);

	if (level >= BT_SECURITY_L2 && !subscribed) {
		atomic_inc(&sec_ok);
		/* 加密完成后先发现/订阅 HID；LLPM 1 ms 延后 500 ms 再切，
		 * 避免和鼠标自己刚发的 7.5 ms 参数更新撞车（会得到 0x11）。 */
		(void)k_work_reschedule(&llpm_work, K_MSEC(500));
		start_discovery();
	}
}

BT_CONN_CB_DEFINE(ctr_conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

static bool name_found;

static bool adv_name_cb(struct bt_data *data, void *user)
{
	ARG_UNUSED(user);

	if (data->type != BT_DATA_NAME_COMPLETE && data->type != BT_DATA_NAME_SHORTENED) {
		return true;
	}

	const size_t want = sizeof(BLE_CTR_TARGET_NAME) - 1;

	if (data->data_len == want &&
	    memcmp(data->data, BLE_CTR_TARGET_NAME, want) == 0) {
		name_found = true;
		return false;
	}
	return true;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
		    struct net_buf_simple *buf)
{
	ARG_UNUSED(rssi);
	ARG_UNUSED(adv_type);

	if (name_found) {
		return;
	}

	/* 名字可能出现在广播包（ADV_IND）或扫描响应（SCAN_RSP）里，
	 * 两处都要检查；能回扫描响应的设备都是可连接的。 */
	name_found = false;
	bt_data_parse(buf, adv_name_cb, NULL);

	if (!name_found) {
		return;
	}

	printk("ble: found \"%s\", connecting\n", BLE_CTR_TARGET_NAME);
	(void)bt_le_scan_stop();

	int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
				    BT_LE_CONN_PARAM(6, 6, 0, 400), &conn);

	if (err) {
		printk("ble: create conn failed %d\n", err);
		if (conn) {
			bt_conn_unref(conn);
			conn = NULL;
		}
		schedule_scan_retry(300);
	}
}

static void start_scan(void)
{
	static const struct bt_le_scan_param param = {
		/* 主动扫描：鼠标的设备名很可能只在 scan response 里 */
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};

	name_found = false;
	int err = bt_le_scan_start(&param, scan_cb);

	if (err) {
		printk("ble: scan start failed %d\n", err);
	}
}

void ble_ctr_stats(uint32_t *total, uint32_t *err)
{
	*total = (uint32_t)atomic_get(&rx_total);
	*err = (uint32_t)atomic_get(&rx_err);
}

uint32_t ble_ctr_btn_count(void)
{
	return (uint32_t)atomic_get(&btn_cnt);
}

int64_t ble_ctr_last_ms(void)
{
	return (int64_t)atomic_get(&last_rx_ms);
}

bool ble_ctr_ready(void)
{
	return subscribed;
}

int ble_ctr_battery_level(void)
{
	return (int)atomic_get(&batt_pct);
}

void ble_ctr_diag(uint32_t *sec_ok_, uint32_t *sec_err_, uint32_t *disc_ok_,
		  uint32_t *sub_ok_, uint32_t *disc_evt_)
{
	*sec_ok_ = (uint32_t)atomic_get(&sec_ok);
	*sec_err_ = (uint32_t)atomic_get(&sec_err);
	*disc_ok_ = (uint32_t)atomic_get(&disc_ok);
	*sub_ok_ = (uint32_t)atomic_get(&sub_ok);
	*disc_evt_ = (uint32_t)atomic_get(&disc_evt);
}

void ble_ctr_bas_diag(uint32_t *disc_ok_, uint32_t *found_, uint32_t *chr_,
		      uint32_t *rd_ok_, uint32_t *rd_err_)
{
	*disc_ok_ = (uint32_t)atomic_get(&bas_disc_ok);
	*found_ = (uint32_t)atomic_get(&bas_found);
	*chr_ = (uint32_t)atomic_get(&bas_char_ok);
	*rd_ok_ = (uint32_t)atomic_get(&bas_read_ok);
	*rd_err_ = (uint32_t)atomic_get(&bas_read_err);
}

int ble_ctr_cfg_send(const uint8_t *data, uint8_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);

	/* 鼠标端 BLE 侧还没有配置特征（只有 HID + BAS），
	 * 等它加上厂商配置服务后，这里改成 bt_gatt_write_without_response。 */
	return -ENOTSUP;
}

static void bond_info_cb(const struct bt_bond_info *info, void *user_data)
{
	char addr[BT_ADDR_LE_STR_LEN];

	ARG_UNUSED(user_data);
	bt_addr_le_to_str(&info->addr, addr, sizeof(addr));
	printk("ble: stored bond %s\n", addr);
}

int ble_ctr_init(void)
{
	int err = bt_enable(NULL);

	if (err) {
		printk("ble: bt_enable failed %d\n", err);
		return err;
	}

	/* 载入已保存的配对信息（LTK/IRK）。必须放在 bt_enable 之后。 */
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		err = settings_load();
		if (err) {
			printk("ble: settings_load failed %d\n", err);
		}
		bt_foreach_bond(BT_ID_DEFAULT, bond_info_cb, NULL);
	}

	/* LLPM 模式必须在建链前打开 */
	(void)llpm_mode_enable();

	printk("ble: central up, scanning for \"%s\"\n", BLE_CTR_TARGET_NAME);
	start_scan();

	return 0;
}
