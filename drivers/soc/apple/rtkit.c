// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple RTKit IPC library
 * Copyright (C) The Asahi Linux Contributors
 */

#include "rtkit-internal.h"

enum {
	APPLE_RTKIT_PWR_STATE_OFF = 0x00, /* power off, cannot be restarted */
	APPLE_RTKIT_PWR_STATE_SLEEP = 0x01, /* sleeping, can be restarted */
	APPLE_RTKIT_PWR_STATE_IDLE = 0x201, /* sleeping, retain state */
	APPLE_RTKIT_PWR_STATE_QUIESCED = 0x10, /* running but no communication */
	APPLE_RTKIT_PWR_STATE_ON = 0x20, /* normal operating state */
	APPLE_RTKIT_PWR_STATE_INIT = 0x220, /* init after starting the coproc */
};

enum {
	APPLE_RTKIT_EP_MGMT = 0,
	APPLE_RTKIT_EP_CRASHLOG = 1,
	APPLE_RTKIT_EP_SYSLOG = 2,
	APPLE_RTKIT_EP_DEBUG = 3,
	APPLE_RTKIT_EP_IOREPORT = 4,
	/* Advertised by newer firmware and required to complete RTKit boot. */
	APPLE_RTKIT_EP_UNKNOWN7 = 7,
	APPLE_RTKIT_EP_OSLOG = 8,
	APPLE_RTKIT_EP_TRACEKIT = 0xa,
};

/* RTBuddyManagementEndpoint::_messageHandler: ubfx #52, #4 (K:b7013c0) */
#define APPLE_RTKIT_MGMT_TYPE GENMASK_ULL(55, 52)

enum {
	APPLE_RTKIT_MGMT_HELLO = 1,
	APPLE_RTKIT_MGMT_HELLO_REPLY = 2,
	APPLE_RTKIT_MGMT_PING = 3,
	APPLE_RTKIT_MGMT_PING_ACK = 4,
	APPLE_RTKIT_MGMT_STARTEP = 5,
	APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE = 6,
	APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE_ACK = 7,
	APPLE_RTKIT_MGMT_EPMAP = 8,
	APPLE_RTKIT_MGMT_EPMAP_REPLY = 8,
	APPLE_RTKIT_MGMT_SET_AP_PWR_STATE = 0xb,
	APPLE_RTKIT_MGMT_SET_AP_PWR_STATE_ACK = 0xb,
	/* idle-msg (_handlePowerIdleGated); no J713 nub has idle-msg */
	APPLE_RTKIT_MGMT_POWER_IDLE = 0xc,
};

/* RTBuddy: timeout = the nub's "timeout" (s), 20 by default; no J713 nub has one */
#define APPLE_RTKIT_TIMEOUT_MS 20000
/* ping period = timeout * 1000 / rtb_ping_rate (10) (K:b6f2854, K:b70124c) */
#define APPLE_RTKIT_PING_RATE 10
#define APPLE_RTKIT_PING_PERIOD_MS (APPLE_RTKIT_TIMEOUT_MS / APPLE_RTKIT_PING_RATE)
/* _handleHello: refused versions are answered with this and fail validation */
#define APPLE_RTKIT_MGMT_HELLO_BAD 0xffff0badULL

#define APPLE_RTKIT_MGMT_HELLO_MINVER GENMASK_ULL(15, 0)
#define APPLE_RTKIT_MGMT_HELLO_MAXVER GENMASK_ULL(31, 16)

#define APPLE_RTKIT_MGMT_EPMAP_LAST   BIT_ULL(51)
/* roll-call part, bits 37:32, echoed with bit 51 (K:b7022c0, K:b702334) */
#define APPLE_RTKIT_MGMT_EPMAP_BASE   GENMASK_ULL(37, 32)
#define APPLE_RTKIT_MGMT_EPMAP_BITMAP GENMASK_ULL(31, 0)


#define APPLE_RTKIT_MGMT_STARTEP_EP   GENMASK_ULL(39, 32)
#define APPLE_RTKIT_MGMT_STARTEP_FLAG BIT_ULL(1)

#define APPLE_RTKIT_MGMT_PWR_STATE GENMASK_ULL(15, 0)

#define APPLE_RTKIT_CRASHLOG_CRASH 1

#define APPLE_RTKIT_BUFFER_REQUEST	1
#define APPLE_RTKIT_BUFFER_REQUEST_SIZE GENMASK_ULL(51, 44)
#define APPLE_RTKIT_BUFFER_REQUEST_IOVA GENMASK_ULL(43, 0)

#define APPLE_RTKIT_SYSLOG_TYPE GENMASK_ULL(59, 52)

#define APPLE_RTKIT_SYSTEM7_BUFFER_REPLY 5

#define APPLE_RTKIT_SYSLOG_LOG 5

#define APPLE_RTKIT_SYSLOG_INIT	     8
#define APPLE_RTKIT_SYSLOG_N_ENTRIES GENMASK_ULL(7, 0)
#define APPLE_RTKIT_SYSLOG_MSG_SIZE  GENMASK_ULL(31, 24)

/*
 * RTBuddyOSLogEndpoint::_messageHandler (macOS 27.0, fffffe000b6d3dd4) takes
 * the message type from bits 59:56; bits 63:60 are the coalescer index.
 */
#define APPLE_RTKIT_OSLOG_TYPE GENMASK_ULL(59, 56)
#define APPLE_RTKIT_OSLOG_BUFFER_REQUEST 1
#define APPLE_RTKIT_OSLOG_FLUSH 2
#define APPLE_RTKIT_OSLOG_SOURCE_REGISTER 3
#define APPLE_RTKIT_OSLOG_SOURCE_UUID 4
#define APPLE_RTKIT_OSLOG_SOURCE_ROLE 5
#define APPLE_RTKIT_OSLOG_FLUSH_ACK_MASK 0xf0000000ffffffffULL
#define APPLE_RTKIT_OSLOG_SIZE GENMASK_ULL(55, 36)
#define APPLE_RTKIT_OSLOG_IOVA GENMASK_ULL(35, 0)

#define APPLE_RTKIT_MAX_SUPPORTED_VERSION 12

struct apple_rtkit_rx_work {
	struct apple_rtkit *rtk;
	u8 ep;
	u64 msg;
	struct work_struct work;
};

bool apple_rtkit_is_running(struct apple_rtkit *rtk)
{
	if (rtk->crashed)
		return false;
	if ((rtk->iop_power_state & 0xff) != APPLE_RTKIT_PWR_STATE_ON)
		return false;
	if ((rtk->ap_power_state & 0xff) != APPLE_RTKIT_PWR_STATE_ON)
		return false;
	return true;
}
EXPORT_SYMBOL_GPL(apple_rtkit_is_running);

bool apple_rtkit_is_crashed(struct apple_rtkit *rtk)
{
	return rtk->crashed;
}
EXPORT_SYMBOL_GPL(apple_rtkit_is_crashed);

static int apple_rtkit_management_send(struct apple_rtkit *rtk, u8 type,
					u64 msg)
{
	int ret;

	msg &= ~APPLE_RTKIT_MGMT_TYPE;
	msg |= FIELD_PREP(APPLE_RTKIT_MGMT_TYPE, type);
	ret = apple_rtkit_send_message(rtk, APPLE_RTKIT_EP_MGMT, msg, NULL, false);

	if (ret)
		dev_err(rtk->dev, "RTKit: Failed to send management message: %d\n", ret);

	return ret;
}

/*
 * RTBuddyManagementEndpoint::_handleHello (K:b702034): the firmware's range
 * must hold 12 (min <= 12 <= max); then the reply is 12/12, upper bits 0
 * (bit 33 = mgmt+0x120, only zeroed; bit 32 is 0 on production). Otherwise
 * the reply is 0xffff0bad and validation fails.
 */
