/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple RTKit IPC library
 * Copyright (C) The Asahi Linux Contributors
 */

#ifndef _APPLE_RTKIT_INTERAL_H
#define _APPLE_RTKIT_INTERAL_H

#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <clocksource/arm_arch_timer.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/soc/apple/mailbox.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/workqueue.h>

#define APPLE_RTKIT_APP_ENDPOINT_START 0x20
#define APPLE_RTKIT_MAX_ENDPOINTS 0x100

struct apple_rtkit {
	void *cookie;
	const struct apple_rtkit_ops *ops;
	struct device *dev;

	struct apple_mbox *mbox;

	struct completion epmap_completion;
	struct completion iop_pwr_ack_completion;
	struct completion ap_pwr_ack_completion;

	int boot_result;
	int version;

	unsigned int iop_power_state;
	unsigned int ap_power_state;
	bool boot_ap_power;
	bool early_crashlog;
	struct completion crashlog_completion;
	bool crashed;
	/*
	 * RTBuddyManagementEndpoint +0x121 (K:b70231c): set at the first roll
	 * call's last part, cleared only by reinit. Every later roll call (a
	 * firmware restarted from DRAM after S2R) is answered with the AP's
	 * enabled endpoints and starts nothing.
	 */
	bool rollcall_done;
	/* mailbox receive interrupt off after an IOP idle, until the next RUN */
	bool irq_off;
	/* messages handled, for the timeout re-check (checkForWork) */
	unsigned long rx_handled;

	/* RTBuddy ping watchdog (ADT watchdog-enable, DT apple,rtkit-watchdog) */
	bool watchdog;
	bool watchdog_armed;
	struct delayed_work ping_work;
	u32 ping_sent;
	u32 ping_acked;
	unsigned long ping_since;
	unsigned int ping_send_fails;
	struct completion ping_ack_completion;

	DECLARE_BITMAP(endpoints, APPLE_RTKIT_MAX_ENDPOINTS);
	/* endpoints the AP started: macOS answers a later roll call with its enabled set */
	DECLARE_BITMAP(started, APPLE_RTKIT_MAX_ENDPOINTS);

	struct apple_rtkit_shmem ioreport_buffer;
	struct apple_rtkit_shmem system7_buffer;
	struct apple_rtkit_shmem crashlog_buffer;
	struct apple_rtkit_shmem oslog_buffer;

	struct apple_rtkit_shmem syslog_buffer;
	char *syslog_msg_buffer;
	size_t syslog_n_entries;
	size_t syslog_msg_size;

	struct workqueue_struct *wq;
};

void apple_rtkit_crashlog_dump(struct apple_rtkit *rtk, u8 *bfr, size_t size);

#endif
