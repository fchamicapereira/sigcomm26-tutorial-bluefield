#ifndef DOCA_FLOW_COMPAT_H_
#define DOCA_FLOW_COMPAT_H_

#include <doca_version.h>
#include <doca_flow.h>

/* DOCA 2.9 renamed doca_flow_query -> doca_flow_resource_query (struct wrapped in a `counter`
 * union member), doca_flow_query_entry -> doca_flow_resource_query_entry, and
 * doca_flow_shared_resource_cfg -> doca_flow_shared_resource_set_cfg. Shim the old (pre-2.9)
 * names to the new call sites this directory's .c files use, so the same source builds against
 * either DOCA release. Force-included via -include in meson.build, not #include'd directly.
 */
#if DOCA_VERSION_MAJOR == 2 && DOCA_VERSION_MINOR < 9

struct doca_flow_resource_query {
	struct doca_flow_query counter;
};

static inline doca_error_t doca_flow_resource_query_entry(struct doca_flow_pipe_entry *entry,
							    struct doca_flow_resource_query *query)
{
	return doca_flow_query_entry(entry, &query->counter);
}

#define doca_flow_shared_resource_set_cfg doca_flow_shared_resource_cfg

#endif /* DOCA < 2.9 */

/*
 * Shared-mirror "original packet destination" (doca_flow_resource_mirror_cfg.fwd).
 *
 * The header calls the field optional, but the two releases disagree about what leaving it zero
 * means, and each REJECTS the other's answer:
 *
 *   DOCA 2.7  zero is DOCA_FLOW_FWD_NONE, taken literally: the original packet is dropped, the
 *             mirror delivers nothing, and the PF0 data path goes dark with no error logged.
 *             Measured on testbed B (2.7, FW 32.41.1000): unset = RoCE never connects, 0 packets
 *             captured; set = 92.27 Gb/s (line rate, matching the no-program baseline) with
 *             2.35M CE-marked packets in the pcap.
 *   DOCA 2.9  setting it makes doca_flow_shared_resources_bind fail outright —
 *             "Failed to create HWS mirror action", FTE syndrome 0x6d3a25 — so the program will
 *             not start at all. Leaving it zero is correct here; 2.9 falls back to the entry fwd.
 *
 * It tracks the library, not the firmware: DOCA 2.9 userspace in a container on testbed B's own
 * 2.7-era firmware works with the field unset.
 */
#if DOCA_VERSION_MAJOR == 2 && DOCA_VERSION_MINOR < 9
#define DOCA_TUT_MIRROR_SET_ORIG_FWD(mc, dest_port) \
	do {                                        \
		(mc).fwd.type = DOCA_FLOW_FWD_PORT; \
		(mc).fwd.port_id = (dest_port);     \
	} while (0)
#else
#define DOCA_TUT_MIRROR_SET_ORIG_FWD(mc, dest_port) \
	do {                                        \
		(void)(dest_port);                  \
	} while (0)
#endif

#endif /* DOCA_FLOW_COMPAT_H_ */