static void apple_rtkit_management_rx_hello(struct apple_rtkit *rtk, u64 msg)
{
	u64 reply;

	int min_ver = FIELD_GET(APPLE_RTKIT_MGMT_HELLO_MINVER, msg);
	int max_ver = FIELD_GET(APPLE_RTKIT_MGMT_HELLO_MAXVER, msg);

	dev_dbg(rtk->dev, "RTKit: Min ver %d, max ver %d\n", min_ver, max_ver);

	if (min_ver > APPLE_RTKIT_MAX_SUPPORTED_VERSION ||
	    max_ver < APPLE_RTKIT_MAX_SUPPORTED_VERSION) {
		dev_err(rtk->dev, "RTKit: Firmware versions %d..%d exclude %d\n",
			min_ver, max_ver, APPLE_RTKIT_MAX_SUPPORTED_VERSION);
		apple_rtkit_management_send(rtk, APPLE_RTKIT_MGMT_HELLO_REPLY,
					    APPLE_RTKIT_MGMT_HELLO_BAD);
		rtk->boot_result = -EINVAL;
		complete_all(&rtk->epmap_completion);
		complete_all(&rtk->iop_pwr_ack_completion);
		return;
	}

	dev_info(rtk->dev, "RTKit: Initializing (protocol version %d)\n",
		 APPLE_RTKIT_MAX_SUPPORTED_VERSION);
	rtk->version = APPLE_RTKIT_MAX_SUPPORTED_VERSION;

	reply = FIELD_PREP(APPLE_RTKIT_MGMT_HELLO_MINVER, APPLE_RTKIT_MAX_SUPPORTED_VERSION);
	reply |= FIELD_PREP(APPLE_RTKIT_MGMT_HELLO_MAXVER, APPLE_RTKIT_MAX_SUPPORTED_VERSION);
	apple_rtkit_management_send(rtk, APPLE_RTKIT_MGMT_HELLO_REPLY, reply);
}

/*
 * RTBuddyManagementEndpoint::_handleEPRollCall (K:b702288). The reply echoes
 * the part (bits 37:32) and the last bit, with the low word:
 * - first roll call: (part == 0) (K:b70230c-b702310); its last part sets
 *   rollcall_done (mgmt+0x121, K:b70231c), cleared only by reinit;
 * - every later one (a firmware restarted from DRAM after S2R): the AP's
 *   existing and enabled endpoints in that range (_buildRollCall,
 *   K:b702708), management 0 and crashlog 1 among them since RTBuddy creates
 *   both at first start (K:b6eed58, K:b6eee08); nothing is started, STARTEP
 *   comes only from an endpoint enable (K:b701dac).
 */
static void apple_rtkit_management_rx_epmap(struct apple_rtkit *rtk, u64 msg)
{
	int i, ep;
	u64 reply;
	unsigned long bitmap = FIELD_GET(APPLE_RTKIT_MGMT_EPMAP_BITMAP, msg);
	u32 base = FIELD_GET(APPLE_RTKIT_MGMT_EPMAP_BASE, msg);
	bool later = rtk->rollcall_done;

	dev_dbg(rtk->dev,
		"RTKit: received endpoint bitmap 0x%lx with base 0x%x\n",
		bitmap, base);

	for_each_set_bit(i, &bitmap, 32) {
		ep = 32 * base + i;
		dev_dbg(rtk->dev, "RTKit: Discovered endpoint 0x%02x\n", ep);
		set_bit(ep, rtk->endpoints);
		if (!later && rtk->early_crashlog && ep == APPLE_RTKIT_EP_CRASHLOG)
			apple_rtkit_start_ep(rtk, ep);
	}

	reply = msg & (APPLE_RTKIT_MGMT_EPMAP_BASE | APPLE_RTKIT_MGMT_EPMAP_LAST);
	if (!later) {
		reply |= base == 0;
	} else {
		reply |= bitmap_read(rtk->started, 32 * base, 32);
		if (base == 0)
			reply |= BIT_ULL(APPLE_RTKIT_EP_MGMT) | BIT_ULL(APPLE_RTKIT_EP_CRASHLOG);
	}

	apple_rtkit_management_send(rtk, APPLE_RTKIT_MGMT_EPMAP_REPLY, reply);

	if (!(msg & APPLE_RTKIT_MGMT_EPMAP_LAST))
		return;

	if (later) {
		rtk->boot_result = 0;
		complete_all(&rtk->epmap_completion);
		return;
	}
	rtk->rollcall_done = true;

	for_each_set_bit(ep, rtk->endpoints, APPLE_RTKIT_APP_ENDPOINT_START) {
		if (rtk->early_crashlog && ep == APPLE_RTKIT_EP_CRASHLOG)
			continue;
		switch (ep) {
		/* the management endpoint is started by default */
		case APPLE_RTKIT_EP_MGMT:
			break;

		/* without starting these RTKit refuses to boot */
		case APPLE_RTKIT_EP_SYSLOG:
		case APPLE_RTKIT_EP_CRASHLOG:
		case APPLE_RTKIT_EP_DEBUG:
		case APPLE_RTKIT_EP_IOREPORT:
		case APPLE_RTKIT_EP_UNKNOWN7:
		case APPLE_RTKIT_EP_OSLOG:
		case APPLE_RTKIT_EP_TRACEKIT:
			dev_dbg(rtk->dev,
				"RTKit: Starting system endpoint 0x%02x\n", ep);
			apple_rtkit_start_ep(rtk, ep);
			break;

		default:
			dev_warn(rtk->dev,
				 "RTKit: Unknown system endpoint: 0x%02x\n",
				 ep);
		}
	}

	rtk->boot_result = 0;
	complete_all(&rtk->epmap_completion);
}

/*
 * _handlePowerAck (K:b7024d0) decodes the low byte: 1 or 2 is idle, 0 off,
 * 0x20 on; the waiters check it.
 */
static void apple_rtkit_management_rx_iop_pwr_ack(struct apple_rtkit *rtk,
						  u64 msg)
{
	unsigned int new_state = FIELD_GET(APPLE_RTKIT_MGMT_PWR_STATE, msg);

	dev_dbg(rtk->dev, "RTKit: IOP power state transition: 0x%x -> 0x%x\n",
		rtk->iop_power_state, new_state);
	rtk->iop_power_state = new_state;

	complete_all(&rtk->iop_pwr_ack_completion);
}

/* the AP state echo: low 32 bits (mgmt+0x108, K:b701488) */
static void apple_rtkit_management_rx_ap_pwr_ack(struct apple_rtkit *rtk,
						 u64 msg)
{
	unsigned int new_state = lower_32_bits(msg);

	dev_dbg(rtk->dev, "RTKit: AP power state transition: 0x%x -> 0x%x\n",
		rtk->ap_power_state, new_state);
	rtk->ap_power_state = new_state;

	complete_all(&rtk->ap_pwr_ack_completion);
}

/* ping acknowledge (K:b7023d0): the low 32 bits echo the next counter */
static void apple_rtkit_management_rx_ping_ack(struct apple_rtkit *rtk, u64 msg)
{
	if (lower_32_bits(msg) != rtk->ping_acked)
		panic("%s: RTKit: ping ack %#x, expected %#x\n", dev_name(rtk->dev),
		      lower_32_bits(msg), rtk->ping_acked);
	rtk->ping_acked++;
	complete_all(&rtk->ping_ack_completion);
}

static void apple_rtkit_management_rx(struct apple_rtkit *rtk, u64 msg)
{
	u8 type = FIELD_GET(APPLE_RTKIT_MGMT_TYPE, msg);

	switch (type) {
	case APPLE_RTKIT_MGMT_HELLO:
		apple_rtkit_management_rx_hello(rtk, msg);
		break;
	case APPLE_RTKIT_MGMT_PING_ACK:
		apple_rtkit_management_rx_ping_ack(rtk, msg);
		break;
	case APPLE_RTKIT_MGMT_EPMAP:
		apple_rtkit_management_rx_epmap(rtk, msg);
		break;
	case APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE_ACK:
		apple_rtkit_management_rx_iop_pwr_ack(rtk, msg);
		break;
	case APPLE_RTKIT_MGMT_SET_AP_PWR_STATE_ACK:
		apple_rtkit_management_rx_ap_pwr_ack(rtk, msg);
		break;
	case APPLE_RTKIT_MGMT_POWER_IDLE:
		/* _handlePowerIdleGated: only nubs with idle-msg, none on J713 */
		break;
	default:
		/* any other type panics (_messageHandler.cold.1, K:b701554) */
		panic("%s: RTKit: unknown management message %#llx\n",
		      dev_name(rtk->dev), msg);
	}
}

