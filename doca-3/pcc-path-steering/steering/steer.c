/* PCC-informed RoCE path-steering demo (BF3 p0<->p1 DAC loopback).
 *
 * Egress admits IPv4 RoCEv2/UDP 4791 and uses parser_meta.random to write a
 * two-way DSCP path marker. Ingress first demuxes by path, then runs one
 * destination-IP marker per path. Only packets whose receiver IP belongs to the selected path may be newly CE-marked; all
 * branches clear the private path marker before SF delivery.
 */

#include <doca_dev.h>
#include <doca_dpdk.h>
#include <doca_flow.h>
#include <doca_log.h>
#include "doca_flow_compat.h"
#include "steer.h"
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_udp.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdatomic.h>

DOCA_LOG_REGISTER(FLOW_STEER);

#define NB_QUEUES 1
#define NB_COUNTERS 16
#define PCC_FULL_RATE (1u << 20)
#define MAX_RATE_FLOWS 256u
#define MAX_CM_CONNECTIONS 256u
#define MAX_CNP_TRACKED_QPS 64u
#define PATH_SHARE_BUCKETS 64u
#define PATH_SHARE_MIN_BUCKETS 3u
/* Persistent host-side EWMA: new sample weight = numerator / denominator. */
#define PATH_RATE_EWMA_NUMERATOR 1u
#define PATH_RATE_EWMA_DENOMINATOR 8u

#if PATH_RATE_EWMA_NUMERATOR == 0 || PATH_RATE_EWMA_NUMERATOR > PATH_RATE_EWMA_DENOMINATOR
#error "PATH_RATE_EWMA_NUMERATOR must be in [1, PATH_RATE_EWMA_DENOMINATOR]"
#endif

#define ROCE_UDP_PORT_NATIVE 4791
#define QP1_QPN 1u
#define QP1_CLONE_QUEUE 0u
#define QP1_RX_BURST 32u
#define QP1_WIRE_MIRROR_ID 1u
#define QP1_SF_MIRROR_ID 2u
#define ARP_MIRROR_ID 3u
/* Shared-resource IDs start at 1 on the DOCA 2 backend, while the resource
 * count is an exclusive upper bound and therefore includes unused slot 0. */
#define LEGACY_SHARED_MIRRORS (ARP_MIRROR_ID + 1u)
#define IB_MGMT_CLASS_CM 0x07u
#define IB_CM_ATTR_REQ 0x0010u
#define IB_CM_ATTR_REP 0x0013u
#define ROCE_BTH_OPCODE_CNP 0x81u
#define IP4_DSCP_ECN_CE 0x03	 /* ECN=11 (CE); set masked so DSCP is preserved */
#define IP4_ECN_MASK 0x03	 /* the two ECN bits of the ToS byte */
#define IP4_ECN_ECT0 0x02	 /* ECN=10 (ECT(0)) */
/*
 * On-wire "current-path" marker. The two virtual paths are distinguished purely
 * by one bit of the IP ToS byte (DSCP+ECN is a *variant* field excluded from the
 * RoCEv2 ICRC -- proven safe by CE-marking working -- so writing it is
 * transparent to RoCE, unlike the UDP destination port which the ICRC covers).
 *
 * Egress randomly writes the current-path bit per packet. Ingress uses the QPN
 * classification together with that bit to decide whether marked ECN is kept.
 * Ingress demuxes on this bit alone (no dst-IP needed, so both QPs may share one
 * receiver IP), marks per path, then clears the bit before SF delivery. ToS bit
 * 2 (DSCP LSB) is 0 for common RoCE DSCP classes (e.g. 24/26); change
 * PATH_DSCP_MASK if your deployment uses a DSCP whose LSB is set.
 */
#define PATH_DSCP_MASK 0x04
/* path id (0/1) -> ToS value for the masked write, and back */
#define PATH_DSCP_VAL(path) ((uint8_t)((path) ? PATH_DSCP_MASK : 0x00))

/* EGRESS_CLASSIFY distributes packets independently of QPN. The low bits of
 * the HW parser random value select an explicit per-packet bucket. */

#define NB_PATHS STEER_NB_PATHS

/*
 * parser_meta.random is a 16-bit HW per-packet value independent of packet
 * content. Masking it restricts marking to a power-of-two fraction of traffic
 * (same technique as the tutorial doca_flow_ecn.c / DOCA flow_random sample).
 */
#define RANDOM_FIELD_WIDTH 16
#define LEGACY_RANDOM_BUCKETS 64

/* Log at CRIT level and terminate if err != DOCA_SUCCESS -- mirrors rte_exit(). */
static __attribute__((format(printf, 2, 3))) void crash_if_unsuccessful(doca_error_t err, const char *fmt, ...)
{
	if (err == DOCA_SUCCESS)
		return;

	char msg[512];
	va_list args;

	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);

	DOCA_LOG_CRIT("%s: %s", msg, doca_error_get_descr(err));
	exit(EXIT_FAILURE);
}

/* DOCA Flow global entry-process callback -- updates a per-batch status struct. */
struct entry_batch_status {
	bool failure;
	uint32_t nb_processed;
};

static void entry_process_cb(struct doca_flow_pipe_entry *entry, uint16_t pipe_queue,
			     enum doca_flow_entry_status status, enum doca_flow_entry_op op, void *user_ctx)
{
	(void)entry;
	(void)pipe_queue;
	/* Entry usr_ctx points at the batch status used while an ADD/UPDATE is
	 * synchronously processed. Most setup batches are stack-local and are no
	 * longer valid when a later pipe flush reports DEL/AGED completions. */
	if (op != DOCA_FLOW_ENTRY_OP_ADD && op != DOCA_FLOW_ENTRY_OP_UPD)
		return;
	struct entry_batch_status *s = user_ctx;

	if (s == NULL)
		return;
	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS)
		s->failure = true;
	s->nb_processed++;
}

/*
 * Stable usr_ctx for classify entries because live updates reuse the usr_ctx
 * bound when each rule was added.
 */
static struct entry_batch_status g_classify_batch;

/* Block until a just-added batch of `n` entries is processed, or crash. */
static void process_entries(struct doca_flow_port *port, struct entry_batch_status *status, uint32_t n, const char *label)
{
	doca_error_t err = doca_flow_entries_process(port, 0, 10000 /* us */, n);

	crash_if_unsuccessful(err, "doca_flow_entries_process (%s)", label);
	err = (status->failure || status->nb_processed != n) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS;
	crash_if_unsuccessful(err, "%s: %u/%u entries processed", label, status->nb_processed, n);
}

/* A stable, role-specific DPDK --file-prefix so the egress and ingress steering
 * primaries (and doca_pcc's embedded egress) never share a runtime config dir. */
const char *steer_eal_prefix_for_role(int role)
{
	switch (role) {
	case STEER_ROLE_EGRESS:
		return "pcc-egress";
	case STEER_ROLE_INGRESS:
		return "pcc-ingress";
	default:
		return "pcc-steer";
	}
}

/*
 * EAL init for the steering datapath. Appends a dummy -a allowlist entry (so EAL
 * does not auto-probe real PCI devices; the device is attached later via
 * doca_dpdk_port_probe*) and a unique --file-prefix so this DPDK primary does not
 * collide with another primary on the host ("Cannot create lock on
 * /var/run/dpdk/<prefix>/config"). Pass steer_eal_prefix_for_role(role).
 */
doca_error_t steer_eal_init(int argc, char **argv, const char *file_prefix)
{
	static char allow_flag[] = "-a";
	/* A well-formed but nonexistent PCI address puts EAL in allowlist mode so it
	 * auto-probes nothing; the real device is attached later via
	 * doca_dpdk_port_probe*. (An empty "-a \"\"" does NOT enable allowlist mode, so
	 * EAL auto-probes the NIC and the explicit probe then fails with
	 * "cmd_fd mismatch / Probe again".) */
	static char allow_none[] = "pci:00:00.0";
	static char allow_aux_none[] = "auxiliary:mlx5_core.sf.4294967295";
	static char prefix_flag[] = "--file-prefix";
	char *new_argv[64];

	if (argc >= 57) {
		DOCA_LOG_ERR("Too many EAL arguments");
		return DOCA_ERROR_INVALID_VALUE;
	}
	for (int i = 0; i < argc; i++)
		new_argv[i] = argv[i];
	new_argv[argc] = allow_flag;
	new_argv[argc + 1] = allow_none;
	new_argv[argc + 2] = allow_flag;
	new_argv[argc + 3] = allow_aux_none;
	new_argv[argc + 4] = prefix_flag;
	new_argv[argc + 5] = (char *)((file_prefix && file_prefix[0]) ? file_prefix : "pcc-steer");

	if (rte_eal_init(argc + 6, new_argv) < 0) {
		DOCA_LOG_ERR("EAL initialization failed");
		return DOCA_ERROR_DRIVER;
	}
	return DOCA_SUCCESS;
}

static uint16_t g_dpdk_rx_port_id = UINT16_MAX;

#if DOCA_HAS_DEVICE_REPRESENTORS

/*
 * DOCA 3.x: the PF doca_dev and SF doca_dev_rep are opened by the caller (DOCA
 * argp --device/--rep, or the embedding process) and probed into DPDK together.
 * devargs default: dv_flow_en=2 (HWS) + fdb_def_rule_en=1 (keep the kernel FDB
 * default for the other PF). NOTE: repr_matching_en and representor=sfN are NOT
 * valid probe keys on 3.x -- the representor is passed as a doca_dev_rep object.
 */
static void probe_device(struct doca_dev *dev, const char *devargs, struct doca_dev_rep **dev_reps,
			 uint32_t nb_reps)
{
	const char *args = (devargs && devargs[0]) ? devargs : "dv_flow_en=2,fdb_def_rule_en=1";
	doca_error_t err = doca_dpdk_port_probe_with_representors(dev, args, dev_reps, nb_reps);

	crash_if_unsuccessful(err, "doca_dpdk_port_probe_with_representors");
}

#else /* DOCA 2.x */

/* Open the PF selected by -r and probe its requested SF representors into DPDK. */
static struct doca_dev *open_and_probe_dev(const char *device_pci_addr, const char *probe_args)
{
	struct doca_devinfo **devinfo_list;
	uint32_t nb_devs;
	struct doca_dev *dev = NULL;
	doca_error_t err;

	err = doca_devinfo_create_list(&devinfo_list, &nb_devs);
	crash_if_unsuccessful(err, "doca_devinfo_create_list");
	for (uint32_t i = 0; i < nb_devs; i++) {
		uint8_t is_equal = 0;

		if (doca_devinfo_is_equal_pci_addr(devinfo_list[i], device_pci_addr, &is_equal) != DOCA_SUCCESS ||
		    !is_equal)
			continue;
		err = doca_dev_open(devinfo_list[i], &dev);
		crash_if_unsuccessful(err, "doca_dev_open (%s)", device_pci_addr);
		break;
	}
	doca_devinfo_destroy_list(devinfo_list);
	if (dev == NULL) {
		DOCA_LOG_CRIT("PCI device %s not found", device_pci_addr);
		exit(EXIT_FAILURE);
	}

	err = doca_dpdk_port_probe(dev, probe_args);
	crash_if_unsuccessful(err, "doca_dpdk_port_probe (%s)", device_pci_addr);
	return dev;
}

static void probe_open_dev(struct doca_dev *dev, const char *device_pci_addr, const char *probe_args)
{
	uint8_t is_equal = 0;
	doca_error_t err = doca_devinfo_is_equal_pci_addr(doca_dev_as_devinfo(dev),
	                                                   device_pci_addr, &is_equal);

	crash_if_unsuccessful(err, "compare PCC device PCI address");
	if (!is_equal) {
		DOCA_LOG_CRIT("-r PF %s does not match the PCC --device", device_pci_addr);
		exit(EXIT_FAILURE);
	}
	err = doca_dpdk_port_probe(dev, probe_args);

	crash_if_unsuccessful(err, "doca_dpdk_port_probe (supplied PCC device)");
}

#endif

#if DOCA_USES_LEGACY_FLOW_BACKEND
static uint16_t find_pf_dpdk_port_id(void)
{
	uint16_t port_id;

	RTE_ETH_FOREACH_DEV(port_id) {
		struct rte_eth_dev_info dev_info = {0};

		if (rte_eth_dev_info_get(port_id, &dev_info) < 0)
			continue;
		if (dev_info.dev_flags == NULL ||
		    (*dev_info.dev_flags & RTE_ETH_DEV_REPRESENTOR) == 0)
			return port_id;
	}
	DOCA_LOG_CRIT("No non-representor PF ethdev found for the eSwitch proxy");
	exit(EXIT_FAILURE);
}
#endif

/*
 * DPDK must be configured and started before DOCA Flow (HWS requirement), and
 * needs at least one RX queue to start. Isolated mode: no ingress goes to RSS
 * queues -- every packet is steered by flow rules alone.
 */
static void configure_and_start_dpdk_port(struct doca_dev *dev)
{
	uint16_t first_port_id;
#if DOCA_USES_LEGACY_FLOW_BACKEND
	(void)dev;
	first_port_id = find_pf_dpdk_port_id();
#else
	doca_error_t err = doca_dpdk_get_first_port_id(dev, &first_port_id);

	crash_if_unsuccessful(err, "doca_dpdk_get_first_port_id");
#endif
	g_dpdk_rx_port_id = first_port_id;

	struct rte_mempool *mp = rte_pktmbuf_pool_create("mbuf_pool", 8192, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
							 rte_eth_dev_socket_id(first_port_id));
	if (mp == NULL) {
		DOCA_LOG_CRIT("rte_pktmbuf_pool_create failed");
		exit(EXIT_FAILURE);
	}

	uint16_t port_id;

	RTE_ETH_FOREACH_DEV(port_id)
	{
		struct rte_eth_dev_info dev_info = {0};
		int ret = rte_eth_dev_info_get(port_id, &dev_info);

		if (ret < 0) {
			DOCA_LOG_CRIT("rte_eth_dev_info_get port %u failed (errno %d)", port_id, -ret);
			exit(EXIT_FAILURE);
		}

		struct rte_eth_conf eth_conf = {0};

		ret = rte_eth_dev_configure(port_id, NB_QUEUES, NB_QUEUES, &eth_conf);
		if (ret < 0) {
			DOCA_LOG_CRIT("rte_eth_dev_configure port %u failed (errno %d)", port_id, -ret);
			exit(EXIT_FAILURE);
		}

		struct rte_eth_txconf tx_conf = dev_info.default_txconf;

		for (int q = 0; q < NB_QUEUES; q++) {
			ret = rte_eth_rx_queue_setup(port_id, q, 512, rte_eth_dev_socket_id(port_id), NULL, mp);
			if (ret < 0) {
				DOCA_LOG_CRIT("rte_eth_rx_queue_setup port %u q%d failed (errno %d)", port_id, q,
					      -ret);
				exit(EXIT_FAILURE);
			}
			ret = rte_eth_tx_queue_setup(port_id, q, 512, rte_eth_dev_socket_id(port_id), &tx_conf);
			if (ret < 0) {
				DOCA_LOG_CRIT("rte_eth_tx_queue_setup port %u q%d failed (errno %d)", port_id, q,
					      -ret);
				exit(EXIT_FAILURE);
			}
		}

#if DOCA_USES_LEGACY_FLOW_BACKEND
		/* 2.9 used "switch,hws,isolated,disable_switch_rss"; isolated mode must
		 * be set before start. 3.x uses plain "switch,hws" and does not isolate. */
		struct rte_flow_error flow_err = {0};

		ret = rte_flow_isolate(port_id, 1, &flow_err);
		if (ret < 0) {
			DOCA_LOG_CRIT("rte_flow_isolate port %u failed (errno %d): %s", port_id, -ret,
				      flow_err.message ? flow_err.message : "no details");
			exit(EXIT_FAILURE);
		}
#endif

		ret = rte_eth_dev_start(port_id);
		if (ret < 0) {
			DOCA_LOG_CRIT("rte_eth_dev_start port %u failed (errno %d)", port_id, -ret);
			exit(EXIT_FAILURE);
		}
	}
}

