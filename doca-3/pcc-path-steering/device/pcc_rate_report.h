/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
 *
 * Shared DPA-to-host per-flow rate-report definitions. DOCA 3.x uses PCC binary
 * trace reports; DOCA 2.x exposes the latest per-QPN snapshot through the PCC
 * mailbox because the binary trace callback is unavailable there.
 */

#ifndef PCC_DEVICE_RATE_REPORT_H_
#define PCC_DEVICE_RATE_REPORT_H_

/*
 * Trace format ID for per-flow rate reports.
 * Must not collide with existing format IDs (0..9 are used by the app).
 */
#define PCC_RATE_REPORT_FORMAT_ID (6)

#define PCC_RATE_MAILBOX_VERSION 1u
#define PCC_RATE_MAILBOX_MAX_FLOWS 64u

struct pcc_rate_mailbox_request {
	uint32_t version;
};

struct pcc_rate_mailbox_entry {
	uint32_t qpn;
	uint32_t rate;
};

struct pcc_rate_mailbox_response {
	uint32_t version;
	uint32_t count;
	uint32_t dropped;
	struct pcc_rate_mailbox_entry flow[PCC_RATE_MAILBOX_MAX_FLOWS];
};

/*
 * Trace arguments layout (5 x 64-bit):
 *   arg1 = flow QPN
 *   arg2 = new rate (FXP20 format, same as results->rate)
 *   arg3 = event type that caused the rate change
 *   arg4 = current RTT (nanoseconds)
 *   arg5 = timestamp (doca_pcc_dev_get_timer_lo)
 */

#endif /* PCC_DEVICE_RATE_REPORT_H_ */