/*
 * A repeated request (iova 0) for a buffer that is kept, large enough:
 * RTBuddy keeps the buffers across power state 0 (powerStatePreservesMemory
 * = state < 3, K:b6f7028) and answers with the same address (syslog
 * K:b707cb4-b707e68, IOReport K:b6f8474). Returns true when answered.
 */
static bool apple_rtkit_reuse_buffer(struct apple_rtkit *rtk,
				     struct apple_rtkit_shmem *buffer, u8 ep,
				     u64 msg)
{
	size_t size = FIELD_GET(APPLE_RTKIT_BUFFER_REQUEST_SIZE, msg) << 12;
	u64 reply;

	if (!buffer->size || FIELD_GET(APPLE_RTKIT_BUFFER_REQUEST_IOVA, msg) ||
	    size > buffer->size)
		return false;

	reply = FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, APPLE_RTKIT_BUFFER_REQUEST);
	reply |= FIELD_PREP(APPLE_RTKIT_BUFFER_REQUEST_SIZE, buffer->size >> 12);
	reply |= FIELD_PREP(APPLE_RTKIT_BUFFER_REQUEST_IOVA, buffer->iova);
	apple_rtkit_send_message(rtk, ep, reply, NULL, false);
	return true;
}

static int apple_rtkit_common_rx_get_buffer(struct apple_rtkit *rtk,
					    struct apple_rtkit_shmem *buffer,
					    u8 ep, u64 msg)
{
	u64 reply;
	int err;

	/* The different size vs. IOVA shifts look odd but are indeed correct this way */
	if (ep == APPLE_RTKIT_EP_OSLOG) {
		buffer->size = FIELD_GET(APPLE_RTKIT_OSLOG_SIZE, msg);
		buffer->iova = FIELD_GET(APPLE_RTKIT_OSLOG_IOVA, msg) << 12;
	} else {
		buffer->size = FIELD_GET(APPLE_RTKIT_BUFFER_REQUEST_SIZE, msg) << 12;
		buffer->iova = FIELD_GET(APPLE_RTKIT_BUFFER_REQUEST_IOVA, msg);
	}

	buffer->buffer = NULL;
	buffer->iomem = NULL;
	buffer->is_mapped = false;

	dev_dbg(rtk->dev, "RTKit: buffer request for 0x%zx bytes at %pad\n",
		buffer->size, &buffer->iova);

	if (buffer->iova && !rtk->ops->shmem_setup) {
		err = -EINVAL;
		goto error;
	}

	if (rtk->ops->shmem_setup) {
		err = rtk->ops->shmem_setup(rtk->cookie, buffer);
		if (err)
			goto error;
	} else {
		buffer->buffer = dma_alloc_coherent(rtk->dev, buffer->size,
						    &buffer->iova, GFP_KERNEL);
		if (!buffer->buffer) {
			err = -ENOMEM;
			goto error;
		}
	}

	if (!buffer->is_mapped) {
		/* oslog uses different fields and needs a shifted IOVA instead of size */
		if (ep == APPLE_RTKIT_EP_OSLOG) {
			reply = FIELD_PREP(APPLE_RTKIT_OSLOG_TYPE,
					   APPLE_RTKIT_OSLOG_BUFFER_REQUEST);
			reply |= FIELD_PREP(APPLE_RTKIT_OSLOG_SIZE, buffer->size);
			reply |= FIELD_PREP(APPLE_RTKIT_OSLOG_IOVA,
					    buffer->iova >> 12);
		} else {
			reply = FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE,
					   ep == APPLE_RTKIT_EP_UNKNOWN7 ?
					   APPLE_RTKIT_SYSTEM7_BUFFER_REPLY :
					   APPLE_RTKIT_BUFFER_REQUEST);
			reply |= FIELD_PREP(APPLE_RTKIT_BUFFER_REQUEST_SIZE,
					    buffer->size >> 12);
			reply |= FIELD_PREP(APPLE_RTKIT_BUFFER_REQUEST_IOVA,
					    buffer->iova);
		}
		apple_rtkit_send_message(rtk, ep, reply, NULL, false);
	} else if (rtk->early_crashlog && ep == APPLE_RTKIT_EP_CRASHLOG) {
		apple_rtkit_send_message(rtk, ep, msg, NULL, false);
	}

	return 0;

error:
	dev_err(rtk->dev, "RTKit: failed buffer request for 0x%zx bytes (%d)\n",
		buffer->size, err);

	buffer->buffer = NULL;
	buffer->iomem = NULL;
	buffer->iova = 0;
	buffer->size = 0;
	buffer->is_mapped = false;
	return err;
}

static void apple_rtkit_free_buffer(struct apple_rtkit *rtk,
				    struct apple_rtkit_shmem *bfr)
{
	if (bfr->size == 0)
		return;

	if (rtk->ops->shmem_destroy)
		rtk->ops->shmem_destroy(rtk->cookie, bfr);
	else if (bfr->buffer)
		dma_free_coherent(rtk->dev, bfr->size, bfr->buffer, bfr->iova);

	bfr->buffer = NULL;
	bfr->iomem = NULL;
	bfr->iova = 0;
	bfr->size = 0;
	bfr->is_mapped = false;
}

static void apple_rtkit_memcpy(struct apple_rtkit *rtk, void *dst,
			       struct apple_rtkit_shmem *bfr, size_t offset,
			       size_t len)
{
	if (bfr->iomem)
		memcpy_fromio(dst, bfr->iomem + offset, len);
	else
		memcpy(dst, bfr->buffer + offset, len);
}

static void apple_rtkit_crashlog_rx(struct apple_rtkit *rtk, u64 msg)
{
	u8 type = FIELD_GET(APPLE_RTKIT_SYSLOG_TYPE, msg);
	u8 *bfr __free(kfree) = NULL;

	if (type != APPLE_RTKIT_CRASHLOG_CRASH) {
		dev_warn(rtk->dev, "RTKit: Unknown crashlog message: %llx\n",
			 msg);
		return;
	}

	if (!rtk->crashlog_buffer.size) {
		if (!apple_rtkit_common_rx_get_buffer(rtk, &rtk->crashlog_buffer,
						 APPLE_RTKIT_EP_CRASHLOG, msg))
			complete_all(&rtk->crashlog_completion);
		return;
	}

	dev_err(rtk->dev, "RTKit: co-processor has crashed\n");

	/*
	 * create a shadow copy here to make sure the co-processor isn't able
	 * to change the log while we're dumping it. this also ensures
	 * the buffer is in normal memory and not iomem for e.g. the SMC
	 */
	bfr = kzalloc(rtk->crashlog_buffer.size, GFP_KERNEL);
	if (bfr) {
		apple_rtkit_memcpy(rtk, bfr, &rtk->crashlog_buffer, 0,
				   rtk->crashlog_buffer.size);
		apple_rtkit_crashlog_dump(rtk, bfr, rtk->crashlog_buffer.size);
	} else {
		dev_err(rtk->dev,
			"RTKit: Couldn't allocate crashlog shadow buffer\n");
	}

	rtk->crashed = true;
	if (rtk->ops->crashed)
		rtk->ops->crashed(rtk->cookie, bfr, bfr ? rtk->crashlog_buffer.size : 0);
}