static void initialize_doca_flow(void)
{
	struct doca_flow_cfg *cfg;
	doca_error_t err = doca_flow_cfg_create(&cfg);

	crash_if_unsuccessful(err, "doca_flow_cfg_create");

	err = doca_flow_cfg_set_pipe_queues(cfg, NB_QUEUES);
	crash_if_unsuccessful(err, "doca_flow_cfg_set_pipe_queues");

#if STEER_HAS_PORT_RESOURCE_MODE
	err = doca_flow_cfg_set_mode_args(cfg, "switch,hws");
	crash_if_unsuccessful(err, "doca_flow_cfg_set_mode_args");

	/* Port-level resource mode (counters allocated per port via nr_resources). */
	err = doca_flow_cfg_set_resource_mode(cfg, DOCA_FLOW_RESOURCE_MODE_PORT);
	crash_if_unsuccessful(err, "doca_flow_cfg_set_resource_mode");
#else
	/* DOCA 3.1 and 2.x allocate counters globally. DOCA 3.1 otherwise uses
	 * the normal 3.x switch mode and must not enable the old isolated mode. */
#if DOCA_HAS_DEVICE_REPRESENTORS
	err = doca_flow_cfg_set_mode_args(cfg, "switch,hws");
#else
	err = doca_flow_cfg_set_mode_args(cfg, "switch,hws,isolated,disable_switch_rss");
#endif
	crash_if_unsuccessful(err, "doca_flow_cfg_set_mode_args");

	err = doca_flow_cfg_set_nr_counters(cfg, NB_COUNTERS);
	crash_if_unsuccessful(err, "doca_flow_cfg_set_nr_counters");
#if DOCA_USES_LEGACY_FLOW_BACKEND
	err = doca_flow_cfg_set_nr_shared_resource(cfg, LEGACY_SHARED_MIRRORS,
	                                           DOCA_FLOW_SHARED_RESOURCE_MIRROR);
	crash_if_unsuccessful(err, "doca_flow_cfg_set_nr_shared_resource (mirror)");
#endif
#endif

	err = doca_flow_cfg_set_cb_entry_process(cfg, entry_process_cb);
	crash_if_unsuccessful(err, "doca_flow_cfg_set_cb_entry_process");

	err = doca_flow_init(cfg);
	crash_if_unsuccessful(err, "doca_flow_init");

	doca_flow_cfg_destroy(cfg);
}

static struct doca_flow_port *port_start(struct doca_dev *dev, uint16_t flow_port_id)
{
	struct doca_flow_port_cfg *cfg;
	doca_error_t err = doca_flow_port_cfg_create(&cfg);

	crash_if_unsuccessful(err, "doca_flow_port_cfg_create");

	err = doca_flow_port_cfg_set_dev(cfg, dev);
	crash_if_unsuccessful(err, "doca_flow_port_cfg_set_dev");

	/* Logical port id 0 = PF uplink (also the parser_meta source-port value). */
	err = steer_port_cfg_set_port_id(cfg, flow_port_id);
	crash_if_unsuccessful(err, "steer_port_cfg_set_port_id (uplink %u)", flow_port_id);

#if DOCA_HAS_DEVICE_REPRESENTORS
	err = doca_flow_port_cfg_set_actions_mem_size(cfg, 256 * DOCA_FLOW_MAX_ENTRY_ACTIONS_MEM_SIZE);
	crash_if_unsuccessful(err, "doca_flow_port_cfg_set_actions_mem_size");
#if STEER_HAS_PORT_RESOURCE_MODE
	err = doca_flow_port_cfg_set_nr_resources(cfg, DOCA_FLOW_RESOURCE_COUNTER, 128);
	crash_if_unsuccessful(err, "doca_flow_port_cfg_set_nr_resources (counter)");
#endif
#else
	{
		char port_id_str[8];

		snprintf(port_id_str, sizeof(port_id_str), "%u", flow_port_id);
		err = doca_flow_port_cfg_set_devargs(cfg, port_id_str);
		crash_if_unsuccessful(err, "doca_flow_port_cfg_set_devargs");
	}
#endif

	struct doca_flow_port *port;

	err = doca_flow_port_start(cfg, &port);
	crash_if_unsuccessful(err, "doca_flow_port_start");

	doca_flow_port_cfg_destroy(cfg);
	return port;
}

#if DOCA_USES_LEGACY_FLOW_BACKEND
/* Find the DPDK port id of the SF representor (2.9: probed via "representor=sfN"). */
static uint32_t find_sf_representor_port_ids(uint16_t ids[NB_PATHS], uint32_t needed)
{
	uint16_t port_id;
	uint32_t found = 0;

	RTE_ETH_FOREACH_DEV(port_id)
	{
		struct rte_eth_dev_info dev_info = {0};

		if (rte_eth_dev_info_get(port_id, &dev_info) < 0)
			continue;
		if (dev_info.dev_flags != NULL && (*dev_info.dev_flags & RTE_ETH_DEV_REPRESENTOR) != 0) {
			if (found < needed)
				ids[found] = port_id;
			DOCA_LOG_INFO("SF representor %u found on DPDK port %u", found, port_id);
			found++;
		}
	}
	if (found < needed) {
		DOCA_LOG_CRIT("Expected %u SF representors, found %u", needed, found);
		exit(EXIT_FAILURE);
	}
	return found;
}
#endif

#if DOCA_HAS_DEVICE_REPRESENTORS
/* 3.x: start the SF representor as a DOCA Flow port (logical id 1) via its doca_dev_rep. */
static struct doca_flow_port *rep_port_start(uint16_t flow_port_id, struct doca_dev_rep *dev_rep)
{
	struct doca_flow_port_cfg *cfg;
	doca_error_t err = doca_flow_port_cfg_create(&cfg);

	crash_if_unsuccessful(err, "doca_flow_port_cfg_create (rep port %u)", flow_port_id);

	err = doca_flow_port_cfg_set_port_id(cfg, flow_port_id);
	crash_if_unsuccessful(err, "doca_flow_port_cfg_set_port_id (rep port %u)", flow_port_id);

	err = doca_flow_port_cfg_set_dev_rep(cfg, dev_rep);
	crash_if_unsuccessful(err, "doca_flow_port_cfg_set_dev_rep (rep port %u)", flow_port_id);

	err = doca_flow_port_cfg_set_actions_mem_size(cfg, 256 * DOCA_FLOW_MAX_ENTRY_ACTIONS_MEM_SIZE);
	crash_if_unsuccessful(err, "doca_flow_port_cfg_set_actions_mem_size (rep port %u)", flow_port_id);

	struct doca_flow_port *port;

	err = doca_flow_port_start(cfg, &port);
	crash_if_unsuccessful(err, "doca_flow_port_start (rep port %u)", flow_port_id);

	doca_flow_port_cfg_destroy(cfg);
	return port;
}
#else
static struct doca_flow_port *rep_port_start(uint16_t dpdk_port_id)
{
	struct doca_flow_port_cfg *cfg;
	char port_id_str[8];

	snprintf(port_id_str, sizeof(port_id_str), "%u", dpdk_port_id);

	doca_error_t err = doca_flow_port_cfg_create(&cfg);

	crash_if_unsuccessful(err, "doca_flow_port_cfg_create (rep port %u)", dpdk_port_id);

	err = doca_flow_port_cfg_set_devargs(cfg, port_id_str);
	crash_if_unsuccessful(err, "doca_flow_port_cfg_set_devargs (rep port %u)", dpdk_port_id);

	struct doca_flow_port *port;

	err = doca_flow_port_start(cfg, &port);
	crash_if_unsuccessful(err, "doca_flow_port_start (rep port %u)", dpdk_port_id);

	doca_flow_port_cfg_destroy(cfg);
	return port;
}
#endif

/* Logical eSwitch ports: uplink 0, path-0 SF 1, optional path-1 SF 2. */
#define WIRE_PORT_ID 0
#define SF_PORT_ID 1
#define SF_PATH1_PORT_ID 2

/*
 * DELIVER pipe: forward-only basic pipe whose HIT fate is a port. On 3.x HWS a
 * pipe's fwd_miss may not be a port (only a pipe or drop), so pipes that need to
 * "deliver to a port on miss" point their fwd/fwd_miss at one of these via
 * FWD_PIPE instead. Matches all IPv4 (dscp_ecn wildcarded); non-IPv4 misses ->
 * NULL fwd_miss (mirrors the tutorial create_fwd_pipe).
 */
static struct doca_flow_pipe *create_deliver_pipe(struct doca_flow_port *port, const char *name, uint16_t dest_port_id)
{
	struct doca_flow_match match = {0}, match_mask = {0};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = dest_port_id};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	struct entry_batch_status status = {0};
	doca_error_t err;

	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.dscp_ecn = 0xFF;      /* variable -> non-empty HWS template */
	match_mask.outer.ip4.dscp_ecn = 0x00; /* wildcard: all IPv4 */

	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (%s)", name);
	err = doca_flow_pipe_cfg_set_name(cfg, name);
	crash_if_unsuccessful(err, "pipe_cfg_set_name (%s)", name);
	err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
	crash_if_unsuccessful(err, "pipe_cfg_set_type (%s)", name);
	err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	crash_if_unsuccessful(err, "pipe_cfg_set_domain (%s)", name);
	err = doca_flow_pipe_cfg_set_is_root(cfg, false);
	crash_if_unsuccessful(err, "pipe_cfg_set_is_root (%s)", name);
	err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
	crash_if_unsuccessful(err, "pipe_cfg_set_nr_entries (%s)", name);
	err = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
	crash_if_unsuccessful(err, "pipe_cfg_set_match (%s)", name);

	err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
	crash_if_unsuccessful(err, "pipe_create (%s)", name);
	doca_flow_pipe_cfg_destroy(cfg);

	struct doca_flow_match ematch = {0};

	err = steer_pipe_add_entry(0, pipe, &ematch, 0, NULL, NULL, NULL, 0, &status, &entry);
	crash_if_unsuccessful(err, "pipe_add_entry (%s)", name);
	process_entries(port, &status, 1, name);
	DOCA_LOG_INFO("%s ready: deliver IPv4 -> port %u", name, dest_port_id);
	return pipe;
}

/* Final PF0 receiver selection. The path bit and ECN policy are handled before
 * this pipe; delivery itself is determined solely by the packet's original
 * RoCE destination IP. */
static struct doca_flow_pipe *create_receiver_ip_demux(struct doca_flow_port *port,
						       struct doca_flow_pipe *deliver_sf[NB_PATHS],
						       const uint32_t path_ip[NB_PATHS])
{
	struct doca_flow_match match = {0}, match_mask = {0};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = deliver_sf[0]};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	doca_error_t err;

	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.dst_ip = UINT32_MAX;
	match_mask.outer.l3_type = UINT32_MAX;
	match_mask.outer.ip4.dst_ip = UINT32_MAX;

	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (receiver IP demux)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_name(cfg, "RECEIVER_IP_DEMUX"),
				"pipe_cfg_set_name (receiver IP demux)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC),
				"pipe_cfg_set_type (receiver IP demux)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_nr_entries(cfg, NB_PATHS),
				"pipe_cfg_set_nr_entries (receiver IP demux)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask),
				"pipe_cfg_set_match (receiver IP demux)");
	err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
	crash_if_unsuccessful(err, "pipe_create (receiver IP demux)");
	doca_flow_pipe_cfg_destroy(cfg);

	struct entry_batch_status status = {0};
	for (int path = 0; path < NB_PATHS; path++) {
		struct doca_flow_match entry_match = {0};
		struct doca_flow_fwd entry_fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = deliver_sf[path]};
		struct doca_flow_pipe_entry *entry;

		entry_match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		entry_match.outer.ip4.dst_ip = path_ip[path];
		err = steer_pipe_add_entry(0, pipe, &entry_match, 0, NULL, NULL, &entry_fwd,
					   path == 0 ? STEER_WAIT_FOR_BATCH : 0, &status, &entry);
		crash_if_unsuccessful(err, "pipe_add_entry (receiver IP path %d)", path);
	}
	process_entries(port, &status, NB_PATHS, "receiver IP demux entries");
	DOCA_LOG_INFO("RECEIVER_IP_DEMUX ready: path IPs -> SF ports 1/2");
	return pipe;
}

/*
 * Mask-based random matching supports only negative-power-of-two percentages
 * (50, 25, 12.5, ...): rounds the request down to the nearest supported one.
 * Same technique as the tutorial doca_flow_ecn.c / DOCA flow_random sample.
 */
static uint16_t get_random_mask(double percentage)
{
	double next_supported = 50.0;
	uint8_t i;

	for (i = 1; i <= RANDOM_FIELD_WIDTH; ++i) {
		if (percentage >= next_supported)
			break;
		next_supported /= 2;
	}
	if (percentage > next_supported)
		DOCA_LOG_WARN("Requested %.4g%% not supported (power-of-2 only); using %.4g%%", percentage,
			      next_supported);
	return (uint16_t)((1u << i) - 1);
}

/*
 * RANDOM_SAMPLE (non-root): match only parser_meta.random with a mask, so a
 * power-of-two fraction of a path's packets reaches hit_pipe (the mark pipe)
 * and the rest go to miss_pipe (clear and deliver unmarked). Used per
 * path when 0 < percent < 100. Combining a random match with a header-modify
 * action in one HWS entry is not supported, hence a separate action-free pipe.
 */
static struct doca_flow_pipe *create_random_sample_pipe(struct doca_flow_port *port, const char *name,
							struct doca_flow_pipe *hit_pipe,
							struct doca_flow_pipe *miss_pipe, uint16_t random_mask)
{
	struct doca_flow_match match = {0};
	struct doca_flow_match match_mask = {0};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = hit_pipe};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = miss_pipe};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	struct entry_batch_status status = {0};
	doca_error_t err;

	/* The 3.x egress HASH consumes random bit 0, so its independent ingress
	 * sampling masks start at bit 1. The DOCA 2.x HASH API does not share it. */
#if STEER_USE_RANDOM_HASH_CLASSIFIER
	random_mask = (uint16_t)(random_mask << 1);
#endif
	match.parser_meta.random = 0;
	match_mask.parser_meta.random = random_mask;

	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (%s)", name);
	err = doca_flow_pipe_cfg_set_name(cfg, name);
	crash_if_unsuccessful(err, "pipe_cfg_set_name (%s)", name);
	err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
	crash_if_unsuccessful(err, "pipe_cfg_set_type (%s)", name);
	err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	crash_if_unsuccessful(err, "pipe_cfg_set_domain (%s)", name);
	err = doca_flow_pipe_cfg_set_is_root(cfg, false);
	crash_if_unsuccessful(err, "pipe_cfg_set_is_root (%s)", name);
	err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
	crash_if_unsuccessful(err, "pipe_cfg_set_nr_entries (%s)", name);
	err = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
	crash_if_unsuccessful(err, "pipe_cfg_set_match (%s)", name);

	err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
	crash_if_unsuccessful(err, "pipe_create (%s)", name);
	doca_flow_pipe_cfg_destroy(cfg);

	err = steer_pipe_add_entry(0, pipe, &match, 0, NULL, NULL, NULL, 0, &status, &entry);
	crash_if_unsuccessful(err, "pipe_add_entry (%s)", name);
	process_entries(port, &status, 1, name);
	DOCA_LOG_INFO("%s ready: mask 0x%04x", name, random_mask);
	return pipe;
}

#if DOCA_USES_LEGACY_FLOW_BACKEND
/* DOCA 2.x random HASH is immutable after creation. Each bucket writes its
 * index to scratch metadata; a downstream BASIC dispatch pipe owns the
 * changeable path forwarding. */
static struct doca_flow_pipe *create_legacy_small_random_table(
	struct doca_flow_port *port, struct doca_flow_pipe *dispatch_target,
	uint8_t random_bits, struct doca_flow_pipe_entry *bucket_entry[LEGACY_RANDOM_BUCKETS])
{
	const uint32_t nr_entries = 1u << random_bits;
	struct doca_flow_match match_mask = {0};
	struct doca_flow_actions set_bucket = {0};
	struct doca_flow_actions *actions_arr[1] = {&set_bucket};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = dispatch_target};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	doca_error_t err;

	match_mask.parser_meta.random = UINT16_MAX;
	set_bucket.meta.u32[4] = UINT32_MAX;
	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (legacy random hash)");
	err = doca_flow_pipe_cfg_set_name(cfg, "EGRESS_RANDOM_PATH_HASH_6BIT");
	crash_if_unsuccessful(err, "pipe_cfg_set_name (legacy random hash)");
	err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_HASH);
	crash_if_unsuccessful(err, "pipe_cfg_set_type (legacy random hash)");
	err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	crash_if_unsuccessful(err, "pipe_cfg_set_domain (legacy random hash)");
	err = doca_flow_pipe_cfg_set_is_root(cfg, false);
	crash_if_unsuccessful(err, "pipe_cfg_set_is_root (legacy random hash)");
	err = doca_flow_pipe_cfg_set_nr_entries(cfg, nr_entries);
	crash_if_unsuccessful(err, "pipe_cfg_set_nr_entries (legacy random hash)");
	err = doca_flow_pipe_cfg_set_match(cfg, NULL, &match_mask);
	crash_if_unsuccessful(err, "pipe_cfg_set_match (legacy random hash)");
	err = doca_flow_pipe_cfg_set_actions(cfg, actions_arr, NULL, NULL, 1);
	crash_if_unsuccessful(err, "pipe_cfg_set_actions (legacy bucket metadata)");
	err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
	crash_if_unsuccessful(err, "pipe_create (legacy random hash)");
	doca_flow_pipe_cfg_destroy(cfg);

	memset(&g_classify_batch, 0, sizeof(g_classify_batch));
	for (uint32_t bucket = 0; bucket < nr_entries; bucket++) {
		struct doca_flow_actions actions = {0};
		uint32_t flags = bucket + 1 < nr_entries ? STEER_WAIT_FOR_BATCH : 0;

		actions.meta.u32[4] = RTE_BE32(bucket);
		err = steer_pipe_hash_add_entry(0, pipe, bucket, 0, &actions, NULL, NULL,
			flags, &g_classify_batch, &bucket_entry[bucket]);
		crash_if_unsuccessful(err, "pipe_hash_add_entry (legacy random bucket %u)", bucket);
	}
	process_entries(port, &g_classify_batch, nr_entries, "legacy random hash entries");
	DOCA_LOG_INFO("Legacy random HASH ready: %u bits, %u immutable metadata buckets",
		random_bits, nr_entries);
	return pipe;
}
#endif

/*
 * INGRESS_PATH_DEMUX (non-root): classify looped-back wire ingress by the DSCP
 * path bit (written at egress) into two independent path-specific destination-IP markers.
 * Non-IPv4 misses go to the shared clear-and-deliver pipe.
 */
static struct doca_flow_pipe *create_path_demux_pipe(struct doca_flow_port *port,
						     struct doca_flow_pipe *target[NB_PATHS],
						     struct doca_flow_pipe *clear_pipe,
						     struct doca_flow_pipe_entry *path_entries[NB_PATHS])
{
	struct doca_flow_match match = {0};
	struct doca_flow_match match_mask = {0};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = clear_pipe};
	struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
	struct doca_flow_pipe_cfg *cfg_pipe;
	struct doca_flow_pipe *pipe;
	doca_error_t err;

	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.dscp_ecn = 0xFF;		/* changeable: path bit value is per entry */
	match_mask.outer.ip4.dscp_ecn = PATH_DSCP_MASK; /* match only the path bit */

	err = doca_flow_pipe_cfg_create(&cfg_pipe, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (path demux)");
	err = doca_flow_pipe_cfg_set_name(cfg_pipe, "INGRESS_PATH_DEMUX");
	crash_if_unsuccessful(err, "pipe_cfg_set_name (path demux)");
	err = doca_flow_pipe_cfg_set_type(cfg_pipe, DOCA_FLOW_PIPE_BASIC);
	crash_if_unsuccessful(err, "pipe_cfg_set_type (path demux)");
	err = doca_flow_pipe_cfg_set_domain(cfg_pipe, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	crash_if_unsuccessful(err, "pipe_cfg_set_domain (path demux)");
	err = doca_flow_pipe_cfg_set_is_root(cfg_pipe, false);
	crash_if_unsuccessful(err, "pipe_cfg_set_is_root (path demux)");
	err = doca_flow_pipe_cfg_set_nr_entries(cfg_pipe, NB_PATHS);
	crash_if_unsuccessful(err, "pipe_cfg_set_nr_entries (path demux)");
	err = doca_flow_pipe_cfg_set_match(cfg_pipe, &match, &match_mask);
	crash_if_unsuccessful(err, "pipe_cfg_set_match (path demux)");
	err = doca_flow_pipe_cfg_set_monitor(cfg_pipe, &monitor);
	crash_if_unsuccessful(err, "pipe_cfg_set_monitor (path demux)");

	err = doca_flow_pipe_create(cfg_pipe, &fwd, &fwd_miss, &pipe);
	crash_if_unsuccessful(err, "pipe_create (path demux)");
	doca_flow_pipe_cfg_destroy(cfg_pipe);

	struct entry_batch_status status = {0};

	for (int i = 0; i < NB_PATHS; i++) {
		struct doca_flow_match entry_match = {0};
		struct doca_flow_fwd entry_fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = target[i]};
		uint32_t flags = (i == NB_PATHS - 1) ? 0 : STEER_WAIT_FOR_BATCH;

		entry_match.outer.ip4.dscp_ecn = PATH_DSCP_VAL(i);
		err = steer_pipe_add_entry(0, pipe, &entry_match, 0, NULL, &monitor, &entry_fwd,
					   flags, &status, &path_entries[i]);
		crash_if_unsuccessful(err, "pipe_add_entry (path demux %d)", i);
	}
	process_entries(port, &status, NB_PATHS, "path demux entries");
	DOCA_LOG_INFO("Path demux ready: path0->PATH0_IP_MATCH path1->PATH1_IP_MATCH by DSCP bit 0x%02x",
	              PATH_DSCP_MASK);
	return pipe;
}

/* Fixed path action. The separate action pipe is valid on both backends and
 * follows the validated ECN tutorial encoding.
 * Keep this identical to the validated ECN tutorial:
 * one fixed full-byte action and one entry. EGRESS_CLASSIFY is action-free and
 * chooses between the two pipes with a changeable per-bucket forward. */
static struct doca_flow_pipe *create_path_rewrite_pipe(struct doca_flow_port *port, uint8_t path,
						       struct doca_flow_pipe *next_pipe,
						       struct doca_flow_pipe_entry **entry_out)
{
	struct doca_flow_match match = {0}, match_mask = {0};
	struct doca_flow_actions actions = {0};
	struct doca_flow_actions *actions_arr[1] = {&actions};
	struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = next_pipe};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	struct entry_batch_status status = {0};
	char name[32];
	doca_error_t err;

	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.dscp_ecn = UINT8_MAX;
	match_mask.outer.ip4.dscp_ecn = 0;
	actions.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions.outer.ip4.dscp_ecn = UINT8_MAX;
	snprintf(name, sizeof(name), "EGRESS_PATH%u_REWRITE", path);

	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_name(cfg, name),
	                      "pipe_cfg_set_name (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC),
	                      "pipe_cfg_set_type (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT),
	                      "pipe_cfg_set_domain (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_is_root(cfg, false),
	                      "pipe_cfg_set_is_root (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_nr_entries(cfg, 1),
	                      "pipe_cfg_set_nr_entries (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask),
	                      "pipe_cfg_set_match (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_actions(cfg, actions_arr, NULL, NULL, 1),
	                      "pipe_cfg_set_actions (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_monitor(cfg, &monitor),
	                      "pipe_cfg_set_monitor (%s)", name);
	err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
	crash_if_unsuccessful(err, "pipe_create (%s)", name);
	doca_flow_pipe_cfg_destroy(cfg);

	struct doca_flow_match entry_match = {0};
	struct doca_flow_actions entry_actions = {0};
	entry_actions.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	entry_actions.outer.ip4.dscp_ecn = PATH_DSCP_VAL(path) | IP4_ECN_ECT0;
	err = steer_pipe_add_entry(0, pipe, &entry_match, 0, &entry_actions, &monitor, NULL, 0,
	                           &status, entry_out != NULL ? entry_out : &entry);
	crash_if_unsuccessful(err, "pipe_add_entry (%s)", name);
	process_entries(port, &status, 1, name);
	DOCA_LOG_INFO("%s ready: fixed dscp_ecn=0x%02x", name,
	              entry_actions.outer.ip4.dscp_ecn);
	return pipe;
}

/* Metadata update stage. RANDOM HASH writes its selected index to application
 * scratch u32[4]; this single BASIC table maps that value to a changeable path
 * forward. HASH internally uses part of u32[3], so u32[4] is intentional. */
static struct doca_flow_pipe *create_classify_dispatch_pipe(
	struct doca_flow_port *port, struct doca_flow_pipe *path_target[NB_PATHS],
	struct doca_flow_pipe_entry *entries[PATH_SHARE_BUCKETS],
	uint8_t bucket_path[PATH_SHARE_BUCKETS], int force_path)
{
	struct doca_flow_match match = {0}, match_mask = {0};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	doca_error_t err;

	match.meta.u32[4] = UINT32_MAX;
	match_mask.meta.u32[4] = UINT32_MAX;
	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (classify dispatch)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_name(cfg, "EGRESS_BUCKET_DISPATCH"),
	                      "pipe_cfg_set_name (classify dispatch)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC),
	                      "pipe_cfg_set_type (classify dispatch)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT),
	                      "pipe_cfg_set_domain (classify dispatch)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_is_root(cfg, false),
	                      "pipe_cfg_set_is_root (classify dispatch)");
	/* Entry replacement temporarily needs a free rule index. */
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_nr_entries(cfg, PATH_SHARE_BUCKETS * 2),
	                      "pipe_cfg_set_nr_entries (classify dispatch)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask),
	                      "pipe_cfg_set_match (classify dispatch)");
	err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
	crash_if_unsuccessful(err, "pipe_create (classify dispatch)");
	doca_flow_pipe_cfg_destroy(cfg);

	memset(&g_classify_batch, 0, sizeof(g_classify_batch));
	for (uint32_t idx = 0; idx < PATH_SHARE_BUCKETS; idx++) {
		struct doca_flow_match entry_match = {0};
		uint8_t path = force_path >= 0 ? (uint8_t)force_path
		                                                  : (idx < PATH_SHARE_BUCKETS / 2 ? 0 : 1);
		struct doca_flow_fwd entry_fwd = {
			.type = DOCA_FLOW_FWD_PIPE,
			.next_pipe = path_target[path],
		};
		uint32_t flags = idx == PATH_SHARE_BUCKETS - 1 ? 0 : STEER_WAIT_FOR_BATCH;

		entry_match.meta.u32[4] = RTE_BE32(idx);
		err = steer_pipe_add_entry(0, pipe, &entry_match, 0, NULL, NULL, &entry_fwd,
		                           flags, &g_classify_batch, &entries[idx]);
		crash_if_unsuccessful(err, "pipe_add_entry (dispatch bucket=%u)", idx);
		bucket_path[idx] = path;
	}
	process_entries(port, &g_classify_batch, PATH_SHARE_BUCKETS, "classify dispatch entries");
	DOCA_LOG_INFO("EGRESS_BUCKET_DISPATCH ready: meta.u32[4] bucket -> changeable path forward");
	return pipe;
}

/* DOCA 3.x EGRESS_CLASSIFY: native RANDOM HASH selects one of 64 persistent
 * buckets and writes its index to metadata for the shared dispatch stage. */
#if STEER_USE_RANDOM_HASH_CLASSIFIER
static struct doca_flow_pipe *create_classify_pipe(struct doca_flow_port *port,
                                                    struct doca_flow_pipe *dispatch_target)
{
	struct doca_flow_actions set_bucket = {0};
	struct doca_flow_actions *actions_arr[1] = {&set_bucket};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = dispatch_target};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	doca_error_t err;

	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (classify)");
	err = doca_flow_pipe_cfg_set_name(cfg, "EGRESS_CLASSIFY");
	crash_if_unsuccessful(err, "pipe_cfg_set_name (classify)");
	err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_HASH);
	crash_if_unsuccessful(err, "pipe_cfg_set_type (classify)");
	err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	crash_if_unsuccessful(err, "pipe_cfg_set_domain (classify)");
	err = doca_flow_pipe_cfg_set_is_root(cfg, false);
	crash_if_unsuccessful(err, "pipe_cfg_set_is_root (classify)");
	err = doca_flow_pipe_cfg_set_nr_entries(cfg, PATH_SHARE_BUCKETS);
	crash_if_unsuccessful(err, "pipe_cfg_set_nr_entries (classify)");
	set_bucket.meta.u32[4] = UINT32_MAX;
	err = doca_flow_pipe_cfg_set_hash_map_algorithm(
		cfg, DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_RANDOM);
	crash_if_unsuccessful(err, "pipe_cfg_set_hash_map_algorithm (classify random)");
	err = doca_flow_pipe_cfg_set_actions(cfg, actions_arr, NULL, NULL, 1);
	crash_if_unsuccessful(err, "pipe_cfg_set_actions (classify bucket metadata)");
	err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
	crash_if_unsuccessful(err, "pipe_create (classify)");
	doca_flow_pipe_cfg_destroy(cfg);
	return pipe;
}

static void add_classify_entries(struct doca_flow_pipe *pipe, struct doca_flow_port *port)
{
	doca_error_t err;

	memset(&g_classify_batch, 0, sizeof(g_classify_batch));
	for (uint32_t idx = 0; idx < PATH_SHARE_BUCKETS; idx++) {
		struct doca_flow_actions actions = {0};
		struct doca_flow_pipe_entry *hash_entry;
		uint32_t flags = idx == PATH_SHARE_BUCKETS - 1 ? 0 : STEER_WAIT_FOR_BATCH;

		actions.meta.u32[4] = RTE_BE32(idx);
		err = steer_pipe_hash_add_entry(0, pipe, idx, 0, &actions, NULL, NULL,
		                                      flags, &g_classify_batch, &hash_entry);
		crash_if_unsuccessful(err, "pipe_hash_add_entry (classify bucket=%u)", idx);
	}
	process_entries(port, &g_classify_batch, PATH_SHARE_BUCKETS, "classify hash entries");
	DOCA_LOG_INFO("Classify random-bucket pipe ready: HASH/random writes bucket to meta.u32[4]");
}
#endif

/* EGRESS_ROCE_CHECK (non-root): admit only IPv4 RoCEv2 on UDP 4791 to
 * the random path classifier. All other SF-egress traffic bypasses
 * the random-bucket pipe and is delivered to the wire unchanged. */
static struct doca_flow_pipe *create_roce_check_pipe(struct doca_flow_port *port, const char *name,
                                                     struct doca_flow_pipe *roce_target,
                                                     struct doca_flow_pipe *bypass_target,
						     bool roce_target_is_random_hash)
{
	struct doca_flow_match match = {0};
	struct doca_flow_match match_mask = {0};
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = bypass_target};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	struct entry_batch_status status = {0};
	doca_error_t err;

	if (roce_target_is_random_hash) {
#if STEER_HAS_HASH_FWD
		fwd.type = DOCA_FLOW_FWD_HASH_PIPE;
		fwd.hash_pipe.pipe = roce_target;
		fwd.hash_pipe.algorithm = DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_RANDOM;
#else
		crash_if_unsuccessful(DOCA_ERROR_NOT_SUPPORTED, "random HASH forward on DOCA 2.x");
#endif
	} else {
		fwd.type = DOCA_FLOW_FWD_PIPE;
		fwd.next_pipe = roce_target;
	}

	steer_set_roce_udp_match(&match, &match_mask, RTE_BE16(ROCE_UDP_PORT_NATIVE));

	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (RoCE check)");
	err = doca_flow_pipe_cfg_set_name(cfg, name);
	crash_if_unsuccessful(err, "pipe_cfg_set_name (RoCE check)");
	err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
	crash_if_unsuccessful(err, "pipe_cfg_set_type (RoCE check)");
	err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	crash_if_unsuccessful(err, "pipe_cfg_set_domain (RoCE check)");
	err = doca_flow_pipe_cfg_set_is_root(cfg, false);
	crash_if_unsuccessful(err, "pipe_cfg_set_is_root (RoCE check)");
	err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
	crash_if_unsuccessful(err, "pipe_cfg_set_nr_entries (RoCE check)");
	err = doca_flow_pipe_cfg_set_match(cfg, &match,
	                                  steer_roce_udp_match_mask(&match_mask));
	crash_if_unsuccessful(err, "pipe_cfg_set_match (RoCE check)");

	err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
	crash_if_unsuccessful(err, "pipe_create (RoCE check)");
	doca_flow_pipe_cfg_destroy(cfg);

	struct doca_flow_match entry_match = {0};
#if !STEER_HAS_ROCE_MATCH
	/* Fixed DOCA 2.x template fields must also be present on the entry. */
	entry_match = match;
#endif

	err = steer_pipe_add_entry(0, pipe, &entry_match, 0, NULL, NULL, NULL, 0, &status, &entry);
	crash_if_unsuccessful(err, "pipe_add_entry (RoCE check)");
	process_entries(port, &status, 1, "RoCE check entry");
	DOCA_LOG_INFO("%s ready: IPv4 RoCEv2 UDP %u admitted; other traffic bypasses", name,
	              ROCE_UDP_PORT_NATIVE);
	return pipe;
}