static void apple_rtkit_ioreport_rx(struct apple_rtkit *rtk, u64 msg)
{
	u8 type = FIELD_GET(APPLE_RTKIT_SYSLOG_TYPE, msg);

	switch (type) {
	case APPLE_RTKIT_BUFFER_REQUEST:
		if (apple_rtkit_reuse_buffer(rtk, &rtk->ioreport_buffer, APPLE_RTKIT_EP_IOREPORT, msg))
			break;
		apple_rtkit_common_rx_get_buffer(rtk, &rtk->ioreport_buffer,
						 APPLE_RTKIT_EP_IOREPORT, msg);
		break;
	/*
	 * 0x8: _updateRemoteReportersGated, then a bare type-8 reply; 0xc: a
	 * flag (+0xba), no reply (K:b6f82dc)
	 */
	case 0x8:
		apple_rtkit_send_message(rtk, APPLE_RTKIT_EP_IOREPORT,
					 FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, 0x8), NULL, false);
		break;
	case 0xc:
		break;
	default:
		dev_warn(rtk->dev, "RTKit: Unknown ioreport message: %llx\n",
			 msg);
	}
}

static void apple_rtkit_system7_rx(struct apple_rtkit *rtk, u64 msg)
{
	u8 type = FIELD_GET(APPLE_RTKIT_SYSLOG_TYPE, msg);

	switch (type) {
	case APPLE_RTKIT_BUFFER_REQUEST:
		apple_rtkit_common_rx_get_buffer(rtk, &rtk->system7_buffer,
						 APPLE_RTKIT_EP_UNKNOWN7, msg);
		break;
	default:
		dev_warn(rtk->dev, "RTKit: Unknown system endpoint 7 message: %llx\n",
			 msg);
	}
}

static void apple_rtkit_syslog_rx_init(struct apple_rtkit *rtk, u64 msg)
{
	rtk->syslog_n_entries = FIELD_GET(APPLE_RTKIT_SYSLOG_N_ENTRIES, msg);
	rtk->syslog_msg_size = FIELD_GET(APPLE_RTKIT_SYSLOG_MSG_SIZE, msg);

	rtk->syslog_msg_buffer = kzalloc(rtk->syslog_msg_size, GFP_KERNEL);

	dev_dbg(rtk->dev,
		"RTKit: syslog initialized: entries: %zd, msg_size: %zd\n",
		rtk->syslog_n_entries, rtk->syslog_msg_size);
}

static bool should_crop_syslog_char(char c)
{
	return c == '\n' || c == '\r' || c == ' ' || c == '\0';
}

static void apple_rtkit_syslog_rx_log(struct apple_rtkit *rtk, u64 msg)
{
	u8 idx = msg & 0xff;
	char log_context[24];
	size_t entry_size = 0x20 + rtk->syslog_msg_size;
	int msglen;

	if (!rtk->syslog_msg_buffer) {
		dev_warn(
			rtk->dev,
			"RTKit: received syslog message but no syslog_msg_buffer\n");
		goto done;
	}
	if (!rtk->syslog_buffer.size) {
		dev_warn(
			rtk->dev,
			"RTKit: received syslog message but syslog_buffer.size is zero\n");
		goto done;
	}
	if (!rtk->syslog_buffer.buffer && !rtk->syslog_buffer.iomem) {
		dev_warn(
			rtk->dev,
			"RTKit: received syslog message but no syslog_buffer.buffer or syslog_buffer.iomem\n");
		goto done;
	}
	if (idx > rtk->syslog_n_entries) {
		dev_warn(rtk->dev, "RTKit: syslog index %d out of range\n",
			 idx);
		goto done;
	}

	apple_rtkit_memcpy(rtk, log_context, &rtk->syslog_buffer,
			   idx * entry_size + 8, sizeof(log_context));
	apple_rtkit_memcpy(rtk, rtk->syslog_msg_buffer, &rtk->syslog_buffer,
			   idx * entry_size + 8 + sizeof(log_context),
			   rtk->syslog_msg_size);

	log_context[sizeof(log_context) - 1] = 0;

	msglen = strnlen(rtk->syslog_msg_buffer, rtk->syslog_msg_size - 1);
	while (msglen > 0 &&
		   should_crop_syslog_char(rtk->syslog_msg_buffer[msglen - 1]))
		msglen--;

	rtk->syslog_msg_buffer[msglen] = 0;
	dev_info(rtk->dev, "RTKit: syslog message: %s: %s\n", log_context,
		 rtk->syslog_msg_buffer);

done:
	/* the log ack is type 5 with the low 32 bits only (K:b707950) */
	apple_rtkit_send_message(rtk, APPLE_RTKIT_EP_SYSLOG,
				 FIELD_PREP(APPLE_RTKIT_SYSLOG_TYPE, APPLE_RTKIT_SYSLOG_LOG) |
				 lower_32_bits(msg), NULL, false);
}

static void apple_rtkit_syslog_rx(struct apple_rtkit *rtk, u64 msg)
{
	u8 type = FIELD_GET(APPLE_RTKIT_SYSLOG_TYPE, msg);

	switch (type) {
	case APPLE_RTKIT_BUFFER_REQUEST:
		if (apple_rtkit_reuse_buffer(rtk, &rtk->syslog_buffer, APPLE_RTKIT_EP_SYSLOG, msg))
			break;
		apple_rtkit_common_rx_get_buffer(rtk, &rtk->syslog_buffer,
						 APPLE_RTKIT_EP_SYSLOG, msg);
		break;
	case APPLE_RTKIT_SYSLOG_INIT:
		apple_rtkit_syslog_rx_init(rtk, msg);
		break;
	case APPLE_RTKIT_SYSLOG_LOG:
		apple_rtkit_syslog_rx_log(rtk, msg);
		break;
	default:
		dev_warn(rtk->dev, "RTKit: Unknown syslog message: %llx\n",
			 msg);
	}
}

static void apple_rtkit_oslog_rx(struct apple_rtkit *rtk, u64 msg)
{
	u8 type = FIELD_GET(APPLE_RTKIT_OSLOG_TYPE, msg);

	switch (type) {
	case APPLE_RTKIT_OSLOG_BUFFER_REQUEST:
		apple_rtkit_common_rx_get_buffer(rtk, &rtk->oslog_buffer,
						 APPLE_RTKIT_EP_OSLOG, msg);
		break;
	case APPLE_RTKIT_OSLOG_FLUSH:
		/*
		 * The firmware asks for its log entries to be consumed up to
		 * the position in the low 32 bits. macOS answers with the
		 * coalescer index and that position under the same type
		 * (_handleFlushRequest, fffffe000b6d4d34..b6d4d70). The entries
		 * themselves are not decoded here.
		 */
		apple_rtkit_send_message(rtk, APPLE_RTKIT_EP_OSLOG,
					 (msg & APPLE_RTKIT_OSLOG_FLUSH_ACK_MASK) |
					 FIELD_PREP(APPLE_RTKIT_OSLOG_TYPE,
						    APPLE_RTKIT_OSLOG_FLUSH),
					 NULL, false);
		break;
	case APPLE_RTKIT_OSLOG_SOURCE_REGISTER:
	case APPLE_RTKIT_OSLOG_SOURCE_UUID:
	case APPLE_RTKIT_OSLOG_SOURCE_ROLE:
		/* The firmware names the source of its log: a registration,
		 * the UUID in parts and the role string in parts. */
		break;
	default:
		dev_warn(rtk->dev, "RTKit: Unknown oslog message: %llx\n",
			 msg);
	}
}