#if DOCA_HAS_DEVICE_REPRESENTORS
/* Shared terminal software target for QP1 clones from both directions. */
static struct doca_flow_pipe *create_qp1_rss_pipe(struct doca_flow_port *port, const char *name,
						  uint16_t queue)
{
	struct doca_flow_match match = {0}, match_mask = {0}, entry_match = {0};
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	struct entry_batch_status status = {0};
	uint16_t queues[1] = {queue};
	doca_error_t err;

	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.dscp_ecn = 0xFF;
	match_mask.outer.ip4.dscp_ecn = 0;
	steer_fwd_set_rss(&fwd, queues, 1, DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP);

	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_name(cfg, name), "pipe_cfg_set_name (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC), "pipe_cfg_set_type (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT), "pipe_cfg_set_domain (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_is_root(cfg, false), "pipe_cfg_set_is_root (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_nr_entries(cfg, 1), "pipe_cfg_set_nr_entries (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask), "pipe_cfg_set_match (%s)", name);
	err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
	crash_if_unsuccessful(err, "pipe_create (%s)", name);
	doca_flow_pipe_cfg_destroy(cfg);
	err = steer_pipe_add_entry(0, pipe, &entry_match, 0, NULL, NULL, NULL, 0, &status, &entry);
	crash_if_unsuccessful(err, "pipe_add_entry (%s)", name);
	process_entries(port, &status, 1, name);
	return pipe;
}

/* FLOODING hash duplicates every hit to the normal hardware path and to the
 * shared RSS terminal. */
static struct doca_flow_pipe *create_qp1_flood_pipe(struct doca_flow_port *port, const char *name,
						    struct doca_flow_pipe *normal_target,
						    struct doca_flow_pipe *rss_target)
{
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = NULL};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	struct entry_batch_status status = {0};
	doca_error_t err;

	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_name(cfg, name), "pipe_cfg_set_name (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_HASH), "pipe_cfg_set_type (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT), "pipe_cfg_set_domain (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_is_root(cfg, false), "pipe_cfg_set_is_root (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_nr_entries(cfg, 2), "pipe_cfg_set_nr_entries (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_hash_map_algorithm(cfg, DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING),
	                      "pipe_cfg_set_hash_map_algorithm (%s)", name);
	err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
	crash_if_unsuccessful(err, "pipe_create (%s)", name);
	doca_flow_pipe_cfg_destroy(cfg);
	for (uint32_t i = 0; i < 2; i++) {
		struct doca_flow_fwd entry_fwd = {
			.type = DOCA_FLOW_FWD_PIPE,
			.next_pipe = i == 0 ? normal_target : rss_target,
		};
		struct doca_flow_pipe_entry *entry;
		uint32_t flags = i == 0 ? STEER_WAIT_FOR_BATCH : 0;
		err = steer_pipe_hash_add_entry(0, pipe, i, 0, NULL, NULL, &entry_fwd, flags,
		                                  &status, &entry);
		crash_if_unsuccessful(err, "pipe_hash_add_entry (%s clone=%u)", name, i);
	}
	process_entries(port, &status, 2, name);
	return pipe;
}

/* Match IPv4 RoCEv2 management packets addressed to QP1. Hits enter the
 * flooding clone pipe; misses continue through the existing direction path. */
static struct doca_flow_pipe *create_qp1_clone_check(struct doca_flow_port *port, const char *name,
						      struct doca_flow_pipe *flood_target,
						      struct doca_flow_pipe *miss_target)
{
	struct doca_flow_match match = {0}, match_mask = {0}, entry_match = {0};
	struct doca_flow_fwd fwd = {
		.type = DOCA_FLOW_FWD_HASH_PIPE,
		.hash_pipe = {
			.pipe = flood_target,
			.algorithm = DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING,
		},
	};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = miss_target};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	struct entry_batch_status status = {0};
	doca_error_t err;

	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_ROCE_V2;
	memset(match.outer.roce_v2.bth.dest_qp, 0xFF, sizeof(match.outer.roce_v2.bth.dest_qp));
	memset(match_mask.outer.roce_v2.bth.dest_qp, 0xFF, sizeof(match_mask.outer.roce_v2.bth.dest_qp));
	entry_match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	entry_match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_ROCE_V2;
	entry_match.outer.roce_v2.bth.dest_qp[2] = QP1_QPN;

	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_name(cfg, name), "pipe_cfg_set_name (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC), "pipe_cfg_set_type (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT), "pipe_cfg_set_domain (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_is_root(cfg, false), "pipe_cfg_set_is_root (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_nr_entries(cfg, 1), "pipe_cfg_set_nr_entries (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask), "pipe_cfg_set_match (%s)", name);
	err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
	crash_if_unsuccessful(err, "pipe_create (%s)", name);
	doca_flow_pipe_cfg_destroy(cfg);
	err = steer_pipe_add_entry(0, pipe, &entry_match, 0, NULL, NULL, NULL, 0, &status, &entry);
	crash_if_unsuccessful(err, "pipe_add_entry (%s)", name);
	process_entries(port, &status, 1, name);
	DOCA_LOG_INFO("%s ready: QP1 normal-forward + DPDK clone", name);
	return pipe;
}

#endif /* DOCA_HAS_DEVICE_REPRESENTORS */

#if DOCA_USES_LEGACY_FLOW_BACKEND
/* A DOCA 2 shared mirror cannot target RSS directly. Its clone first enters
 * this BASIC source-IP filter; matching path feedback reaches RSS queue 0 and
 * misses drop only the clone. */
static struct doca_flow_pipe *create_legacy_qp1_rss_pipe(
	struct doca_flow_port *port, const uint32_t path_ip[NB_PATHS],
	struct doca_flow_pipe_entry *path_entry[NB_PATHS])
{
	struct doca_flow_match match = {0}, match_mask = {0};
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_DROP};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	struct entry_batch_status status = {0};
	uint16_t queues[1] = {QP1_CLONE_QUEUE};
	doca_error_t err;

	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = UINT32_MAX;
	match_mask.outer.ip4.src_ip = UINT32_MAX;
	steer_fwd_set_rss(&fwd, queues, 1, DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP);
	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (QP1_RX_FILTER)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_name(cfg, "QP1_RX_FILTER"),
	                      "pipe_cfg_set_name (QP1_RX_FILTER)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC),
	                      "pipe_cfg_set_type (QP1_RX_FILTER)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT),
	                      "pipe_cfg_set_domain (QP1_RX_FILTER)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_is_root(cfg, false),
	                      "pipe_cfg_set_is_root (QP1_RX_FILTER)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_dir_info(
		cfg, DOCA_FLOW_DIRECTION_BIDIRECTIONAL),
		"pipe_cfg_set_dir_info (QP1_RX_FILTER)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_nr_entries(cfg, NB_PATHS),
	                      "pipe_cfg_set_nr_entries (QP1_RX_FILTER)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask),
	                      "pipe_cfg_set_match (QP1_RX_FILTER)");
	err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
	crash_if_unsuccessful(err, "pipe_create (QP1_RX_FILTER)");
	doca_flow_pipe_cfg_destroy(cfg);

	for (uint8_t path = 0; path < NB_PATHS; path++) {
		struct doca_flow_match entry_match = match;
		uint32_t flags = path + 1 < NB_PATHS ? STEER_WAIT_FOR_BATCH : 0;

		entry_match.outer.ip4.src_ip = path_ip[path];
		err = steer_pipe_add_entry(0, pipe, &entry_match, 0, NULL, NULL, NULL,
		                           flags, &status, &path_entry[path]);
		crash_if_unsuccessful(err, "pipe_add_entry (QP1_RX_FILTER path%u)", path);
	}
	process_entries(port, &status, NB_PATHS, "QP1_RX_FILTER");
	DOCA_LOG_INFO("QP1_RX_FILTER ready: post-clone path source-IP hits -> RSS; miss -> drop");
	return pipe;
}

static void configure_legacy_mirror(struct doca_flow_port *port, uint32_t mirror_id,
                                    const struct doca_flow_fwd *clone_fwd,
                                    const struct doca_flow_fwd *original_fwd)
{
	struct doca_flow_mirror_target target = {.fwd = *clone_fwd};
	struct doca_flow_shared_resource_cfg cfg = {0};
	doca_error_t err;

	cfg.domain = DOCA_FLOW_PIPE_DOMAIN_DEFAULT;
	cfg.mirror_cfg.nr_targets = 1;
	cfg.mirror_cfg.target = &target;
	steer_mirror_set_original_fwd(&cfg, original_fwd);
	err = steer_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_MIRROR, mirror_id, &cfg);
	crash_if_unsuccessful(err, "doca_flow_shared_resource_set_cfg (mirror %u)", mirror_id);
	err = doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_MIRROR, &mirror_id, 1, port);
	crash_if_unsuccessful(err, "doca_flow_shared_resources_bind (mirror %u)", mirror_id);
}

static struct doca_flow_pipe *create_legacy_roce_mirror_pipe(
	struct doca_flow_port *port, const char *name, uint16_t original_port,
	struct doca_flow_pipe *miss_target, uint32_t mirror_id,
	struct doca_flow_pipe_entry *path_entry[NB_PATHS])
{
	struct doca_flow_match match = {0}, match_mask = {0};
	struct doca_flow_monitor monitor = {.shared_mirror_id = mirror_id};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = original_port};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = miss_target};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	struct entry_batch_status status = {0};
	doca_error_t err;

	steer_set_roce_udp_match(&match, &match_mask, RTE_BE16(ROCE_UDP_PORT_NATIVE));
	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_name(cfg, name), "pipe_cfg_set_name (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC), "pipe_cfg_set_type (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT), "pipe_cfg_set_domain (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_is_root(cfg, false), "pipe_cfg_set_is_root (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_nr_entries(cfg, 1), "pipe_cfg_set_nr_entries (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_match(
		cfg, &match, steer_roce_udp_match_mask(&match_mask)), "pipe_cfg_set_match (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_monitor(cfg, &monitor), "pipe_cfg_set_monitor (%s)", name);
	err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
	crash_if_unsuccessful(err, "pipe_create (%s)", name);
	doca_flow_pipe_cfg_destroy(cfg);

	{
		struct doca_flow_match entry_match = {0};
		struct doca_flow_monitor entry_monitor = monitor;
#if !STEER_HAS_ROCE_MATCH
		entry_match = match;
#endif
		err = steer_pipe_add_entry(0, pipe, &entry_match, 0, NULL, &entry_monitor, NULL,
			0, &status, &path_entry[0]);
		crash_if_unsuccessful(err, "pipe_add_entry (%s)", name);
	}
	path_entry[1] = NULL;
	process_entries(port, &status, 1, name);
	DOCA_LOG_INFO("%s ready: all wire-ingress RoCEv2 packets cloned", name);
	return pipe;
}
#endif

/* Install QP1 observation through backend-specific interfaces so each path
 * receives only the Flow objects and state it owns. */
#if DOCA_HAS_DEVICE_REPRESENTORS
static void install_native_qp1_clone_paths(struct doca_flow_port *port,
                                            struct doca_flow_pipe *deliver_sf,
                                            struct doca_flow_pipe *deliver_wire,
                                            struct doca_flow_pipe **wire_target,
                                            struct doca_flow_pipe **sf_target)
{
	struct doca_flow_pipe *qp1_rss = create_qp1_rss_pipe(port, "QP1_RSS", QP1_CLONE_QUEUE);
	struct doca_flow_pipe *wire_flood =
		create_qp1_flood_pipe(port, "QP1_FLOOD_WIRE", deliver_sf, qp1_rss);
	*wire_target = create_qp1_clone_check(port, "QP1_CHECK_WIRE", wire_flood, *wire_target);

	struct doca_flow_pipe *sf_flood =
		create_qp1_flood_pipe(port, "QP1_FLOOD_SF", deliver_wire, qp1_rss);
	*sf_target = create_qp1_clone_check(port, "QP1_CHECK_SF", sf_flood, *sf_target);
}
#else
static void install_legacy_qp1_clone_path(
	struct doca_flow_port *port, struct doca_flow_pipe **wire_target,
	const uint32_t path_ip[NB_PATHS],
	struct doca_flow_pipe_entry *wire_entry[NB_PATHS],
	struct doca_flow_pipe_entry *filter_entry[NB_PATHS])
{
	struct doca_flow_pipe *qp1_rss =
		create_legacy_qp1_rss_pipe(port, path_ip, filter_entry);
	struct doca_flow_fwd clone_fwd = {
		.type = DOCA_FLOW_FWD_PIPE,
		.next_pipe = qp1_rss,
	};
	/* DOCA 2.7 requires an explicit original destination on the shared
	 * mirror. QP1 management packets use the terminal SF port so connection
	 * establishment does not depend on the egress rewrite chain. */
	struct doca_flow_fwd wire_original = {
		.type = DOCA_FLOW_FWD_PORT,
		.port_id = SF_PORT_ID,
	};

	configure_legacy_mirror(port, QP1_WIRE_MIRROR_ID, &clone_fwd, &wire_original);
	*wire_target = create_legacy_roce_mirror_pipe(
		port, "QP1_MIRROR_WIRE", SF_PORT_ID, *wire_target,
		QP1_WIRE_MIRROR_ID, wire_entry);
	DOCA_LOG_WARN("DOCA 2.x clones all wire-ingress UDP 4791; "
	              "ACK/CNP destination QPN and source IP identify sender QPN and path");
}
#endif

#if DOCA_HAS_DEVICE_REPRESENTORS
/* Explicit ARP handling: wire requests reach both receiver SFs and replies
 * from either SF reach wire, independent of IPv4/default-miss behavior. */
static struct doca_flow_pipe *create_arp_flood_pipe(struct doca_flow_port *port)
{
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = NULL};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	struct entry_batch_status status = {0};
	doca_error_t err = doca_flow_pipe_cfg_create(&cfg, port);

	crash_if_unsuccessful(err, "pipe_cfg_create (ARP flood)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_name(cfg, "ARP_FLOOD_TO_SFS"), "pipe_cfg_set_name (ARP flood)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_HASH), "pipe_cfg_set_type (ARP flood)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT), "pipe_cfg_set_domain (ARP flood)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_is_root(cfg, false), "pipe_cfg_set_is_root (ARP flood)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_nr_entries(cfg, NB_PATHS), "pipe_cfg_set_nr_entries (ARP flood)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_hash_map_algorithm(cfg, DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING),
	                      "pipe_cfg_set_hash_map_algorithm (ARP flood)");
	err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
	crash_if_unsuccessful(err, "pipe_create (ARP flood)");
	doca_flow_pipe_cfg_destroy(cfg);

	for (uint32_t path = 0; path < NB_PATHS; path++) {
		struct doca_flow_fwd entry_fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = SF_PORT_ID + path};
		struct doca_flow_pipe_entry *entry;
		uint32_t flags = path == 0 ? STEER_WAIT_FOR_BATCH : 0;

		err = steer_pipe_hash_add_entry(0, pipe, path, 0, NULL, NULL, &entry_fwd, flags, &status, &entry);
		crash_if_unsuccessful(err, "pipe_hash_add_entry (ARP flood path=%u)", path);
	}
	process_entries(port, &status, NB_PATHS, "ARP flood entries");
	return pipe;
}

#endif /* DOCA_HAS_DEVICE_REPRESENTORS */

static struct doca_flow_pipe *create_arp_check_pipe(struct doca_flow_port *port, const char *name,
						     const struct doca_flow_fwd *arp_fwd,
                                                     struct doca_flow_pipe *miss_target,
                                                     uint32_t mirror_id)
{
	struct doca_flow_match match = {0}, match_mask = {0}, entry_match = {0};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = miss_target};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	struct entry_batch_status status = {0};
#if DOCA_USES_LEGACY_FLOW_BACKEND
	struct doca_flow_monitor monitor = {.shared_mirror_id = mirror_id};
	struct doca_flow_monitor entry_monitor = monitor;
#endif
	doca_error_t err;

	match.outer.eth.type = UINT16_MAX;
	match_mask.outer.eth.type = UINT16_MAX;
	entry_match.outer.eth.type = RTE_BE16(RTE_ETHER_TYPE_ARP);
	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_name(cfg, name), "pipe_cfg_set_name (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC), "pipe_cfg_set_type (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT), "pipe_cfg_set_domain (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_is_root(cfg, false), "pipe_cfg_set_is_root (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_nr_entries(cfg, 1), "pipe_cfg_set_nr_entries (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask), "pipe_cfg_set_match (%s)", name);
#if DOCA_USES_LEGACY_FLOW_BACKEND
	if (mirror_id != UINT32_MAX)
		crash_if_unsuccessful(doca_flow_pipe_cfg_set_monitor(cfg, &monitor),
		                      "pipe_cfg_set_monitor (%s)", name);
#else
	(void)mirror_id;
#endif
	err = doca_flow_pipe_create(cfg, arp_fwd, &fwd_miss, &pipe);
	crash_if_unsuccessful(err, "pipe_create (%s)", name);
	doca_flow_pipe_cfg_destroy(cfg);
#if DOCA_USES_LEGACY_FLOW_BACKEND
	err = steer_pipe_add_entry(0, pipe, &entry_match, 0, NULL, mirror_id == UINT32_MAX ? NULL : &entry_monitor, NULL, 0, &status, &entry);
#else
	err = steer_pipe_add_entry(0, pipe, &entry_match, 0, NULL, NULL, NULL, 0, &status, &entry);
#endif
	crash_if_unsuccessful(err, "pipe_add_entry (%s)", name);
	process_entries(port, &status, 1, name);
	return pipe;
}

static void install_arp_paths(struct doca_flow_port *port, struct doca_flow_pipe **wire_target,
			      struct doca_flow_pipe **sf_target)
{
#if DOCA_HAS_DEVICE_REPRESENTORS
	struct doca_flow_pipe *flood = create_arp_flood_pipe(port);
	struct doca_flow_fwd wire_fwd = {.type = DOCA_FLOW_FWD_HASH_PIPE,
		.hash_pipe = {.pipe = flood, .algorithm = DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING}};
	struct doca_flow_fwd sf_fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = WIRE_PORT_ID};

	*wire_target = create_arp_check_pipe(port, "ARP_CHECK_WIRE", &wire_fwd, *wire_target, UINT32_MAX);
	*sf_target = create_arp_check_pipe(port, "ARP_CHECK_SF", &sf_fwd, *sf_target, UINT32_MAX);
	DOCA_LOG_INFO("ARP steering ready: wire -> SF0+SF1; SF replies -> wire");
#else
	struct doca_flow_fwd clone_fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = SF_PATH1_PORT_ID};
	struct doca_flow_fwd path0_fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = SF_PORT_ID};
	struct doca_flow_fwd wire_fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = WIRE_PORT_ID};

	configure_legacy_mirror(port, ARP_MIRROR_ID, &clone_fwd, &path0_fwd);
	*wire_target = create_arp_check_pipe(port, "ARP_CHECK_WIRE", &path0_fwd,
	                                     *wire_target, ARP_MIRROR_ID);
	*sf_target = create_arp_check_pipe(port, "ARP_CHECK_SF", &wire_fwd,
	                                   *sf_target, UINT32_MAX);
	DOCA_LOG_INFO("ARP steering ready: wire -> SF0+SF1 via shared mirror; SF replies -> wire");
#endif
}

/*
 * PORT_DEMUX (root): demux by source vport.
 *   port_meta == SF_PORT_ID (sender egress) -> classify_pipe
 *   port_meta == 0          (wire ingress)  -> mark_pipe
 *   miss -> DROP
 */
static void create_port_demux_pipe(struct doca_flow_port *port, struct doca_flow_pipe *sf_target,
				   struct doca_flow_pipe *wire_target, uint32_t nb_sf_ports)
{
	struct doca_flow_match match = {0};
	struct doca_flow_match match_mask = {0};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_DROP};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	doca_error_t err;

	match.parser_meta.STEER_PARSER_PORT = STEER_PORT_ALL;
	match_mask.parser_meta.STEER_PARSER_PORT = STEER_PORT_ALL;

	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (demux)");
	err = doca_flow_pipe_cfg_set_name(cfg, "PORT_DEMUX");
	crash_if_unsuccessful(err, "pipe_cfg_set_name (demux)");
	err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
	crash_if_unsuccessful(err, "pipe_cfg_set_type (demux)");
	err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	crash_if_unsuccessful(err, "pipe_cfg_set_domain (demux)");
	err = doca_flow_pipe_cfg_set_is_root(cfg, true);
	crash_if_unsuccessful(err, "pipe_cfg_set_is_root (demux)");
	err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1 + nb_sf_ports);
	crash_if_unsuccessful(err, "pipe_cfg_set_nr_entries (demux)");
	err = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
	crash_if_unsuccessful(err, "pipe_cfg_set_match (demux)");

	err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
	crash_if_unsuccessful(err, "pipe_create (demux)");
	doca_flow_pipe_cfg_destroy(cfg);

	struct doca_flow_match entry_match = {0};
	struct doca_flow_fwd entry_fwd;
	struct entry_batch_status status = {0};
	struct doca_flow_pipe_entry *entry;

	/* wire ingress (port 0) -> wire_target (ingress path demux / mark chain) */
	entry_match.parser_meta.STEER_PARSER_PORT = WIRE_PORT_ID;
	memset(&entry_fwd, 0, sizeof(entry_fwd));
	entry_fwd.type = DOCA_FLOW_FWD_PIPE;
	entry_fwd.next_pipe = wire_target;
	err = steer_pipe_add_entry(0, pipe, &entry_match, 0, NULL, NULL, &entry_fwd, STEER_WAIT_FOR_BATCH, &status,
				   &entry);
	crash_if_unsuccessful(err, "pipe_add_entry (demux wire->target)");

	/* Every receiver SF egresses through the same wire-facing target. */
	for (uint32_t path = 0; path < nb_sf_ports; path++) {
		entry_match.parser_meta.STEER_PARSER_PORT = SF_PORT_ID + path;
		memset(&entry_fwd, 0, sizeof(entry_fwd));
		entry_fwd.type = DOCA_FLOW_FWD_PIPE;
		entry_fwd.next_pipe = sf_target;
		err = steer_pipe_add_entry(0, pipe, &entry_match, 0, NULL, NULL, &entry_fwd, 0,
					   &status, &entry);
		crash_if_unsuccessful(err, "pipe_add_entry (demux sf%u->target)", path);
	}

	process_entries(port, &status, 1 + nb_sf_ports, "demux entries");
	DOCA_LOG_INFO("Port demux ready: wire ingress and %u SF egress port(s) routed", nb_sf_ports);
}

/* CLI parsing lives in the standalone main (doca_flow_steer.c). */

/* Final ingress bypass action: remove only the private DSCP path marker and
 * preserve the packets existing ECN bits. Selected-class sampling misses and
 * destination-IP mismatches both terminate here. */
static struct doca_flow_pipe *create_clear_path_pipe(struct doca_flow_port *port,
					      struct doca_flow_pipe *deliver_sf,
					      struct doca_flow_pipe_entry **entry_out)
{
	struct doca_flow_match match = {0}, match_mask = {0}, entry_match = {0};
	struct doca_flow_actions actions = {0}, actions_mask = {0};
	struct doca_flow_actions *actions_arr[1] = {&actions};
	struct doca_flow_actions *actions_masks_arr[1] = {&actions_mask};
	struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = deliver_sf};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	struct entry_batch_status status = {0};
	doca_error_t err;

	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.dscp_ecn = 0xFF;
	match_mask.outer.ip4.dscp_ecn = 0; /* all IPv4 */
	actions.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions.outer.ip4.dscp_ecn = 0;
	actions_mask.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions_mask.outer.ip4.dscp_ecn = PATH_DSCP_MASK;

	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (clear path)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_name(cfg, "INGRESS_CLEAR_PATH"),
	                      "pipe_cfg_set_name (clear path)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC),
	                      "pipe_cfg_set_type (clear path)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT),
	                      "pipe_cfg_set_domain (clear path)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_is_root(cfg, false),
	                      "pipe_cfg_set_is_root (clear path)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_nr_entries(cfg, 1),
	                      "pipe_cfg_set_nr_entries (clear path)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask),
	                      "pipe_cfg_set_match (clear path)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_actions(cfg, actions_arr, actions_masks_arr, NULL, 1),
	                      "pipe_cfg_set_actions (clear path)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_monitor(cfg, &monitor),
	                      "pipe_cfg_set_monitor (clear path)");
	err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
	crash_if_unsuccessful(err, "pipe_create (clear path)");
	doca_flow_pipe_cfg_destroy(cfg);

	err = steer_pipe_add_entry(0, pipe, &entry_match, 0, &actions, &monitor, NULL, 0,
	                           &status, entry_out);
	crash_if_unsuccessful(err, "pipe_add_entry (clear path)");
	process_entries(port, &status, 1, "clear path entry");
	DOCA_LOG_INFO("Ingress clear-path ready: preserve ECN, clear DSCP path bit");
	return pipe;
}

/* Per-path selected-class marker. Packets reach this pipe only after that
 * path's destination-IP match and optional sampling hit. */
static struct doca_flow_pipe *create_selected_mark_pipe(struct doca_flow_port *port, int path,
						 struct doca_flow_pipe *deliver_sf,
						 struct doca_flow_pipe_entry **entry_out)
{
	struct doca_flow_match match = {0}, match_mask = {0}, entry_match = {0};
	struct doca_flow_actions actions = {0}, actions_mask = {0};
	struct doca_flow_actions *actions_arr[1] = {&actions};
	struct doca_flow_actions *actions_masks_arr[1] = {&actions_mask};
	struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = deliver_sf};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	struct entry_batch_status status = {0};
	char name[32];
	doca_error_t err;

	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.dscp_ecn = 0xFF;
	match_mask.outer.ip4.dscp_ecn = 0;
	actions.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions.outer.ip4.dscp_ecn = IP4_DSCP_ECN_CE;
	actions_mask.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions_mask.outer.ip4.dscp_ecn = PATH_DSCP_MASK | IP4_ECN_MASK;
	snprintf(name, sizeof(name), "PATH%d_CE_MARK", path);

	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_name(cfg, name), "pipe_cfg_set_name (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC), "pipe_cfg_set_type (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT), "pipe_cfg_set_domain (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_is_root(cfg, false), "pipe_cfg_set_is_root (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_nr_entries(cfg, 1), "pipe_cfg_set_nr_entries (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask), "pipe_cfg_set_match (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_actions(cfg, actions_arr, actions_masks_arr, NULL, 1),
	                      "pipe_cfg_set_actions (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_monitor(cfg, &monitor), "pipe_cfg_set_monitor (%s)", name);
	err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
	crash_if_unsuccessful(err, "pipe_create (%s)", name);
	doca_flow_pipe_cfg_destroy(cfg);

	err = steer_pipe_add_entry(0, pipe, &entry_match, 0, &actions, &monitor, NULL, 0,
	                           &status, entry_out);
	crash_if_unsuccessful(err, "pipe_add_entry (%s)", name);
	process_entries(port, &status, 1, name);
	DOCA_LOG_INFO("%s ready: set CE and clear DSCP path bit", name);
	return pipe;
}

/* Each virtual path has one independent ECN marker. It is eligible only when
 * the packet's original destination IP belongs to that path's receiver SF. */
static struct doca_flow_pipe *create_path_ip_match_pipe(struct doca_flow_port *port, int path,
						struct doca_flow_pipe *selected_target,
						struct doca_flow_pipe *clear_target,
						uint32_t path_ip,
						struct doca_flow_pipe_entry **entry_out)
{
	struct doca_flow_match match = {0}, match_mask = {0}, entry_match = {0};
	struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = selected_target};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = clear_target};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	char name[32];
	doca_error_t err;

	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.dst_ip = UINT32_MAX;
	match_mask.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match_mask.outer.ip4.dst_ip = UINT32_MAX;
	snprintf(name, sizeof(name), "PATH%d_IP_MATCH", path);

	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_name(cfg, name), "pipe_cfg_set_name (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC), "pipe_cfg_set_type (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT), "pipe_cfg_set_domain (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_is_root(cfg, false), "pipe_cfg_set_is_root (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_nr_entries(cfg, 1),
	                      "pipe_cfg_set_nr_entries (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask),
	                      "pipe_cfg_set_match (%s)", name);
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_monitor(cfg, &monitor),
	                      "pipe_cfg_set_monitor (%s)", name);
	err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
	crash_if_unsuccessful(err, "pipe_create (%s)", name);
	doca_flow_pipe_cfg_destroy(cfg);
	entry_match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	entry_match.outer.ip4.dst_ip = path_ip;
	struct entry_batch_status status = {0};
	err = steer_pipe_add_entry(0, pipe, &entry_match, 0, NULL, &monitor, NULL, 0,
	                           &status, entry_out);
	crash_if_unsuccessful(err, "pipe_add_entry (%s)", name);
	process_entries(port, &status, 1, name);
	DOCA_LOG_INFO("%s ready: destination IP 0x%08x is eligible for path%d CE", name,
	              rte_be_to_cpu_32(path_ip), path);
	return pipe;
}

/* Sender-side diagnostic: exact sender-QPN CNP entries are added after CM
 * pairing. Hits and misses both continue unchanged to the sender SF. */
static struct doca_flow_pipe *create_cnp_count_pipe(struct doca_flow_port *port,
					     struct doca_flow_pipe *deliver_sf,
					     struct doca_flow_pipe *miss_target)
{
#if !STEER_HAS_ROCE_MATCH
	(void)port;
	(void)deliver_sf;
	DOCA_LOG_WARN("DOCA 2.x has no public BTH matcher; sender CNP Flow counters are disabled");
	return miss_target;
#else
	struct doca_flow_match match = {0}, match_mask = {0};
	struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = deliver_sf};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = miss_target};
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_pipe *pipe;
	doca_error_t err;

	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_ROCE_V2;
	match.outer.roce_v2.bth.opcode = 0xFF;
	memset(match.outer.roce_v2.bth.dest_qp, 0xFF,
	       sizeof(match.outer.roce_v2.bth.dest_qp));
	match_mask.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match_mask.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_ROCE_V2;
	match_mask.outer.roce_v2.bth.opcode = 0xFF;
	memset(match_mask.outer.roce_v2.bth.dest_qp, 0xFF,
	       sizeof(match_mask.outer.roce_v2.bth.dest_qp));

	err = doca_flow_pipe_cfg_create(&cfg, port);
	crash_if_unsuccessful(err, "pipe_cfg_create (CNP count)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_name(cfg, "EGRESS_CNP_COUNT"),
	                      "pipe_cfg_set_name (CNP count)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC),
	                      "pipe_cfg_set_type (CNP count)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT),
	                      "pipe_cfg_set_domain (CNP count)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_is_root(cfg, false),
	                      "pipe_cfg_set_is_root (CNP count)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_nr_entries(cfg, MAX_CNP_TRACKED_QPS),
	                      "pipe_cfg_set_nr_entries (CNP count)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask),
	                      "pipe_cfg_set_match (CNP count)");
	crash_if_unsuccessful(doca_flow_pipe_cfg_set_monitor(cfg, &monitor),
	                      "pipe_cfg_set_monitor (CNP count)");
	err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
	crash_if_unsuccessful(err, "pipe_create (CNP count)");
	doca_flow_pipe_cfg_destroy(cfg);
	DOCA_LOG_INFO("EGRESS_CNP_COUNT ready: learned sender-QPN CNP diagnostics");
	return pipe;
#endif
}

/* Compensate for the unmarkable half of each path: the configured percentage
 * is the intended fraction over all path traffic, while only class==path can
 * enter this sampler. The selected-class rate is therefore doubled and capped. */
static struct doca_flow_pipe *path_ce_target(struct doca_flow_port *port, int idx, double intended_percent,
					     struct doca_flow_pipe *mark_pipe, struct doca_flow_pipe *clear_pipe)
{
	char name[32];
	double selected_percent = intended_percent * 2.0;

	if (selected_percent > 100.0)
		selected_percent = 100.0;
	DOCA_LOG_INFO("Path%d CE sampling: intended all-traffic=%.4g%% selected-class=%.4g%%",
	              idx, intended_percent, selected_percent);
	if (intended_percent > 50.0)
		DOCA_LOG_WARN("Path%d intended CE rate %.4g%% exceeds the approximately 50%% maximum "
		              "with class-selective marking", idx, intended_percent);

	if (selected_percent >= 100.0)
		return mark_pipe;
	if (selected_percent <= 0.0)
		return clear_pipe;
	snprintf(name, sizeof(name), "RANDOM_SAMPLE_P%d", idx);
	return create_random_sample_pipe(port, name, mark_pipe, clear_pipe, get_random_mask(selected_percent));
}

/* ------------------------------------------------------------------ *
 *  Module state + public API (see steer.h)                            *
 * ------------------------------------------------------------------ */

struct rate_flow_state {
	uint32_t qpn; /* PCC sender/initiator QPN */
	uint32_t receiver_qpn; /* optional responder QPN learned from CM REP */
	uint32_t latest_rate;
	uint64_t interval_rate_sum;
	uint64_t interval_rate_reports;
	uint32_t smoothed_rate;
	bool smoothed_rate_valid;
	uint8_t path;
	bool classified;
};