static void apple_rtkit_rx_work(struct work_struct *work)
{
	struct apple_rtkit_rx_work *rtk_work =
		container_of(work, struct apple_rtkit_rx_work, work);
	struct apple_rtkit *rtk = rtk_work->rtk;

	switch (rtk_work->ep) {
	case APPLE_RTKIT_EP_MGMT:
		apple_rtkit_management_rx(rtk, rtk_work->msg);
		break;
	case APPLE_RTKIT_EP_CRASHLOG:
		apple_rtkit_crashlog_rx(rtk, rtk_work->msg);
		break;
	case APPLE_RTKIT_EP_SYSLOG:
		apple_rtkit_syslog_rx(rtk, rtk_work->msg);
		break;
	case APPLE_RTKIT_EP_IOREPORT:
		apple_rtkit_ioreport_rx(rtk, rtk_work->msg);
		break;
	case APPLE_RTKIT_EP_UNKNOWN7:
		apple_rtkit_system7_rx(rtk, rtk_work->msg);
		break;
	case APPLE_RTKIT_EP_OSLOG:
		apple_rtkit_oslog_rx(rtk, rtk_work->msg);
		break;
	case APPLE_RTKIT_APP_ENDPOINT_START ... 0xff:
		if (rtk->ops->recv_message)
			rtk->ops->recv_message(rtk->cookie, rtk_work->ep,
					       rtk_work->msg);
		else
			dev_warn(
				rtk->dev,
				"Received unexpected message to EP%02d: %llx\n",
				rtk_work->ep, rtk_work->msg);
		break;
	default:
		dev_warn(rtk->dev,
			 "RTKit: message to unknown endpoint %02x: %llx\n",
			 rtk_work->ep, rtk_work->msg);
	}

	WRITE_ONCE(rtk->rx_handled, rtk->rx_handled + 1);
	kfree(rtk_work);
}

static void apple_rtkit_rx(struct apple_mbox *mbox, struct apple_mbox_msg msg,
			   void *cookie)
{
	struct apple_rtkit *rtk = cookie;
	struct apple_rtkit_rx_work *work;
	u8 ep = msg.msg1;

	/*
	 * The message was read from a MMIO FIFO and we have to make
	 * sure all reads from buffers sent with that message happen
	 * afterwards.
	 */
	dma_rmb();

	/* every inbound message re-arms the ping timer (K:b6f0504) */
	if (READ_ONCE(rtk->watchdog_armed))
		mod_delayed_work(system_wq, &rtk->ping_work,
				 msecs_to_jiffies(APPLE_RTKIT_PING_PERIOD_MS));

	if (!test_bit(ep, rtk->endpoints))
		dev_warn(rtk->dev,
			 "RTKit: Message to undiscovered endpoint 0x%02x\n",
			 ep);

	if (ep >= APPLE_RTKIT_APP_ENDPOINT_START &&
	    rtk->ops->recv_message_early &&
	    rtk->ops->recv_message_early(rtk->cookie, ep, msg.msg0))
		return;

	work = kzalloc_obj(*work, GFP_ATOMIC);
	if (!work)
		return;

	work->rtk = rtk;
	work->ep = ep;
	work->msg = msg.msg0;
	INIT_WORK(&work->work, apple_rtkit_rx_work);
	queue_work(rtk->wq, &work->work);
}

int apple_rtkit_send_message(struct apple_rtkit *rtk, u8 ep, u64 message,
			     struct completion *completion, bool atomic)
{
	struct apple_mbox_msg msg = {
		.msg0 = message,
		.msg1 = ep,
	};

	if (rtk->crashed) {
		dev_warn(rtk->dev,
			 "RTKit: Device is crashed, cannot send message\n");
		return -EINVAL;
	}

	if (ep >= APPLE_RTKIT_APP_ENDPOINT_START &&
	    !apple_rtkit_is_running(rtk)) {
		dev_warn(rtk->dev,
			 "RTKit: Endpoint 0x%02x is not running, cannot send message\n", ep);
		return -EINVAL;
	}

	/*
	 * The message will be sent with a MMIO write. We need the barrier
	 * here to ensure any previous writes to buffers are visible to the
	 * device before that MMIO write happens.
	 */
	dma_wmb();

	return apple_mbox_send(rtk->mbox, msg, atomic);
}
EXPORT_SYMBOL_GPL(apple_rtkit_send_message);

int apple_rtkit_poll(struct apple_rtkit *rtk)
{
	return apple_mbox_poll(rtk->mbox);
}
EXPORT_SYMBOL_GPL(apple_rtkit_poll);

bool apple_rtkit_has_endpoint(struct apple_rtkit *rtk, u8 ep)
{
	return test_bit(ep, rtk->endpoints);
}
EXPORT_SYMBOL_GPL(apple_rtkit_has_endpoint);

int apple_rtkit_start_ep(struct apple_rtkit *rtk, u8 endpoint)
{
	u64 msg;

	if (!test_bit(endpoint, rtk->endpoints))
		return -EINVAL;
	if (endpoint >= APPLE_RTKIT_APP_ENDPOINT_START &&
	    !apple_rtkit_is_running(rtk))
		return -EINVAL;

	msg = FIELD_PREP(APPLE_RTKIT_MGMT_STARTEP_EP, endpoint);
	msg |= APPLE_RTKIT_MGMT_STARTEP_FLAG;
	apple_rtkit_management_send(rtk, APPLE_RTKIT_MGMT_STARTEP, msg);
	set_bit(endpoint, rtk->started);

	return 0;
}
EXPORT_SYMBOL_GPL(apple_rtkit_start_ep);

static void apple_rtkit_ping_work(struct work_struct *work);