struct cm_request_state {
	uint32_t comm_id;
	uint32_t initiator_qpn;
	uint32_t receiver_ip;
	uint8_t path;
	bool path_known;
};

struct qpn_pair_state {
	uint32_t initiator_qpn;
	uint32_t responder_qpn;
	uint8_t path;
};

struct steer_state {
	bool started;
	struct steer_opts opts;
	struct doca_flow_port *port;
	struct doca_flow_port *sf_rep_port[NB_PATHS];
	struct doca_flow_pipe *classify_pipe;
	struct doca_flow_pipe *classify_dispatch_pipe;
	struct doca_flow_pipe *classify_target[NB_PATHS];
	struct doca_flow_pipe_entry *path_rewrite_entry[NB_PATHS];
	struct doca_flow_pipe_entry *legacy_random_entry[LEGACY_RANDOM_BUCKETS];
	struct doca_flow_pipe_entry *legacy_qp1_wire_entry[NB_PATHS];
	struct doca_flow_pipe_entry *legacy_qp1_filter_entry[NB_PATHS];
	struct doca_flow_pipe *cnp_count_pipe;
	bool grouping_enabled;
	struct doca_flow_pipe_entry *classify_entry[PATH_SHARE_BUCKETS];
	uint8_t classify_bucket_path[PATH_SHARE_BUCKETS];
	uint32_t applied_path0_share;
	struct doca_flow_pipe_entry *path_ip_entry[NB_PATHS];
	struct doca_flow_pipe_entry *path_demux_entry[NB_PATHS];
	struct doca_flow_pipe_entry *mark_entry[NB_PATHS];
	struct doca_flow_pipe_entry *clear_path_entry;
	uint32_t cnp_sender_qpn[MAX_CNP_TRACKED_QPS];
	uint8_t cnp_path[MAX_CNP_TRACKED_QPS];
	struct doca_flow_pipe_entry *cnp_entry[MAX_CNP_TRACKED_QPS];
	uint32_t cnp_entry_count;
	uint64_t cnp_prev_pkts[NB_PATHS];
	uint64_t cnp_prev_cycles;
	bool cnp_delta_ready;
	struct rate_flow_state rate_flow[MAX_RATE_FLOWS];
	uint32_t rate_flow_count;
	struct cm_request_state cm_request[MAX_CM_CONNECTIONS];
	uint32_t cm_request_count;
	struct qpn_pair_state qpn_pair[MAX_CM_CONNECTIONS];
	uint32_t qpn_pair_count;
	uint64_t ingress_prev_bytes[NB_PATHS];
	uint64_t ingress_prev_mark_pkts[NB_PATHS];
	uint64_t ingress_prev_cycles;
	bool ingress_throughput_ready;
	uint64_t dpdk_rx_bursts;
	uint64_t dpdk_full_bursts;
	uint64_t dpdk_rx_pkts;
	uint64_t dpdk_freed_pkts;
	uint64_t dpdk_roce_pkts;
	uint64_t dpdk_feedback_pkts;
	uint64_t dpdk_feedback_path_pkts[NB_PATHS];
	uint64_t dpdk_qp1_pkts;
	uint64_t dpdk_unknown_path_pkts;
	uint64_t dpdk_learned_qpns;
	atomic_flag rate_lock;
};

static struct steer_state g_steer;