struct apple_rtkit *apple_rtkit_init(struct device *dev, void *cookie,
					    const char *mbox_name, int mbox_idx,
					    const struct apple_rtkit_ops *ops)
{
	struct apple_rtkit *rtk;
	int ret;

	if (!ops)
		return ERR_PTR(-EINVAL);

	rtk = kzalloc_obj(*rtk);
	if (!rtk)
		return ERR_PTR(-ENOMEM);

	rtk->dev = dev;
	rtk->cookie = cookie;
	rtk->ops = ops;
	rtk->boot_ap_power = true;
	init_completion(&rtk->crashlog_completion);

	init_completion(&rtk->epmap_completion);
	init_completion(&rtk->iop_pwr_ack_completion);
	init_completion(&rtk->ap_pwr_ack_completion);
	init_completion(&rtk->ping_ack_completion);
	INIT_DELAYED_WORK(&rtk->ping_work, apple_rtkit_ping_work);
	/* ADT watchdog-enable on the RTBuddy nub */
	rtk->watchdog = of_property_read_bool(dev->of_node, "apple,rtkit-watchdog");

	bitmap_zero(rtk->endpoints, APPLE_RTKIT_MAX_ENDPOINTS);
	bitmap_zero(rtk->started, APPLE_RTKIT_MAX_ENDPOINTS);
	set_bit(APPLE_RTKIT_EP_MGMT, rtk->endpoints);

	if (mbox_name)
		rtk->mbox = apple_mbox_get_byname(dev, mbox_name);
	else
		rtk->mbox = apple_mbox_get(dev, mbox_idx);

	if (IS_ERR(rtk->mbox)) {
		ret = PTR_ERR(rtk->mbox);
		goto free_rtk;
	}

	rtk->mbox->rx = apple_rtkit_rx;
	rtk->mbox->cookie = rtk;

	rtk->wq = alloc_ordered_workqueue("rtkit-%s", WQ_HIGHPRI | WQ_MEM_RECLAIM,
					  dev_name(rtk->dev));
	if (!rtk->wq) {
		ret = -ENOMEM;
		goto free_rtk;
	}

	ret = apple_mbox_start(rtk->mbox);
	if (ret)
		goto destroy_wq;

	return rtk;

destroy_wq:
	destroy_workqueue(rtk->wq);
free_rtk:
	kfree(rtk);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(apple_rtkit_init);

/*
 * RTBuddy's waits (K:b6eeed8, K:b701f28, K:b6eff04): commandSleep for the
 * nub's timeout (20 s), not interruptible. On a timeout the management
 * endpoint's checkForWork (K:b05e204) handles a queued message, and if there
 * was one the full timeout starts again; otherwise the wait fails.
 */
static int apple_rtkit_wait_for_completion(struct apple_rtkit *rtk,
					   struct completion *c)
{
	unsigned long handled;

	for (;;) {
		if (wait_for_completion_timeout(c, msecs_to_jiffies(APPLE_RTKIT_TIMEOUT_MS)))
			return 0;
		handled = READ_ONCE(rtk->rx_handled);
		flush_workqueue(rtk->wq);
		if (completion_done(c))
			return 0;
		if (READ_ONCE(rtk->rx_handled) == handled)
			return -ETIME;
	}
}

/*
 * RTBuddy ping watchdog, nubs with watchdog-enable (AOP, MTP, SMC, DCP,
 * dcpext): after the ping period with no inbound message
 * (_watchdogSendPing, K:b702880-b7028c0) send type 3, the sequence and
 * (mach_absolute_time & 0xffff) << 32; each type 4 echoes the next counter.
 * An outstanding ping after a check for work for the whole timeout, or
 * rtb_ping_rate failed sends, is a crash (K:b70279c).
 */
static void apple_rtkit_ping_work(struct work_struct *work)
{
	struct apple_rtkit *rtk =
		container_of(to_delayed_work(work), struct apple_rtkit, ping_work);
	u64 msg;

	if (rtk->ping_sent != rtk->ping_acked) {
		flush_workqueue(rtk->wq);
		if (rtk->ping_sent != rtk->ping_acked &&
		    time_after(jiffies, rtk->ping_since + msecs_to_jiffies(APPLE_RTKIT_TIMEOUT_MS))) {
			dev_err(rtk->dev, "RTKit: ping timeout\n");
			rtk->crashed = true;
			return;
		}
	} else {
		msg = rtk->ping_sent;
		msg |= (arch_timer_read_counter() & 0xffff) << 32;
		if (apple_rtkit_management_send(rtk, APPLE_RTKIT_MGMT_PING, msg)) {
			if (++rtk->ping_send_fails >= APPLE_RTKIT_PING_RATE) {
				dev_err(rtk->dev, "RTKit: ping send failed\n");
				rtk->crashed = true;
				return;
			}
		} else {
			rtk->ping_sent++;
			rtk->ping_since = jiffies;
		}
	}
	if (READ_ONCE(rtk->watchdog_armed))
		mod_delayed_work(system_wq, &rtk->ping_work,
				 msecs_to_jiffies(APPLE_RTKIT_PING_PERIOD_MS));
}

/* _onPowerStateGated(1, 1) (K:b701868-b7018f0): counters reset, timer re-armed */
static void apple_rtkit_watchdog_arm(struct apple_rtkit *rtk)
{
	if (!rtk->watchdog)
		return;
	rtk->ping_sent = 0;
	rtk->ping_acked = 0;
	rtk->ping_send_fails = 0;
	WRITE_ONCE(rtk->watchdog_armed, true);
	mod_delayed_work(system_wq, &rtk->ping_work,
			 msecs_to_jiffies(APPLE_RTKIT_PING_PERIOD_MS));
}

/*
 * _quiesceWatchdogGated (K:b701ad4), after AP 0x201: wait for the
 * outstanding ping's ack, then cancel the timer.
 */
static int apple_rtkit_watchdog_quiesce(struct apple_rtkit *rtk)
{
	if (!rtk->watchdog || !rtk->watchdog_armed)
		return 0;
	WRITE_ONCE(rtk->watchdog_armed, false);
	cancel_delayed_work_sync(&rtk->ping_work);
	while (rtk->ping_sent != rtk->ping_acked) {
		reinit_completion(&rtk->ping_ack_completion);
		if (rtk->ping_sent == rtk->ping_acked)
			break;
		if (apple_rtkit_wait_for_completion(rtk, &rtk->ping_ack_completion)) {
			dev_err(rtk->dev, "RTKit: failed to quiesce watchdog\n");
			return -ETIME;
		}
	}
	return 0;
}


int apple_rtkit_reinit(struct apple_rtkit *rtk)
{
	if (rtk->irq_off) {
		rtk->irq_off = false;
		enable_irq(rtk->mbox->irq_recv_not_empty);
	}
	WRITE_ONCE(rtk->watchdog_armed, false);
	cancel_delayed_work_sync(&rtk->ping_work);
	/* make sure we don't handle any messages while reinitializing */
	apple_mbox_stop(rtk->mbox);
	flush_workqueue(rtk->wq);

	apple_rtkit_free_buffer(rtk, &rtk->ioreport_buffer);
	apple_rtkit_free_buffer(rtk, &rtk->system7_buffer);
	apple_rtkit_free_buffer(rtk, &rtk->crashlog_buffer);
	apple_rtkit_free_buffer(rtk, &rtk->oslog_buffer);
	apple_rtkit_free_buffer(rtk, &rtk->syslog_buffer);

	kfree(rtk->syslog_msg_buffer);

	rtk->syslog_msg_buffer = NULL;
	rtk->syslog_n_entries = 0;
	rtk->syslog_msg_size = 0;
	rtk->rollcall_done = false;

	bitmap_zero(rtk->endpoints, APPLE_RTKIT_MAX_ENDPOINTS);
	bitmap_zero(rtk->started, APPLE_RTKIT_MAX_ENDPOINTS);
	set_bit(APPLE_RTKIT_EP_MGMT, rtk->endpoints);

	reinit_completion(&rtk->epmap_completion);
	reinit_completion(&rtk->crashlog_completion);
	reinit_completion(&rtk->iop_pwr_ack_completion);
	reinit_completion(&rtk->ap_pwr_ack_completion);

	rtk->crashed = false;
	rtk->iop_power_state = APPLE_RTKIT_PWR_STATE_OFF;
	rtk->ap_power_state = APPLE_RTKIT_PWR_STATE_OFF;

	return apple_mbox_start(rtk->mbox);
}
EXPORT_SYMBOL_GPL(apple_rtkit_reinit);

/*
 * _signalApPowerGated (K:b7019cc): nothing is sent when the last echo (its
 * low 32 bits) already is the state; otherwise send and wait for the echo.
 * _onPowerStateGated: going to 0x20 the ping watchdog is re-armed first
 * (K:b701868-b7018f0); going to 0x201 it is quiesced after the echo
 * (K:b701ad4).
 */
static int apple_rtkit_set_ap_power_state(struct apple_rtkit *rtk,
					  unsigned int state)
{
	u64 msg;
	int ret;

	if (state == APPLE_RTKIT_PWR_STATE_ON)
		apple_rtkit_watchdog_arm(rtk);

	if (rtk->ap_power_state != state) {
		reinit_completion(&rtk->ap_pwr_ack_completion);

		msg = FIELD_PREP(APPLE_RTKIT_MGMT_PWR_STATE, state);
		ret = apple_rtkit_management_send(rtk, APPLE_RTKIT_MGMT_SET_AP_PWR_STATE,
						  msg);
		if (ret)
			return ret;

		ret = apple_rtkit_wait_for_completion(rtk, &rtk->ap_pwr_ack_completion);
		if (ret) {
			dev_err(rtk->dev, "RTKit: no response to AP power state %#x\n", state);
			return ret;
		}

		if (rtk->ap_power_state != state)
			return -EINVAL;
	}

	if (state == APPLE_RTKIT_PWR_STATE_IDLE)
		return apple_rtkit_watchdog_quiesce(rtk);
	return 0;
}

/*
 * RTBuddy action 3 (AOP, MTP, SMC on S2R, _signalApStateAllRoutes): the AP
 * power state alone, 0x201 for sleep and 0x20 for wake, with the ping
 * watchdog quiesced and re-armed.
 */
int apple_rtkit_set_ap_power(struct apple_rtkit *rtk, bool on)
{
	return apple_rtkit_set_ap_power_state(rtk, on ? APPLE_RTKIT_PWR_STATE_ON :
							APPLE_RTKIT_PWR_STATE_IDLE);
}
EXPORT_SYMBOL_GPL(apple_rtkit_set_ap_power);

void apple_rtkit_set_boot_ap_power(struct apple_rtkit *rtk, bool enable)
{
	rtk->boot_ap_power = enable;
}
EXPORT_SYMBOL_GPL(apple_rtkit_set_boot_ap_power);

void apple_rtkit_set_early_crashlog(struct apple_rtkit *rtk)
{
	rtk->early_crashlog = true;
	reinit_completion(&rtk->crashlog_completion);
}
EXPORT_SYMBOL_GPL(apple_rtkit_set_early_crashlog);

int apple_rtkit_adopt_running(struct apple_rtkit *rtk, const u8 *endpoints,
			      size_t n_endpoints)
{
	size_t i;

	if (!rtk || (!endpoints && n_endpoints) || rtk->crashed)
		return -EINVAL;

	bitmap_zero(rtk->endpoints, APPLE_RTKIT_MAX_ENDPOINTS);
	bitmap_zero(rtk->started, APPLE_RTKIT_MAX_ENDPOINTS);
	set_bit(APPLE_RTKIT_EP_MGMT, rtk->endpoints);
	for (i = 0; i < n_endpoints; i++)
		set_bit(endpoints[i], rtk->endpoints);

	/* The bootloader already negotiated the current protocol and left both
	 * halves of the link live. No HELLO, endpoint map, INIT, or AP-power
	 * message may be replayed into that session. */
	rtk->version = APPLE_RTKIT_MAX_SUPPORTED_VERSION;
	rtk->boot_result = 0;
	rtk->rollcall_done = true;
	rtk->iop_power_state = APPLE_RTKIT_PWR_STATE_ON;
	rtk->ap_power_state = APPLE_RTKIT_PWR_STATE_ON;
	complete_all(&rtk->epmap_completion);
	complete_all(&rtk->iop_pwr_ack_completion);
	complete_all(&rtk->ap_pwr_ack_completion);
	apple_rtkit_watchdog_arm(rtk);

	dev_info(rtk->dev, "RTKit: adopted running bootloader session\n");
	return 0;
}
EXPORT_SYMBOL_GPL(apple_rtkit_adopt_running);

int apple_rtkit_reuse_syslog_buffer(struct apple_rtkit *rtk, dma_addr_t iova,
				    size_t size, size_t n_entries,
				    size_t msg_size)
{
	int ret;

	if (!rtk || !iova || !size || !n_entries || !msg_size)
		return -EINVAL;
	if (rtk->syslog_buffer.size || rtk->syslog_msg_buffer)
		return -EBUSY;
	if (!rtk->ops->shmem_setup)
		return -EOPNOTSUPP;

	rtk->syslog_buffer.iova = iova;
	rtk->syslog_buffer.size = size;
	ret = rtk->ops->shmem_setup(rtk->cookie, &rtk->syslog_buffer);
	if (ret)
		goto clear_buffer;

	rtk->syslog_msg_buffer = kzalloc(msg_size, GFP_KERNEL);
	if (!rtk->syslog_msg_buffer) {
		ret = -ENOMEM;
		goto destroy_buffer;
	}

	rtk->syslog_n_entries = n_entries;
	rtk->syslog_msg_size = msg_size;
	dev_info(rtk->dev,
		 "RTKit: reused syslog buffer at %pad: entries: %zd, msg_size: %zd\n",
		 &rtk->syslog_buffer.iova, n_entries, msg_size);
	return 0;

destroy_buffer:
	rtk->ops->shmem_destroy(rtk->cookie, &rtk->syslog_buffer);
clear_buffer:
	rtk->syslog_buffer = (struct apple_rtkit_shmem){};
	return ret;
}
EXPORT_SYMBOL_GPL(apple_rtkit_reuse_syslog_buffer);

int apple_rtkit_reuse_crashlog_buffer(struct apple_rtkit *rtk,
				      dma_addr_t iova, size_t size)
{
	int ret;

	if (!rtk || !iova || !size)
		return -EINVAL;
	if (rtk->crashlog_buffer.size)
		return -EBUSY;
	if (!rtk->ops->shmem_setup)
		return -EOPNOTSUPP;

	rtk->crashlog_buffer.iova = iova;
	rtk->crashlog_buffer.size = size;
	ret = rtk->ops->shmem_setup(rtk->cookie, &rtk->crashlog_buffer);
	if (ret) {
		rtk->crashlog_buffer = (struct apple_rtkit_shmem){};
		return ret;
	}

	dev_info(rtk->dev, "RTKit: reused crashlog buffer at %pad\n",
		 &rtk->crashlog_buffer.iova);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_rtkit_reuse_crashlog_buffer);

static int apple_rtkit_set_iop_power_state(struct apple_rtkit *rtk,
					   unsigned int state)
{
	u64 msg;
	int ret;

	reinit_completion(&rtk->iop_pwr_ack_completion);

	msg = FIELD_PREP(APPLE_RTKIT_MGMT_PWR_STATE, state);
	ret = apple_rtkit_management_send(rtk, APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE,
					  msg);
	if (ret)
		return ret;

	ret = apple_rtkit_wait_for_completion(rtk, &rtk->iop_pwr_ack_completion);
	if (ret)
		return ret;

	/* _handlePowerAck (K:b7024d0): the low byte 1 or 2 is idle */
	if (state == APPLE_RTKIT_PWR_STATE_IDLE)
		return ((rtk->iop_power_state & 0xff) == 1 ||
			(rtk->iop_power_state & 0xff) == 2) ? 0 : -EINVAL;
	if (rtk->iop_power_state != state)
		return -EINVAL;
	return 0;
}

int apple_rtkit_boot(struct apple_rtkit *rtk)
{
	int ret;

	if (apple_rtkit_is_running(rtk))
		return 0;
	if (rtk->crashed)
		return -EINVAL;

	dev_dbg(rtk->dev, "RTKit: waiting for boot to finish\n");
	ret = apple_rtkit_wait_for_completion(rtk, &rtk->epmap_completion);
	if (ret) {
		dev_err(rtk->dev, "RTKit: timed out waiting for endpoint map: %d\n", ret);
		return ret;
	}
	if (rtk->boot_result)
		return rtk->boot_result;

	dev_dbg(rtk->dev, "RTKit: waiting for IOP power state ACK\n");
	ret = apple_rtkit_wait_for_completion(rtk, &rtk->iop_pwr_ack_completion);
	if (ret) {
		dev_err(rtk->dev,
			"RTKit: timed out waiting for IOP power ACK (state %#x): %d\n",
			rtk->iop_power_state, ret);
		return ret;
	}
	/* _iopValidate (K:b6efb78) waits for status 8: the type-7 low byte 0x20 */
	if ((rtk->iop_power_state & 0xff) != APPLE_RTKIT_PWR_STATE_ON) {
		dev_err(rtk->dev, "RTKit: IOP power ACK %#x is not on\n", rtk->iop_power_state);
		return -EINVAL;
	}
	if (rtk->early_crashlog) {
		ret = apple_rtkit_wait_for_completion(rtk, &rtk->crashlog_completion);
		if (ret) {
			dev_err(rtk->dev, "RTKit: timed out waiting for crash buffer: %d\n", ret);
			return ret;
		}
	}
	if (!rtk->boot_ap_power) {
		dev_dbg(rtk->dev, "RTKit: reusing boot AP power ownership\n");
		rtk->ap_power_state = APPLE_RTKIT_PWR_STATE_ON;
		apple_rtkit_watchdog_arm(rtk);
		return 0;
	}

	ret = apple_rtkit_set_ap_power_state(rtk, APPLE_RTKIT_PWR_STATE_ON);
	if (ret)
		dev_err(rtk->dev,
			"RTKit: failed waiting for AP power ACK (state %#x): %d\n",
			rtk->ap_power_state, ret);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_rtkit_boot);

int apple_rtkit_shutdown(struct apple_rtkit *rtk)
{
	int ret;

	/* if OFF is used here the co-processor will not wake up again */
	ret = apple_rtkit_set_ap_power_state(rtk,
					     APPLE_RTKIT_PWR_STATE_QUIESCED);
	if (ret)
		return ret;

	ret = apple_rtkit_set_iop_power_state(rtk, APPLE_RTKIT_PWR_STATE_SLEEP);
	if (ret)
		return ret;

	return apple_rtkit_reinit(rtk);
}
EXPORT_SYMBOL_GPL(apple_rtkit_shutdown);

int apple_rtkit_poweroff(struct apple_rtkit *rtk)
{
	int ret;

	ret = apple_rtkit_set_ap_power_state(rtk, APPLE_RTKIT_PWR_STATE_OFF);
	if (ret)
		return ret;

	ret = apple_rtkit_set_iop_power_state(rtk, APPLE_RTKIT_PWR_STATE_OFF);
	if (ret)
		return ret;

	return apple_rtkit_reinit(rtk);
}
EXPORT_SYMBOL_GPL(apple_rtkit_poweroff);

int apple_rtkit_idle(struct apple_rtkit *rtk)
{
	int ret;

	/* if OFF is used here the co-processor will not wake up again */
	ret = apple_rtkit_set_ap_power_state(rtk,
					     APPLE_RTKIT_PWR_STATE_IDLE);
	if (ret)
		return ret;

	ret = apple_rtkit_set_iop_power_state(rtk, APPLE_RTKIT_PWR_STATE_IDLE);
	if (ret)
		return ret;

	/*
	 * enableMailboxInterrupts(false), then every endpoint drained
	 * (agent-rtbuddy-action2.md step 5), until the next RUN
	 */
	disable_irq(rtk->mbox->irq_recv_not_empty);
	rtk->irq_off = true;
	flush_workqueue(rtk->wq);

	rtk->iop_power_state = APPLE_RTKIT_PWR_STATE_IDLE;
	rtk->ap_power_state = APPLE_RTKIT_PWR_STATE_IDLE;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_rtkit_idle);

int apple_rtkit_quiesce(struct apple_rtkit *rtk)
{
	int ret;

	ret = apple_rtkit_set_ap_power_state(rtk,
					     APPLE_RTKIT_PWR_STATE_QUIESCED);
	if (ret)
		return ret;

	ret = apple_rtkit_set_iop_power_state(rtk,
					      APPLE_RTKIT_PWR_STATE_QUIESCED);
	if (ret)
		return ret;

	ret = apple_rtkit_reinit(rtk);
	if (ret)
		return ret;

	rtk->iop_power_state = APPLE_RTKIT_PWR_STATE_QUIESCED;
	rtk->ap_power_state = APPLE_RTKIT_PWR_STATE_QUIESCED;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_rtkit_quiesce);

int apple_rtkit_wake(struct apple_rtkit *rtk)
{
	u64 msg;
	int ret;

	if (apple_rtkit_is_running(rtk))
		return -EINVAL;

	reinit_completion(&rtk->iop_pwr_ack_completion);

	/*
	 * Use open-coded apple_rtkit_set_iop_power_state since apple_rtkit_boot
	 * will wait for the completion anyway.
	 */
	msg = FIELD_PREP(APPLE_RTKIT_MGMT_PWR_STATE, APPLE_RTKIT_PWR_STATE_INIT);
	ret = apple_rtkit_management_send(rtk, APPLE_RTKIT_MGMT_SET_IOP_PWR_STATE,
					  msg);
	if (ret)
		return ret;

	return apple_rtkit_boot(rtk);
}
EXPORT_SYMBOL_GPL(apple_rtkit_wake);

/*
 * RTBuddy::_performPowerStateChangeGated wake branch (K:b6ef268):
 * _setIopStatus(4), then AppleA7IOP::startCPUWithOptions (K:8c08c4c):
 * outbox enable (ASC+0x8114 bit 0, the mailbox I2A control) and the mailbox
 * interrupts on, then the caller sets RUN and calls apple_rtkit_boot(), whose
 * _iopValidate (K:b6efb78) waits only for the IOP's own power acknowledge
 * 0x20. A HELLO and roll call from the firmware are answered, the roll call
 * as a later one (rollcall_done); no IOP power state message is sent.
 */
void apple_rtkit_expect_iop_wake(struct apple_rtkit *rtk)
{
	reinit_completion(&rtk->iop_pwr_ack_completion);
	apple_mbox_set_outbox(rtk->mbox, true);
	if (rtk->irq_off) {
		rtk->irq_off = false;
		enable_irq(rtk->mbox->irq_recv_not_empty);
	}
}
EXPORT_SYMBOL_GPL(apple_rtkit_expect_iop_wake);

void apple_rtkit_warm_restart(struct apple_rtkit *rtk)
{
	apple_rtkit_expect_iop_wake(rtk);
}
EXPORT_SYMBOL_GPL(apple_rtkit_warm_restart);

/*
 * AppleA7IOP::stopCPU's mailbox half on AppleASCWrapV6 (K:8c08fb0), after
 * the IOP went idle: _enableOutbox(false) (K:8c0596c), then _isIdle once and
 * after each of up to ten 1 ms sleeps: with idle-ctrl-check ASC+0x40 & 1,
 * then ASC+0x8000 & 3, in the same check (K:8c05420-8c054b4). Still busy is a
 * panic (A7IOP+0x109, set by the constructor, K:8c07ca8; K:8c090e4-8c0911c).
 * The caller clears RUN and bit 5 afterwards.
 */
void apple_rtkit_stop_outbox(struct apple_rtkit *rtk, void __iomem *idle_ctrl)
{
	u32 v = 0;
	int i;

	apple_mbox_set_outbox(rtk->mbox, false);
	for (i = 0; i <= 10; i++) {
		if (i)
			msleep(1);
		if (idle_ctrl && !((v = readl(idle_ctrl)) & 1))
			continue;
		if (apple_mbox_idle(rtk->mbox))
			return;
	}
	panic("%s failed to become idle (0x%08x) in %dms\n", dev_name(rtk->dev), v, 10);
}
EXPORT_SYMBOL_GPL(apple_rtkit_stop_outbox);

void apple_rtkit_free(struct apple_rtkit *rtk)
{
	WRITE_ONCE(rtk->watchdog_armed, false);
	cancel_delayed_work_sync(&rtk->ping_work);
	if (rtk->irq_off)
		enable_irq(rtk->mbox->irq_recv_not_empty);
	apple_mbox_stop(rtk->mbox);
	destroy_workqueue(rtk->wq);

	apple_rtkit_free_buffer(rtk, &rtk->ioreport_buffer);
	apple_rtkit_free_buffer(rtk, &rtk->system7_buffer);
	apple_rtkit_free_buffer(rtk, &rtk->crashlog_buffer);
	apple_rtkit_free_buffer(rtk, &rtk->oslog_buffer);
	apple_rtkit_free_buffer(rtk, &rtk->syslog_buffer);

	kfree(rtk->syslog_msg_buffer);
	kfree(rtk);
}
EXPORT_SYMBOL_GPL(apple_rtkit_free);

static void apple_rtkit_free_wrapper(void *data)
{
	apple_rtkit_free(data);
}

struct apple_rtkit *devm_apple_rtkit_init(struct device *dev, void *cookie,
					  const char *mbox_name, int mbox_idx,
					  const struct apple_rtkit_ops *ops)
{
	struct apple_rtkit *rtk;
	int ret;

	rtk = apple_rtkit_init(dev, cookie, mbox_name, mbox_idx, ops);
	if (IS_ERR(rtk))
		return rtk;

	ret = devm_add_action_or_reset(dev, apple_rtkit_free_wrapper, rtk);
	if (ret)
		return ERR_PTR(ret);

	return rtk;
}
EXPORT_SYMBOL_GPL(devm_apple_rtkit_init);

void devm_apple_rtkit_free(struct device *dev, struct apple_rtkit *rtk)
{
	devm_release_action(dev, apple_rtkit_free_wrapper, rtk);
}
EXPORT_SYMBOL_GPL(devm_apple_rtkit_free);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_DESCRIPTION("Apple RTKit driver");