doca_error_t steer_parse_rep_spec(const char *spec,
                                  char pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE], uint32_t *sf_num)
{
	const char *bdf, *comma, *sf, *end;
	char *number_end;
	size_t bdf_len;
	unsigned long value;

	if (spec == NULL || pci_addr == NULL || sf_num == NULL || strncmp(spec, "pci/", 4) != 0)
		return DOCA_ERROR_INVALID_VALUE;
	bdf = spec + 4;
	comma = strchr(bdf, ',');
	if (comma == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	bdf_len = (size_t)(comma - bdf);
	if ((bdf_len != DOCA_DEVINFO_PCI_BDF_SIZE - 1 &&
	     bdf_len != DOCA_DEVINFO_PCI_ADDR_SIZE - 1) || bdf_len >= DOCA_DEVINFO_PCI_ADDR_SIZE)
		return DOCA_ERROR_INVALID_VALUE;
	sf = strstr(comma + 1, "sf");
	if (sf == NULL || sf[2] == '\0')
		return DOCA_ERROR_INVALID_VALUE;
	errno = 0;
	value = strtoul(sf + 2, &number_end, 10);
	end = number_end;
	if (errno != 0 || *end != '\0' || value > UINT16_MAX)
		return DOCA_ERROR_INVALID_VALUE;
	memcpy(pci_addr, bdf, bdf_len);
	pci_addr[bdf_len] = '\0';
	*sf_num = (uint32_t)value;
	return DOCA_SUCCESS;
}

void steer_default_opts(struct steer_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->sf_num = 0;
	opts->role = STEER_ROLE_BOTH;
	opts->force_path = -1;
	opts->path_percent[0] = 100.0;
	opts->path_percent[1] = 100.0;
}

/* Enforce a calculated path share by changing only persistent buckets whose
 * assignment crosses the old/new boundary. No entry is added or removed. */
static void apply_path_share(uint32_t path0_share)
{
	if (g_steer.opts.force_path >= 0)
		return;
	if (g_steer.classify_pipe == NULL || path0_share > PATH_SHARE_BUCKETS ||
	    path0_share == g_steer.applied_path0_share)
		return;

	uint32_t old_share = g_steer.applied_path0_share;
	for (uint32_t bucket = 0; bucket < PATH_SHARE_BUCKETS; bucket++) {
		uint8_t wanted_path = bucket < path0_share ? 0 : 1;
		if (g_steer.classify_bucket_path[bucket] == wanted_path)
			continue;

		memset(&g_classify_batch, 0, sizeof(g_classify_batch));
		struct doca_flow_fwd fwd = {
			.type = DOCA_FLOW_FWD_PIPE,
			.next_pipe = g_steer.classify_target[wanted_path],
		};
		doca_error_t err = steer_pipe_update_entry(0, g_steer.classify_dispatch_pipe, 0,
						 NULL, NULL, &fwd, STEER_NO_WAIT,
						 g_steer.classify_entry[bucket]);
		if (err == DOCA_SUCCESS)
			err = doca_flow_entries_process(g_steer.port, 0, 10000, 1);
		if (err != DOCA_SUCCESS || g_classify_batch.failure ||
		    g_classify_batch.nb_processed != 1) {
			DOCA_LOG_WARN("path-share update failed at bucket %u (%s); will retry",
			              bucket, doca_error_get_descr(err != DOCA_SUCCESS ? err : DOCA_ERROR_BAD_STATE));
			return;
		}
		g_steer.classify_bucket_path[bucket] = wanted_path;
	}

	/* Processing one completion per update can return before HWS has retired
	 * the replaced rule version. Drain those internal operations now so pipe
	 * teardown never races a pending replacement. */
	doca_error_t drain_err = doca_flow_entries_process(g_steer.port, 0, 10000, 0);
	if (drain_err != DOCA_SUCCESS)
		DOCA_LOG_WARN("failed to drain path-share replacements: %s",
		              doca_error_get_descr(drain_err));

	g_steer.applied_path0_share = path0_share;
	DOCA_LOG_INFO("PCC path share applied: path0 %u->%u/64 path1=%u/64",
	              old_share, path0_share, PATH_SHARE_BUCKETS - path0_share);
}

doca_error_t steer_start(const struct steer_opts *opts)
{
	if (g_steer.started)
		return DOCA_ERROR_BAD_STATE;
	if (opts == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	if ((opts->role != STEER_ROLE_INGRESS && opts->role != STEER_ROLE_EGRESS &&
	     opts->role != STEER_ROLE_BOTH) ||
	    opts->force_path < -1 || opts->force_path >= NB_PATHS) {
		DOCA_LOG_CRIT("invalid steering role or forced path");
		return DOCA_ERROR_INVALID_VALUE;
	}

	const bool do_ingress = (opts->role != STEER_ROLE_EGRESS);
	const bool do_egress = (opts->role != STEER_ROLE_INGRESS);
	if (!opts->path_ip_set[0] || !opts->path_ip_set[1]) {
		DOCA_LOG_CRIT("both path0-ip and path1-ip are required");
		return DOCA_ERROR_INVALID_VALUE;
	}
#if DOCA_HAS_DEVICE_REPRESENTORS
	if (opts->dev == NULL || opts->dev_rep == NULL) {
		DOCA_LOG_CRIT("steer_start: opts->dev and opts->dev_rep are required on DOCA 3.x "
		              "(open them via --device/--rep or DOCA device APIs)");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (do_ingress && opts->dev_rep_path1 == NULL) {
		DOCA_LOG_CRIT("ingress requires two receiver SF representors");
		return DOCA_ERROR_INVALID_VALUE;
	}
#else
	if (do_ingress && !opts->path1_sf_num_set) {
		DOCA_LOG_CRIT("DOCA 2.x ingress requires -r and -R/--path1-rep");
		return DOCA_ERROR_INVALID_VALUE;
	}
#endif

	g_steer.opts = *opts;
	atomic_flag_clear(&g_steer.rate_lock);

#if DOCA_HAS_DEVICE_REPRESENTORS
	struct doca_dev *dev = opts->dev;
	const bool probe_do_ingress = do_ingress;
	uint32_t probe_nb_sf_ports = probe_do_ingress ? NB_PATHS : 1;
	struct doca_dev_rep *dev_reps[NB_PATHS] = {opts->dev_rep, opts->dev_rep_path1};

	probe_device(dev, opts->devargs, dev_reps, probe_nb_sf_ports);
	configure_and_start_dpdk_port(dev);
	initialize_doca_flow();

	g_steer.port = port_start(dev, WIRE_PORT_ID);
	g_steer.sf_rep_port[0] = rep_port_start(SF_PORT_ID, opts->dev_rep);
	if (probe_nb_sf_ports == NB_PATHS)
		g_steer.sf_rep_port[1] = rep_port_start(SF_PATH1_PORT_ID, opts->dev_rep_path1);
#else
	char probe_args[160];
	const bool probe_do_ingress = do_ingress;
	uint32_t probe_nb_sf_ports = probe_do_ingress ? NB_PATHS : 1;

	if (probe_do_ingress) {
		snprintf(probe_args, sizeof(probe_args),
		         "dv_flow_en=2,fdb_def_rule_en=1,repr_matching_en=0,representor=sf[%u,%u]",
		         opts->sf_num, opts->path1_sf_num);
	} else {
		snprintf(probe_args, sizeof(probe_args),
		         "dv_flow_en=2,fdb_def_rule_en=1,repr_matching_en=0,representor=sf%u", opts->sf_num);
	}
	struct doca_dev *dev = opts->dev;
	if (dev != NULL)
		probe_open_dev(dev, opts->device_pci_addr, probe_args);
	else
		dev = open_and_probe_dev(opts->device_pci_addr, probe_args);

	configure_and_start_dpdk_port(dev);
	initialize_doca_flow();

	g_steer.port = port_start(dev, WIRE_PORT_ID);
	uint16_t rep_ids[NB_PATHS] = {0};
	find_sf_representor_port_ids(rep_ids, probe_nb_sf_ports);
	g_steer.sf_rep_port[0] = rep_port_start(rep_ids[0]);
	if (probe_nb_sf_ports == NB_PATHS)
		g_steer.sf_rep_port[1] = rep_port_start(rep_ids[1]);
#endif

	/* Deliver pipes: fwd_miss cannot be a port on 3.x HWS, so port delivery is
	 * done via these FWD_PIPE targets. Both roles need both. */
	struct doca_flow_pipe *deliver_sf[NB_PATHS] = {0};
	deliver_sf[0] = create_deliver_pipe(g_steer.port, "DELIVER_SF0", SF_PORT_ID);
	struct doca_flow_pipe *deliver_wire = create_deliver_pipe(g_steer.port, "DELIVER_WIRE", WIRE_PORT_ID);

	DOCA_LOG_INFO("Configured path grouping: path0 IP=0x%08x path1 IP=0x%08x",
	              rte_be_to_cpu_32(g_steer.opts.path_ip[0]),
	              rte_be_to_cpu_32(g_steer.opts.path_ip[1]));
	uint32_t nb_sf_ports = do_ingress ? NB_PATHS : 1;
	struct doca_flow_pipe *receiver_target = deliver_sf[0];
	if (do_ingress) {
		deliver_sf[1] = create_deliver_pipe(g_steer.port, "DELIVER_SF1", SF_PATH1_PORT_ID);
		receiver_target = create_receiver_ip_demux(g_steer.port, deliver_sf, g_steer.opts.path_ip);
	}

	/* PORT_DEMUX targets default to plain delivery; the active role overrides. */
	struct doca_flow_pipe *wire_target = receiver_target; /* wire-ingress fate */
	struct doca_flow_pipe *sf_target = deliver_wire;  /* SF-egress fate */
#if DOCA_USES_LEGACY_FLOW_BACKEND
	if (do_egress)
		install_legacy_qp1_clone_path(g_steer.port, &wire_target, g_steer.opts.path_ip,
		                               g_steer.legacy_qp1_wire_entry,
		                               g_steer.legacy_qp1_filter_entry);
#endif

	if (do_ingress) {
		/* Ingress: choose the virtual path first, then run that path's
		 * independent destination-IP ECN marker. */
		struct doca_flow_pipe *clear_path =
			create_clear_path_pipe(g_steer.port, receiver_target, &g_steer.clear_path_entry);
		struct doca_flow_pipe *path_match[NB_PATHS];
		for (int path = 0; path < NB_PATHS; path++) {
			struct doca_flow_pipe *mark =
				create_selected_mark_pipe(g_steer.port, path, receiver_target,
				                          &g_steer.mark_entry[path]);
			struct doca_flow_pipe *selected =
				path_ce_target(g_steer.port, path, g_steer.opts.path_percent[path],
				               mark, clear_path);
			path_match[path] = create_path_ip_match_pipe(g_steer.port, path, selected,
			                                                   clear_path,
			                                                   g_steer.opts.path_ip[path],
			                                                   &g_steer.path_ip_entry[path]);
		}

		struct doca_flow_pipe *path_demux =
			create_path_demux_pipe(g_steer.port, path_match, clear_path,
			                       g_steer.path_demux_entry);
		wire_target = create_roce_check_pipe(g_steer.port, "INGRESS_ROCE_CHECK",
		                                     path_demux, receiver_target, false);
	}

	if (do_egress) {
#if !STEER_USE_RANDOM_HASH_CLASSIFIER
		DOCA_LOG_WARN("DOCA 2.x: immutable 64-bucket random HASH plus metadata dispatch enabled");
		struct doca_flow_pipe *path0_rewrite =
			create_path_rewrite_pipe(g_steer.port, 0, sf_target,
			                         &g_steer.path_rewrite_entry[0]);
		struct doca_flow_pipe *path1_rewrite =
			create_path_rewrite_pipe(g_steer.port, 1, sf_target,
			                         &g_steer.path_rewrite_entry[1]);
		struct doca_flow_pipe *path_target[NB_PATHS] = {path0_rewrite, path1_rewrite};
		g_steer.classify_target[0] = path0_rewrite;
		g_steer.classify_target[1] = path1_rewrite;
		g_steer.classify_dispatch_pipe = create_classify_dispatch_pipe(
			g_steer.port, path_target, g_steer.classify_entry,
			g_steer.classify_bucket_path, g_steer.opts.force_path);
		g_steer.classify_pipe = create_legacy_small_random_table(
			g_steer.port, g_steer.classify_dispatch_pipe, 6,
			g_steer.legacy_random_entry);
		sf_target = g_steer.classify_pipe;
		g_steer.applied_path0_share = g_steer.opts.force_path == 0 ? PATH_SHARE_BUCKETS
		                              : g_steer.opts.force_path == 1 ? 0
		                              : PATH_SHARE_BUCKETS / 2;
		if (g_steer.opts.force_path >= 0)
			DOCA_LOG_WARN("egress diagnostic: all classifier buckets forced to path%d",
			              g_steer.opts.force_path);
		g_steer.grouping_enabled = true;
#else
		g_steer.grouping_enabled = true;
		g_steer.cnp_count_pipe = create_cnp_count_pipe(g_steer.port, deliver_sf[0], wire_target);
		wire_target = g_steer.cnp_count_pipe;
		struct doca_flow_pipe *egress_delivery_target = sf_target;
		struct doca_flow_pipe *classify_target = egress_delivery_target;
		for (uint8_t path = 0; path < NB_PATHS; path++)
			g_steer.classify_target[path] =
				create_path_rewrite_pipe(g_steer.port, path, egress_delivery_target,
			                         &g_steer.path_rewrite_entry[path]);

		if (g_steer.opts.force_path >= 0) {
			uint8_t forced = (uint8_t)g_steer.opts.force_path;

			g_steer.applied_path0_share = forced == 0 ? PATH_SHARE_BUCKETS : 0;
			DOCA_LOG_WARN("egress diagnostic: EGRESS_CLASSIFY bypassed; "
			              "RoCE forwarded directly through path%u rewrite", forced);
			sf_target = create_roce_check_pipe(g_steer.port, "EGRESS_ROCE_CHECK",
			                                   g_steer.classify_target[forced], egress_delivery_target,
			                                   false);
		} else
		{
			g_steer.classify_dispatch_pipe = create_classify_dispatch_pipe(
				g_steer.port, g_steer.classify_target, g_steer.classify_entry,
				g_steer.classify_bucket_path, g_steer.opts.force_path);
			classify_target = g_steer.classify_dispatch_pipe;
			g_steer.classify_pipe = create_classify_pipe(g_steer.port, classify_target);
			add_classify_entries(g_steer.classify_pipe, g_steer.port);
			g_steer.applied_path0_share = PATH_SHARE_BUCKETS / 2;

			/* All admitted RoCE traffic is randomly assigned a DSCP path bit. */
			sf_target = create_roce_check_pipe(g_steer.port, "EGRESS_ROCE_CHECK",
			                                   g_steer.classify_pipe, egress_delivery_target,
			                                   true);
		}
#endif
	}

	/* Sender/receiver pairing is consumed only by egress path grouping. */
#if DOCA_HAS_DEVICE_REPRESENTORS
	install_native_qp1_clone_paths(g_steer.port, receiver_target, deliver_wire,
	                                &wire_target, &sf_target);
#endif
	if (do_ingress)
		install_arp_paths(g_steer.port, &wire_target, &sf_target);

	create_port_demux_pipe(g_steer.port, sf_target, wire_target, nb_sf_ports);

	g_steer.started = true;
	DOCA_LOG_INFO("steer started (role=%s, path0=%.4g%% path1=%.4g%%)",
		      g_steer.opts.role == STEER_ROLE_EGRESS ? "egress"
		      : g_steer.opts.role == STEER_ROLE_INGRESS ? "ingress" : "both",
		      g_steer.opts.path_percent[0], g_steer.opts.path_percent[1]);
	return DOCA_SUCCESS;
}

void steer_update_pcc_rate(uint32_t qpn, uint32_t rate)
{
	if (!g_steer.grouping_enabled)
		return;
	qpn &= 0x00FFFFFFu;
	if (qpn <= 1)
		return;

	while (atomic_flag_test_and_set_explicit(&g_steer.rate_lock, memory_order_acquire))
		;
	struct rate_flow_state *flow = NULL;
	for (uint32_t i = 0; i < g_steer.rate_flow_count; i++) {
		if (g_steer.rate_flow[i].qpn == qpn) {
			flow = &g_steer.rate_flow[i];
			break;
		}
	}
	if (flow == NULL && g_steer.rate_flow_count < MAX_RATE_FLOWS) {
		flow = &g_steer.rate_flow[g_steer.rate_flow_count++];
		memset(flow, 0, sizeof(*flow));
		flow->qpn = qpn;
	}
	if (flow != NULL) {
		flow->latest_rate = rate;
		flow->interval_rate_sum += rate;
		flow->interval_rate_reports++;
		uint32_t hash_qpn = 0;
		uint8_t path = 0;
		bool path_known = false;
		for (uint32_t i = 0; i < g_steer.qpn_pair_count; i++) {
			if (g_steer.qpn_pair[i].initiator_qpn == qpn) {
				hash_qpn = g_steer.qpn_pair[i].responder_qpn;
				path = g_steer.qpn_pair[i].path;
				path_known = true;
				break;
			}
		}
		if (!path_known) {
			for (uint32_t i = 0; i < g_steer.cm_request_count; i++) {
				if (g_steer.cm_request[i].initiator_qpn == qpn &&
				    g_steer.cm_request[i].path_known) {
					path = g_steer.cm_request[i].path;
					path_known = true;
					break;
				}
			}
		}
		if (!flow->classified && path_known) {
			flow->receiver_qpn = hash_qpn;
			flow->path = path;
			flow->classified = true;
			DOCA_LOG_INFO("egress path class: PCC sender QPN 0x%06x rate=%u IP-path=%u",
			              qpn, rate, flow->path);
		}
	}
	atomic_flag_clear_explicit(&g_steer.rate_lock, memory_order_release);
}

static uint16_t read_be16(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t read_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static void install_sender_cnp_entry(uint32_t sender_qpn, uint32_t receiver_qpn, uint8_t path)
{
#if !STEER_HAS_ROCE_MATCH
	(void)sender_qpn;
	(void)receiver_qpn;
	(void)path;
	return;
#else
	if (g_steer.cnp_count_pipe == NULL)
		return;
	sender_qpn &= 0x00FFFFFFu;
	receiver_qpn &= 0x00FFFFFFu;
	for (uint32_t i = 0; i < g_steer.cnp_entry_count; i++) {
		if (g_steer.cnp_sender_qpn[i] == sender_qpn)
			return;
	}
	if (g_steer.cnp_entry_count >= MAX_CNP_TRACKED_QPS) {
		DOCA_LOG_WARN("sender CNP diagnostic table full; cannot install QPN 0x%06x",
		              sender_qpn);
		return;
	}

	struct doca_flow_match match = {0};
	struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
	struct entry_batch_status status = {0};
	uint32_t idx = g_steer.cnp_entry_count;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_ROCE_V2;
	match.outer.roce_v2.bth.opcode = ROCE_BTH_OPCODE_CNP;
	match.outer.roce_v2.bth.dest_qp[0] = (uint8_t)(sender_qpn >> 16);
	match.outer.roce_v2.bth.dest_qp[1] = (uint8_t)(sender_qpn >> 8);
	match.outer.roce_v2.bth.dest_qp[2] = (uint8_t)sender_qpn;
	doca_error_t err = steer_pipe_add_entry(0, g_steer.cnp_count_pipe, &match, 0, NULL,
	                                        &monitor, NULL, 0, &status,
	                                        &g_steer.cnp_entry[idx]);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_WARN("failed to add sender CNP counter for QPN 0x%06x: %s",
		              sender_qpn, doca_error_get_descr(err));
		return;
	}
	process_entries(g_steer.port, &status, 1, "sender CNP counter");
	g_steer.cnp_sender_qpn[idx] = sender_qpn;
	g_steer.cnp_path[idx] = path;
	g_steer.cnp_entry_count++;
	DOCA_LOG_INFO("sender CNP counter installed: sender QPN 0x%06x -> receiver QPN "
	              "0x%06x path%u", sender_qpn, receiver_qpn, path);
#endif
}

#if DOCA_USES_LEGACY_FLOW_BACKEND
static void retire_legacy_qp1_mirror_entry(
	struct doca_flow_pipe_entry *entries[NB_PATHS], uint8_t path, const char *direction)
{
	if (path >= NB_PATHS || entries[path] == NULL)
		return;
	doca_error_t err = steer_pipe_remove_entry(0, STEER_NO_WAIT, entries[path]);
	if (err == DOCA_SUCCESS)
		err = doca_flow_entries_process(g_steer.port, 0, 10000, 1);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_WARN("failed retiring path%u %s QP1 mirror entry: %s",
			path, direction, doca_error_get_descr(err));
		return;
	}
	entries[path] = NULL;
	DOCA_LOG_INFO("retired path%u %s QP1 mirror entry", path, direction);
}
#endif

static void remember_cm_request(uint32_t comm_id, uint32_t initiator_qpn, uint32_t receiver_ip)
{
	uint8_t path = 0;
	bool path_known = false;
	for (uint8_t i = 0; i < NB_PATHS; i++) {
		if (g_steer.opts.path_ip_set[i] &&
		    rte_be_to_cpu_32(g_steer.opts.path_ip[i]) == receiver_ip) {
			path = i;
			path_known = true;
			break;
		}
	}
	while (atomic_flag_test_and_set_explicit(&g_steer.rate_lock, memory_order_acquire))
		;
	for (uint32_t i = 0; i < g_steer.cm_request_count; i++) {
		if (g_steer.cm_request[i].comm_id == comm_id) {
			g_steer.cm_request[i].initiator_qpn = initiator_qpn;
			g_steer.cm_request[i].receiver_ip = receiver_ip;
			g_steer.cm_request[i].path = path;
			g_steer.cm_request[i].path_known = path_known;
			atomic_flag_clear_explicit(&g_steer.rate_lock, memory_order_release);
			return;
		}
	}
	if (g_steer.cm_request_count < MAX_CM_CONNECTIONS) {
		struct cm_request_state *request = &g_steer.cm_request[g_steer.cm_request_count++];
		request->comm_id = comm_id;
		request->initiator_qpn = initiator_qpn;
		request->receiver_ip = receiver_ip;
		request->path = path;
		request->path_known = path_known;
	} else {
		DOCA_LOG_WARN("RDMA-CM request table full; cannot remember comm_id=0x%08x", comm_id);
	}
	if (path_known) {
		for (uint32_t i = 0; i < g_steer.rate_flow_count; i++) {
			struct rate_flow_state *flow = &g_steer.rate_flow[i];
			if (flow->qpn == initiator_qpn) {
				flow->path = path;
				flow->classified = true;
				break;
			}
		}
	}
	atomic_flag_clear_explicit(&g_steer.rate_lock, memory_order_release);
	if (path_known)
		DOCA_LOG_INFO("RDMA-CM REQ grouping: sender QPN 0x%06x receiver IP=0x%08x -> path%u",
		              initiator_qpn, receiver_ip, path);
	else
		DOCA_LOG_WARN("RDMA-CM REQ sender QPN 0x%06x receiver IP=0x%08x is not configured",
		              initiator_qpn, receiver_ip);
}

static void complete_cm_mapping(uint32_t remote_comm_id, uint32_t responder_qpn)
{
	uint32_t initiator_qpn = 0;
	uint8_t path = 0;
	bool path_known = false;
	bool changed = false;

	while (atomic_flag_test_and_set_explicit(&g_steer.rate_lock, memory_order_acquire))
		;
	for (uint32_t i = 0; i < g_steer.cm_request_count; i++) {
		if (g_steer.cm_request[i].comm_id == remote_comm_id) {
			initiator_qpn = g_steer.cm_request[i].initiator_qpn;
			path = g_steer.cm_request[i].path;
			path_known = g_steer.cm_request[i].path_known;
			break;
		}
	}
	if (initiator_qpn != 0 && path_known) {
		struct qpn_pair_state *pair = NULL;
		for (uint32_t i = 0; i < g_steer.qpn_pair_count; i++) {
			if (g_steer.qpn_pair[i].initiator_qpn == initiator_qpn) {
				pair = &g_steer.qpn_pair[i];
				break;
			}
		}
		if (pair == NULL && g_steer.qpn_pair_count < MAX_CM_CONNECTIONS)
			pair = &g_steer.qpn_pair[g_steer.qpn_pair_count++];
		if (pair != NULL && (pair->initiator_qpn != initiator_qpn ||
		                     pair->responder_qpn != responder_qpn || pair->path != path)) {
			pair->initiator_qpn = initiator_qpn;
			pair->responder_qpn = responder_qpn;
			pair->path = path;
			changed = true;
		}
		for (uint32_t i = 0; i < g_steer.rate_flow_count; i++) {
			struct rate_flow_state *flow = &g_steer.rate_flow[i];
			if (flow->qpn == initiator_qpn) {
				flow->receiver_qpn = responder_qpn;
				flow->path = path;
				flow->classified = true;
				break;
			}
		}
	}
	atomic_flag_clear_explicit(&g_steer.rate_lock, memory_order_release);

	if (initiator_qpn == 0)
		DOCA_LOG_WARN("RDMA-CM REP has no captured REQ for remote_comm_id=0x%08x",
		              remote_comm_id);
	else if (!path_known)
		DOCA_LOG_WARN("RDMA-CM sender QPN 0x%06x receiver IP is not a configured path", initiator_qpn);
	else if (changed)
		DOCA_LOG_INFO("RDMA-CM mapping: sender 0x%06x -> receiver 0x%06x path%u",
		              initiator_qpn, responder_qpn, path);
	if (initiator_qpn != 0 && path_known)
		install_sender_cnp_entry(initiator_qpn, responder_qpn, path);
#if DOCA_USES_LEGACY_FLOW_BACKEND
	if (initiator_qpn != 0 && path_known)
		retire_legacy_qp1_mirror_entry(g_steer.legacy_qp1_wire_entry, path, "wire-ingress");
#endif
}

#if DOCA_USES_LEGACY_FLOW_BACKEND
static void learn_ingress_feedback_qpn(uint32_t sender_qpn, uint32_t source_ip)
{
	uint8_t path = 0;
	bool path_known = false;
	bool changed = false;

	for (uint8_t i = 0; i < NB_PATHS; i++) {
		if (g_steer.opts.path_ip_set[i] &&
		    rte_be_to_cpu_32(g_steer.opts.path_ip[i]) == source_ip) {
			path = i;
			path_known = true;
			break;
		}
	}
	if (!path_known) {
		g_steer.dpdk_unknown_path_pkts++;
		return;
	}
	g_steer.dpdk_feedback_path_pkts[path]++;
	sender_qpn &= 0x00FFFFFFu;
	if (sender_qpn <= 1)
		return;

	while (atomic_flag_test_and_set_explicit(&g_steer.rate_lock, memory_order_acquire))
		;
	struct qpn_pair_state *pair = NULL;
	for (uint32_t i = 0; i < g_steer.qpn_pair_count; i++) {
		if (g_steer.qpn_pair[i].initiator_qpn == sender_qpn) {
			pair = &g_steer.qpn_pair[i];
			break;
		}
	}
	if (pair == NULL && g_steer.qpn_pair_count < MAX_CM_CONNECTIONS)
		pair = &g_steer.qpn_pair[g_steer.qpn_pair_count++];
	if (pair != NULL &&
	    (pair->initiator_qpn != sender_qpn || pair->path != path)) {
		pair->initiator_qpn = sender_qpn;
		pair->responder_qpn = sender_qpn;
		pair->path = path;
		changed = true;
	}
	for (uint32_t i = 0; i < g_steer.rate_flow_count; i++) {
		struct rate_flow_state *flow = &g_steer.rate_flow[i];
		if (flow->qpn == sender_qpn) {
			flow->receiver_qpn = sender_qpn;
			flow->path = path;
			flow->classified = true;
			break;
		}
	}
	atomic_flag_clear_explicit(&g_steer.rate_lock, memory_order_release);

	if (pair == NULL) {
		DOCA_LOG_WARN("QPN mapping table full; cannot learn feedback QPN 0x%06x", sender_qpn);
		return;
	}
	if (changed) {
		g_steer.dpdk_learned_qpns++;
		DOCA_LOG_INFO("ingress feedback grouping: PCC sender QPN 0x%06x "
		              "source IP=0x%08x -> path%u", sender_qpn, source_ip, path);
		/* Keep the cross-version test/log contract. DOCA 2 learns the sender
		 * mapping from ingress feedback rather than an RDMA-CM REQ/REP pair. */
		DOCA_LOG_INFO("RDMA-CM mapping: sender 0x%06x -> receiver unknown path%u "
		              "(DOCA 2 ingress-feedback inference)", sender_qpn, path);
	}
}
#endif

static void parse_qp1_clone(struct rte_mbuf *mbuf)
{
	uint8_t scratch[512];
	uint32_t packet_len = rte_pktmbuf_pkt_len(mbuf);
	uint32_t captured = packet_len < sizeof(scratch) ? packet_len : sizeof(scratch);
	const uint8_t *packet = rte_pktmbuf_read(mbuf, 0, captured, scratch);
	if (packet == NULL || captured < RTE_ETHER_HDR_LEN + 20)
		return;
	uint32_t l3_offset = RTE_ETHER_HDR_LEN;
	uint16_t ether_type = read_be16(packet + 12);
	while ((ether_type == RTE_ETHER_TYPE_VLAN || ether_type == RTE_ETHER_TYPE_QINQ) &&
	       captured >= l3_offset + 4 + 20) {
		ether_type = read_be16(packet + l3_offset + 2);
		l3_offset += 4;
	}
	if (ether_type != RTE_ETHER_TYPE_IPV4 || captured < l3_offset + 20)
		return;

	const uint8_t *ip = packet + l3_offset;
	uint32_t ip_header_len = (uint32_t)(ip[0] & 0x0f) * 4;
	if ((ip[0] >> 4) != 4 || ip_header_len < 20 || ip[9] != IPPROTO_UDP ||
	    captured < l3_offset + ip_header_len + sizeof(struct rte_udp_hdr) + 12)
		return;
	const uint8_t *udp = ip + ip_header_len;
	if (read_be16(udp + 2) != ROCE_UDP_PORT_NATIVE)
		return;
	g_steer.dpdk_roce_pkts++;
	const uint8_t *bth = udp + sizeof(struct rte_udp_hdr);
	uint32_t destination_qpn = ((uint32_t)bth[5] << 16) | ((uint32_t)bth[6] << 8) | bth[7];
	if (destination_qpn != QP1_QPN) {
#if DOCA_USES_LEGACY_FLOW_BACKEND
		g_steer.dpdk_feedback_pkts++;
		learn_ingress_feedback_qpn(destination_qpn, read_be32(ip + 12));
#endif
		return;
	}
	g_steer.dpdk_qp1_pkts++;

#if DOCA_USES_LEGACY_FLOW_BACKEND
	/* QP1 is unnecessary on 2.x: wait for an ACK/CNP carrying the sender QPN. */
	return;
#endif

	/* UD QP1: BTH(12), DETH(8), then the 24-byte MAD header. */
	const uint8_t *mad = bth + 20;
	uint32_t mad_offset = (uint32_t)(mad - packet);
	if (captured < mad_offset + 24)
		return;
	uint16_t attribute = read_be16(mad + 16);
	DOCA_LOG_INFO("QP1 clone: len=%u opcode=0x%02x class=0x%02x method=0x%02x attr=0x%04x",
	              packet_len, bth[0], mad[1], mad[3], attribute);
	if (mad[1] != IB_MGMT_CLASS_CM)
		return;
	if (attribute == IB_CM_ATTR_REQ && captured >= mad_offset + 60) {
		uint32_t local_comm_id = read_be32(mad + 24);
		uint32_t local_qpn = read_be32(mad + 56) >> 8;
		uint32_t receiver_ip = read_be32(ip + 16);
		DOCA_LOG_INFO("RDMA-CM REQ: local_comm_id=0x%08x initiator_qpn=0x%06x receiver_ip=0x%08x",
		              local_comm_id, local_qpn, receiver_ip);
		remember_cm_request(local_comm_id, local_qpn, receiver_ip);
	} else if (attribute == IB_CM_ATTR_REP && captured >= mad_offset + 40) {
		uint32_t local_comm_id = read_be32(mad + 24);
		uint32_t remote_comm_id = read_be32(mad + 28);
		uint32_t local_qpn = read_be32(mad + 36) >> 8;
		DOCA_LOG_INFO("RDMA-CM REP: local_comm_id=0x%08x remote_comm_id=0x%08x responder_qpn=0x%06x",
		              local_comm_id, remote_comm_id, local_qpn);
		complete_cm_mapping(remote_comm_id, local_qpn);
	}
}

static void poll_qp1_clones(void)
{
	if (g_dpdk_rx_port_id == UINT16_MAX)
		return;
	for (;;) {
		struct rte_mbuf *packets[QP1_RX_BURST];
		uint16_t received = rte_eth_rx_burst(g_dpdk_rx_port_id, QP1_CLONE_QUEUE, packets,
		                                          QP1_RX_BURST);
		g_steer.dpdk_rx_bursts++;
		g_steer.dpdk_rx_pkts += received;
		if (received == QP1_RX_BURST)
			g_steer.dpdk_full_bursts++;
		for (uint16_t i = 0; i < received; i++) {
			parse_qp1_clone(packets[i]);
			rte_pktmbuf_free(packets[i]);
			g_steer.dpdk_freed_pkts++;
		}
		if (received < QP1_RX_BURST)
			break;
	}
}

void steer_poll_rx(void)
{
	if (g_steer.started)
		poll_qp1_clones();
}

void steer_poll(void)
{
	if (!g_steer.started)
		return;
	steer_poll_rx();

#if DOCA_USES_LEGACY_FLOW_BACKEND
	if (g_steer.dpdk_rx_pkts != 0) {
		DOCA_LOG_INFO("DPDK ingress clones: rx=%lu freed=%lu outstanding=%lu bursts=%lu full=%lu "
		              "roce=%lu feedback=%lu path0=%lu path1=%lu qp1=%lu unknown-path=%lu learned-qpn=%lu",
		              g_steer.dpdk_rx_pkts, g_steer.dpdk_freed_pkts,
		              g_steer.dpdk_rx_pkts - g_steer.dpdk_freed_pkts,
		              g_steer.dpdk_rx_bursts, g_steer.dpdk_full_bursts,
		              g_steer.dpdk_roce_pkts, g_steer.dpdk_feedback_pkts,
		              g_steer.dpdk_feedback_path_pkts[0], g_steer.dpdk_feedback_path_pkts[1],
		              g_steer.dpdk_qp1_pkts, g_steer.dpdk_unknown_path_pkts,
		              g_steer.dpdk_learned_qpns);
		struct rte_eth_stats stats = {0};
		int stats_rc = rte_eth_stats_get(g_dpdk_rx_port_id, &stats);
		if (stats_rc == 0)
			DOCA_LOG_INFO("DPDK RX driver stats: ipackets=%lu imissed=%lu ierrors=%lu "
			              "rx_nombuf=%lu q%u_errors=%lu",
			              stats.ipackets, stats.imissed, stats.ierrors, stats.rx_nombuf,
			              QP1_CLONE_QUEUE, stats.q_errors[QP1_CLONE_QUEUE]);
		else
			DOCA_LOG_WARN("rte_eth_stats_get(port=%u) failed: %d",
			              g_dpdk_rx_port_id, stats_rc);
	}
#endif

	if (g_steer.grouping_enabled) {
		uint64_t reduced_sum[NB_PATHS] = {0};
		uint32_t total[NB_PATHS] = {0}, full[NB_PATHS] = {0}, reduced[NB_PATHS] = {0};
		uint32_t pending_mapping = 0;
		while (atomic_flag_test_and_set_explicit(&g_steer.rate_lock, memory_order_acquire))
			;
		for (uint32_t i = 0; i < g_steer.rate_flow_count; i++) {
			struct rate_flow_state *flow = &g_steer.rate_flow[i];
			uint32_t interval_rate = flow->latest_rate;
			if (flow->interval_rate_reports != 0)
				interval_rate = (uint32_t)(flow->interval_rate_sum /
							 flow->interval_rate_reports);
			flow->interval_rate_sum = 0;
			flow->interval_rate_reports = 0;
			if (!flow->smoothed_rate_valid) {
				flow->smoothed_rate = interval_rate;
				flow->smoothed_rate_valid = true;
			} else {
				uint64_t retained = (uint64_t)flow->smoothed_rate *
				                    (PATH_RATE_EWMA_DENOMINATOR - PATH_RATE_EWMA_NUMERATOR);
				uint64_t added = (uint64_t)interval_rate * PATH_RATE_EWMA_NUMERATOR;
				flow->smoothed_rate = (uint32_t)((retained + added +
							       PATH_RATE_EWMA_DENOMINATOR / 2) /
							      PATH_RATE_EWMA_DENOMINATOR);
			}
			uint32_t control_rate = flow->smoothed_rate;
			if (!flow->classified) {
				pending_mapping++;
				continue;
			}
			uint8_t c = flow->path;
			total[c]++;
			if (control_rate == PCC_FULL_RATE)
				full[c]++;
			else {
				reduced[c]++;
				reduced_sum[c] += control_rate;
			}
		}
		atomic_flag_clear_explicit(&g_steer.rate_lock, memory_order_release);

#if DOCA_USES_LEGACY_FLOW_BACKEND
		for (uint8_t path = 0; path < NB_PATHS; path++)
			if (reduced[path] > 0)
				retire_legacy_qp1_mirror_entry(
					g_steer.legacy_qp1_filter_entry, path,
					"post-clone path-IP filter");
#endif

		uint32_t path0 = PATH_SHARE_BUCKETS / 2;
		bool all_full0 = total[0] > 0 && full[0] == total[0];
		bool all_full1 = total[1] > 0 && full[1] == total[1];
		if (total[0] > 0 && total[1] > 0) {
			if (all_full0 && !all_full1 && reduced[1] > 0)
				path0 = PATH_SHARE_BUCKETS - PATH_SHARE_MIN_BUCKETS;
			else if (all_full1 && !all_full0 && reduced[0] > 0)
				path0 = PATH_SHARE_MIN_BUCKETS;
			else if (!all_full0 && !all_full1 && reduced[0] > 0 && reduced[1] > 0) {
				uint64_t sum = reduced_sum[0] + reduced_sum[1];
				if (sum > 0)
					path0 = (uint32_t)((reduced_sum[0] * PATH_SHARE_BUCKETS + sum / 2) / sum);
				if (path0 < PATH_SHARE_MIN_BUCKETS)
					path0 = PATH_SHARE_MIN_BUCKETS;
				if (path0 > PATH_SHARE_BUCKETS - PATH_SHARE_MIN_BUCKETS)
					path0 = PATH_SHARE_BUCKETS - PATH_SHARE_MIN_BUCKETS;
			}
		}
		DOCA_LOG_INFO("PCC path-share diagnostic: path0 total=%u full=%u reduced=%u sum=%lu avg=%lu; "
		              "path1 total=%u full=%u reduced=%u sum=%lu avg=%lu; pending-map=%u; "
		              "target path0=%u/64 path1=%u/64; applied path0=%u/64",
		              total[0], full[0], reduced[0], reduced_sum[0],
		              reduced[0] ? reduced_sum[0] / reduced[0] : 0,
		              total[1], full[1], reduced[1], reduced_sum[1],
		              reduced[1] ? reduced_sum[1] / reduced[1] : 0,
		              pending_mapping, path0, PATH_SHARE_BUCKETS - path0,
		              g_steer.applied_path0_share);
		apply_path_share(path0);
	}

	struct steer_resource_query q;
	if (g_steer.path_rewrite_entry[0] != NULL || g_steer.path_rewrite_entry[1] != NULL) {
		uint64_t assigned[NB_PATHS] = {0};
		for (uint8_t path = 0; path < NB_PATHS; path++)
			if (g_steer.path_rewrite_entry[path] != NULL &&
			    steer_query_entry(g_steer.path_rewrite_entry[path], &q) == DOCA_SUCCESS)
				assigned[path] = q.total_pkts;
		DOCA_LOG_INFO("egress assigned counters: path0=%lu path1=%lu ratio=%u:%u buckets",
		              assigned[0], assigned[1], g_steer.applied_path0_share,
		              PATH_SHARE_BUCKETS - g_steer.applied_path0_share);
	}
	if (g_steer.cnp_count_pipe != NULL) {
		uint64_t cnp_by_path[NB_PATHS] = {0};
		for (uint32_t i = 0; i < g_steer.cnp_entry_count; i++) {
			if (g_steer.cnp_entry[i] != NULL &&
			    steer_query_entry(g_steer.cnp_entry[i], &q) == DOCA_SUCCESS)
				cnp_by_path[g_steer.cnp_path[i]] += q.total_pkts;
		}
		uint64_t now_cycles = rte_get_timer_cycles();
		if (g_steer.cnp_delta_ready && now_cycles > g_steer.cnp_prev_cycles) {
			double seconds = (double)(now_cycles - g_steer.cnp_prev_cycles) /
			                 (double)rte_get_timer_hz();
			uint64_t delta0 = cnp_by_path[0] - g_steer.cnp_prev_pkts[0];
			uint64_t delta1 = cnp_by_path[1] - g_steer.cnp_prev_pkts[1];
			DOCA_LOG_INFO("sender received raw CNP: path0=%lu (+%lu, %.0f/s) "
			              "path1=%lu (+%lu, %.0f/s)",
			              cnp_by_path[0], delta0, (double)delta0 / seconds,
			              cnp_by_path[1], delta1, (double)delta1 / seconds);
		} else {
			DOCA_LOG_INFO("sender received raw CNP baseline: path0=%lu path1=%lu",
			              cnp_by_path[0], cnp_by_path[1]);
		}
		for (int path = 0; path < NB_PATHS; path++)
			g_steer.cnp_prev_pkts[path] = cnp_by_path[path];
		g_steer.cnp_prev_cycles = now_cycles;
		g_steer.cnp_delta_ready = true;
	}

	uint64_t ingress_bytes[NB_PATHS] = {0};
	uint64_t ingress_mark_pkts[NB_PATHS] = {0};
	bool ingress_bytes_valid = true;
	for (int i = 0; i < NB_PATHS; i++) {
		if (g_steer.path_demux_entry[i] == NULL ||
		    steer_query_entry(g_steer.path_demux_entry[i], &q) != DOCA_SUCCESS)
			ingress_bytes_valid = false;
		else
			ingress_bytes[i] = q.total_bytes;
		if (g_steer.mark_entry[i] &&
		    steer_query_entry(g_steer.mark_entry[i], &q) == DOCA_SUCCESS)
			ingress_mark_pkts[i] = q.total_pkts;
	}
	if (ingress_bytes_valid) {
		uint64_t now_cycles = rte_get_timer_cycles();
		if (g_steer.ingress_throughput_ready && now_cycles > g_steer.ingress_prev_cycles) {
			double seconds = (double)(now_cycles - g_steer.ingress_prev_cycles) /
			                 (double)rte_get_timer_hz();
			uint64_t mark_delta0 = ingress_mark_pkts[0] - g_steer.ingress_prev_mark_pkts[0];
			uint64_t mark_delta1 = ingress_mark_pkts[1] - g_steer.ingress_prev_mark_pkts[1];
			DOCA_LOG_INFO("ingress newly CE-marked: path0=%lu (+%lu, %.0f/s) "
			              "path1=%lu (+%lu, %.0f/s)",
			              ingress_mark_pkts[0], mark_delta0, (double)mark_delta0 / seconds,
			              ingress_mark_pkts[1], mark_delta1, (double)mark_delta1 / seconds);
			double path0_gbps = (double)(ingress_bytes[0] - g_steer.ingress_prev_bytes[0]) * 8.0 /
			                    seconds / 1.0e9;
			double path1_gbps = (double)(ingress_bytes[1] - g_steer.ingress_prev_bytes[1]) * 8.0 /
			                    seconds / 1.0e9;
			DOCA_LOG_INFO("ingress throughput: path0=%.3f Gbps path1=%.3f Gbps total=%.3f Gbps",
			              path0_gbps, path1_gbps, path0_gbps + path1_gbps);
		}
		for (int i = 0; i < NB_PATHS; i++) {
			g_steer.ingress_prev_bytes[i] = ingress_bytes[i];
			g_steer.ingress_prev_mark_pkts[i] = ingress_mark_pkts[i];
		}
		g_steer.ingress_prev_cycles = now_cycles;
		g_steer.ingress_throughput_ready = true;
	}
}

void steer_stop(void)
{
	if (!g_steer.started)
		return;

	/* Prevent any later host callback from consulting Flow objects while they
	 * are being removed. PCC is stopped by the embedding application first. */
	g_steer.started = false;
	g_steer.grouping_enabled = false;
	g_steer.cnp_count_pipe = NULL;
	g_steer.classify_pipe = NULL;

	/* Every pipe belongs to the proxy, so one flush tears down the pipeline.
	 * Representors are child ports and must be stopped before their proxy parent.
	 * Empty representor ports must not be flushed on the DOCA 3.4 dual-SF path. */
	if (g_steer.port) {
		doca_error_t err = doca_flow_entries_process(g_steer.port, 0, 100000, 0);
		if (err != DOCA_SUCCESS)
			DOCA_LOG_WARN("failed to drain Flow operations before flush: %s",
			              doca_error_get_descr(err));
		doca_flow_port_pipes_flush(g_steer.port);
	}
	for (int path = NB_PATHS - 1; path >= 0; path--) {
		if (g_steer.sf_rep_port[path]) {
			DOCA_LOG_INFO("stopping SF%d Flow port", path);
			doca_error_t err = doca_flow_port_stop(g_steer.sf_rep_port[path]);
			if (err != DOCA_SUCCESS)
				DOCA_LOG_WARN("failed to stop SF%d Flow port: %s", path,
				              doca_error_get_descr(err));
			g_steer.sf_rep_port[path] = NULL;
		}
	}
	if (g_steer.port) {
		DOCA_LOG_INFO("stopping proxy Flow port");
		doca_error_t err = doca_flow_port_stop(g_steer.port);
		if (err != DOCA_SUCCESS)
			DOCA_LOG_WARN("failed to stop proxy Flow port: %s", doca_error_get_descr(err));
		g_steer.port = NULL;
	}
	doca_flow_destroy();
	memset(&g_steer, 0, sizeof(g_steer));
}
