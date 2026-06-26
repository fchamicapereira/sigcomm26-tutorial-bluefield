#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_dpdk.h>
#include <doca_flow.h>
#include <doca_log.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

DOCA_LOG_REGISTER(FLOW_ECN);

#define NB_QUEUES 1
#define NB_ENTRIES 1 /* single CE-mark entry: all IPv4 wire ingress → CE, regardless of ECT */

/*
 * mlx5_2 (PF0 SF0) real MAC. DOCA rewrites the dst MAC in every packet forwarded
 * to port 1 (mlx5_2's vport) so the ibverbs hardware QP steering rule — which
 * matches on dst MAC — fires and delivers the packet to the QP's receive queue.
 * Without this rewrite the fake dst MAC used by the sender (to prevent the PF1 FDB
 * L2 shortcircuit) causes the ibverbs rule to miss and all incoming packets are
 * silently discarded before they reach the ibverbs layer.
 */
static const uint8_t RECEIVER_MAC[6] = {0x02, 0xc9, 0xc8, 0xff, 0xa6, 0x24};

static volatile bool g_running = true;

static void signal_handler(int signum) {
  if (signum == SIGINT || signum == SIGTERM) {
    g_running = false;
  }
}

/* Log at CRIT level and terminate if err != DOCA_SUCCESS — mirrors rte_exit(). */
static __attribute__((format(printf, 2, 3))) void crash_if_unsuccessful(doca_error_t err, const char *fmt, ...) {
  if (err == DOCA_SUCCESS) {
    return;
  }
  char msg[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  DOCA_LOG_CRIT("%s: %s", msg, doca_error_get_descr(err));

  exit(EXIT_FAILURE);
}

/* DOCA Flow global entry-process callback — updates a per-batch status struct */
struct entry_batch_status {
  bool failure;
  uint32_t nb_processed;
};

static void entry_process_cb(struct doca_flow_pipe_entry *entry, uint16_t pipe_queue, enum doca_flow_entry_status status,
                             enum doca_flow_entry_op op, void *user_ctx) {
  (void)entry;
  (void)pipe_queue;
  (void)op;
  struct entry_batch_status *s = user_ctx;

  if (s == NULL) {
    return;
  }
  if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS) {
    s->failure = true;
  }
  s->nb_processed++;
}

/*
 * EAL callback: add a dummy -a allowlist entry so EAL does not auto-probe any real PCI devices.
 * Real devices are attached explicitly via doca_dpdk_port_probe after doca_dev_open.
 * This matches the DOCA SDK pattern used in flow_ct_dpdk_init.
 */
static doca_error_t initialize_dpdk(int argc, char **argv) {
  static char allow_flag[] = "-a";
  static char dummy_pci[] = "pci:00:00.0";
  char *new_argv[64];

  if (argc >= 62) {
    DOCA_LOG_ERR("Too many EAL arguments");
    return DOCA_ERROR_INVALID_VALUE;
  }
  for (int i = 0; i < argc; i++) {
    new_argv[i] = argv[i];
  }
  new_argv[argc] = allow_flag;
  new_argv[argc + 1] = dummy_pci;

  if (rte_eal_init(argc + 2, new_argv) < 0) {
    DOCA_LOG_ERR("EAL initialization failed");
    return DOCA_ERROR_DRIVER;
  }
  return DOCA_SUCCESS;
}

/* Open the Nth DOCA device and probe it into DPDK with caller-supplied args. */
static struct doca_dev *open_and_probe_dev(uint32_t index, const char *probe_args) {
  struct doca_devinfo **devinfo_list;
  uint32_t nb_devs;
  struct doca_dev *dev;
  doca_error_t err;

  err = doca_devinfo_create_list(&devinfo_list, &nb_devs);
  crash_if_unsuccessful(err, "doca_devinfo_create_list");

  if (index >= nb_devs) {
    DOCA_LOG_CRIT("Device index %u out of range (%u devices found)", index, nb_devs);
    exit(EXIT_FAILURE);
  }

  err = doca_dev_open(devinfo_list[index], &dev);
  crash_if_unsuccessful(err, "doca_dev_open");

  doca_devinfo_destroy_list(devinfo_list);

  err = doca_dpdk_port_probe(dev, probe_args);
  crash_if_unsuccessful(err, "doca_dpdk_port_probe (index=%u)", index);

  return dev;
}

/*
 * DPDK must be configured and started before DOCA Flow calls rte_flow_configure (HWS requirement).
 * DPDK also requires at least one RX queue to start a port.
 * This is the minimum setup needed — see gpu_packet_processing for the SDK reference.
 */
static void configure_and_start_dpdk_port(struct doca_dev *dev) {
  uint16_t first_port_id;
  doca_error_t err = doca_dpdk_get_first_port_id(dev, &first_port_id);
  crash_if_unsuccessful(err, "doca_dpdk_get_first_port_id");

  /* One shared mempool for all ports (PF + SF rep). */
  struct rte_mempool *mp =
      rte_pktmbuf_pool_create("mbuf_pool", 8192, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_eth_dev_socket_id(first_port_id));
  if (mp == NULL) {
    DOCA_LOG_CRIT("rte_pktmbuf_pool_create failed");
    exit(EXIT_FAILURE);
  }

  uint16_t port_id;
  RTE_ETH_FOREACH_DEV(port_id) {
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
        DOCA_LOG_CRIT("rte_eth_rx_queue_setup port %u q%d failed (errno %d)", port_id, q, -ret);
        exit(EXIT_FAILURE);
      }
      ret = rte_eth_tx_queue_setup(port_id, q, 512, rte_eth_dev_socket_id(port_id), &tx_conf);
      if (ret < 0) {
        DOCA_LOG_CRIT("rte_eth_tx_queue_setup port %u q%d failed (errno %d)", port_id, q, -ret);
        exit(EXIT_FAILURE);
      }
    }

    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
      DOCA_LOG_CRIT("rte_eth_dev_start port %u failed (errno %d)", port_id, -ret);
      exit(EXIT_FAILURE);
    }
  }
}

static void initialize_doca_flow(void) {
  doca_error_t err;

  struct doca_flow_cfg *cfg;
  err = doca_flow_cfg_create(&cfg);
  crash_if_unsuccessful(err, "doca_flow_cfg_create");

  err = doca_flow_cfg_set_pipe_queues(cfg, NB_QUEUES);
  crash_if_unsuccessful(err, "doca_flow_cfg_set_pipe_queues");

  // switch mode: traffic forwarded between eSwitch ports; no CPU RSS queues needed
  err = doca_flow_cfg_set_mode_args(cfg, "switch,hws");
  crash_if_unsuccessful(err, "doca_flow_cfg_set_mode_args");

  err = doca_flow_cfg_set_nr_counters(cfg, NB_ENTRIES);
  crash_if_unsuccessful(err, "doca_flow_cfg_set_nr_counters");

  err = doca_flow_cfg_set_cb_entry_process(cfg, entry_process_cb);
  crash_if_unsuccessful(err, "doca_flow_cfg_set_cb_entry_process");

  err = doca_flow_init(cfg);
  crash_if_unsuccessful(err, "doca_flow_init");

  doca_flow_cfg_destroy(cfg);
}

static struct doca_flow_port *port_start(struct doca_dev *dev) {
  uint16_t port_id;
  doca_error_t err = doca_dpdk_get_first_port_id(dev, &port_id);
  crash_if_unsuccessful(err, "doca_dpdk_get_first_port_id");

  struct doca_flow_port_cfg *cfg;
  err = doca_flow_port_cfg_create(&cfg);
  crash_if_unsuccessful(err, "doca_flow_port_cfg_create");

  err = doca_flow_port_cfg_set_dev(cfg, dev);
  crash_if_unsuccessful(err, "doca_flow_port_cfg_set_dev");

  char port_id_str[8];
  snprintf(port_id_str, sizeof(port_id_str), "%u", port_id);

  err = doca_flow_port_cfg_set_devargs(cfg, port_id_str);
  crash_if_unsuccessful(err, "doca_flow_port_cfg_set_devargs");

  struct doca_flow_port *port;
  err = doca_flow_port_start(cfg, &port);
  crash_if_unsuccessful(err, "doca_flow_port_start");

  doca_flow_port_cfg_destroy(cfg);

  return port;
}

/* Start an arbitrary DPDK port as a DOCA Flow port (used for SF representors). */
static struct doca_flow_port *rep_port_start(uint16_t dpdk_port_id) {
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

/*
 * Create a forward-only pipe (no action template) that passes all IPv4 to dest_port_id.
 *
 * Used as the miss target (INGRESS_PASSTHROUGH) for the ECN mark pipe: non-ECT and
 * already-CE wire packets reach mlx5_2 via port 1, with the dst MAC rewritten so the
 * ibverbs QP steering rule fires.
 *
 * HWS requires a non-empty match template. We declare dscp_ecn as a variable field with
 * mask=0x00, making it a wildcard — the single entry we install catches all IPv4.
 */
static struct doca_flow_pipe *
create_fwd_pipe(struct doca_flow_port *port, const char *name,
                enum doca_flow_pipe_domain domain, bool is_root, uint16_t dest_port_id,
                const uint8_t *dst_mac_rewrite)  /* NULL → forward only, non-NULL → also rewrite dst MAC */
{
  struct doca_flow_match match = {0};
  struct doca_flow_match match_mask = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = dest_port_id};
  struct doca_flow_actions mac_actions = {0}, *mac_actions_arr[1] = {&mac_actions};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  doca_error_t err;

  match.outer.l3_type      = DOCA_FLOW_L3_TYPE_IP4;
  match.outer.ip4.dscp_ecn = 0xFF; /* variable field → non-empty HWS template */
  /* match_mask.outer.ip4.dscp_ecn = 0x00 (zero mask → match any TOS) */

  err = doca_flow_pipe_cfg_create(&cfg, port);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_create (fwd pipe)");
  err = doca_flow_pipe_cfg_set_name(cfg, name);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_name (fwd pipe)");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_type (fwd pipe)");
  err = doca_flow_pipe_cfg_set_domain(cfg, domain);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_domain (fwd pipe)");
  err = doca_flow_pipe_cfg_set_is_root(cfg, is_root);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_is_root (fwd pipe)");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_nr_entries (fwd pipe)");
  err = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_match (fwd pipe)");
  if (dst_mac_rewrite != NULL) {
    memset(mac_actions.outer.eth.dst_mac, 0xFF, 6); /* variable — overridden per entry */
    err = doca_flow_pipe_cfg_set_actions(cfg, mac_actions_arr, NULL, NULL, 1);
    crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_actions (fwd pipe %s)", name);
  }

  err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
  crash_if_unsuccessful(err, "doca_flow_pipe_create (fwd pipe %s)", name);

  doca_flow_pipe_cfg_destroy(cfg);
  return pipe;
}

static void add_fwd_entry(struct doca_flow_pipe *pipe, struct doca_flow_port *port,
                          const char *label, const uint8_t *dst_mac_rewrite)
{
  struct doca_flow_match match = {0};
  struct doca_flow_actions actions = {0};
  struct doca_flow_pipe_entry *entry;
  struct entry_batch_status status = {0};
  doca_error_t err;

  match.outer.l3_type      = DOCA_FLOW_L3_TYPE_IP4;
  /* dscp_ecn left 0; mask=0x00 means any dscp_ecn matches → catches all IPv4 */

  if (dst_mac_rewrite != NULL)
    memcpy(actions.outer.eth.dst_mac, dst_mac_rewrite, 6);

  err = doca_flow_pipe_add_entry(0, pipe, &match, &actions, NULL, NULL, 0, &status, &entry);
  crash_if_unsuccessful(err, "doca_flow_pipe_add_entry (%s)", label);

  err = doca_flow_entries_process(port, 0, 10000, 1);
  crash_if_unsuccessful(err, "doca_flow_entries_process (%s)", label);

  err = (status.failure || status.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS;
  crash_if_unsuccessful(err, "fwd entry installation (%s): %u processed", label, status.nb_processed);

  DOCA_LOG_INFO("Fwd pipe '%s' ready", label);
}

/*
 * Create a non-root pipe (reached from PORT_DEMUX for p0 wire ingress) that SETs CE on
 * all IPv4 packets before delivering them to mlx5_2 — the DPU injects the congestion
 * signal regardless of the sender's ECN bits. This deliberately departs from standard
 * ECN (a router may only promote ECT→CE) so that any RoCE generator drives the PCC loop:
 * tools like ib_send_bw send Not-ECT (00) traffic and cannot reliably stamp ECT here, so
 * strict ECT→CE marking would never fire for them. Setting CE (0x03) is a valid non-zero
 * write; only SET dscp_ecn=0 is hardware-restricted.
 *
 * Non-IPv4 packets miss this pipe and go to miss_pipe (forward-only, no rewrite).
 */
static struct doca_flow_pipe *create_ecn_mark_pipe(struct doca_flow_port *port,
                                                    struct doca_flow_pipe *miss_pipe) {
  struct doca_flow_match match = {0};
  struct doca_flow_match match_mask = {0};
  struct doca_flow_actions actions = {0}, *actions_arr[1] = {&actions};
  struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
  struct doca_flow_fwd fwd = {0};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  doca_error_t err;

  /*
   * Match all IPv4 packets. dscp_ecn is wildcarded (mask 0x00) so any ECN value —
   * Not-ECT, ECT(0/1), or already-CE — hits the single entry and gets marked CE.
   */
  match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  match.outer.ip4.dscp_ecn = 0xFF;      /* wildcard field → non-empty HWS template */
  match_mask.outer.ip4.dscp_ecn = 0x00; /* mask 0 → match any dscp_ecn             */

  /* SET dscp_ecn=CE and rewrite dst MAC — exact values supplied per entry */
  actions.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  actions.outer.ip4.dscp_ecn = 0xFF;        /* variable — overridden per entry */
  memset(actions.outer.eth.dst_mac, 0xFF, 6); /* variable — overridden per entry */

  /* CE mark → deliver to SF0 rep (port 1 = en3f0pf0sf0 = mlx5_2, the ibverbs receiver) */
  fwd.type = DOCA_FLOW_FWD_PORT;
  fwd.port_id = 1;

  err = doca_flow_pipe_cfg_create(&cfg, port);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_create");

  err = doca_flow_pipe_cfg_set_name(cfg, "ECN_MARK");
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_name");

  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_type");

  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_domain");

  err = doca_flow_pipe_cfg_set_is_root(cfg, false); /* root is PORT_DEMUX; reached via FWD_PIPE */
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_is_root");

  err = doca_flow_pipe_cfg_set_nr_entries(cfg, NB_ENTRIES);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_nr_entries");

  err = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_match");

  err = doca_flow_pipe_cfg_set_actions(cfg, actions_arr, NULL, NULL, 1);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_actions");

  err = doca_flow_pipe_cfg_set_monitor(cfg, &monitor);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_monitor");

  /* Non-IPv4 miss → miss_pipe (forward-only, no rewrite) */
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = miss_pipe};
  err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
  crash_if_unsuccessful(err, "doca_flow_pipe_create");

  doca_flow_pipe_cfg_destroy(cfg);
  return pipe;
}

struct ecn_entries {
  struct doca_flow_pipe_entry *ce;
};

static struct ecn_entries add_ecn_entries(struct doca_flow_pipe *pipe, struct doca_flow_port *port) {
  struct doca_flow_match match = {0};
  struct doca_flow_actions actions = {0};
  struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
  struct entry_batch_status status = {0};
  struct ecn_entries entries;
  doca_error_t err;

  actions.action_idx = 0;
  actions.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  actions.outer.ip4.dscp_ecn = 0x03; /* SET to CE (DSCP=0, ECN=11) */
  memcpy(actions.outer.eth.dst_mac, RECEIVER_MAC, sizeof(RECEIVER_MAC));

  /* Single entry: all IPv4 (dscp_ecn wildcarded by the pipe mask) → CE; flags=0 flushes */
  match.outer.ip4.dscp_ecn = 0x00; /* don't-care: pipe match_mask is 0x00 */
  err = doca_flow_pipe_add_entry(0, pipe, &match, &actions, &monitor, NULL, 0, &status, &entries.ce);
  crash_if_unsuccessful(err, "doca_flow_pipe_add_entry CE");

  err = doca_flow_entries_process(port, 0, 10000 /* us */, NB_ENTRIES);
  crash_if_unsuccessful(err, "doca_flow_entries_process");
  err = (status.failure || status.nb_processed != NB_ENTRIES) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS;
  crash_if_unsuccessful(err, "entry installation incomplete: %u/%u processed", status.nb_processed, NB_ENTRIES);

  DOCA_LOG_INFO("ECN mark pipe ready: all IPv4 → CE on p0 ingress (sender ECN ignored)");
  return entries;
}

/*
 * Root pipe of the eSwitch FDB. Demuxes on source port (parser_meta.port_meta):
 *   port_meta == 0  (ingress from p0 wire)   -> ecn_pipe (mark CE, deliver to mlx5_2)
 *   port_meta == 1  (egress from mlx5_2 SF)   -> port 0 (p0 wire), so mlx5_2's RoCE
 *                                                ACKs/CNPs cross the cable back to mlx5_3
 *
 * The source-port match is the crux of the return path. A single root pipe in the
 * DEFAULT domain sees *all* FDB traffic, including mlx5_2's ACKs/CNPs. Routing those
 * through ECN_MARK would forward them to port 1 (looping them back to mlx5_2) — and
 * would also wrongly CE-mark them — so the sender never sees an ACK and the QP dies
 * with RETRY_EXC. Forwarding port_meta==1 straight to port 0 here keeps mlx5_2's egress
 * off the mark pipe and puts it on the wire; no EGRESS-domain pipe is needed.
 */
static struct doca_flow_pipe *create_port_demux_pipe(struct doca_flow_port *port,
                                                     struct doca_flow_pipe *ecn_pipe) {
  struct doca_flow_match match = {0};
  struct doca_flow_match match_mask = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE}; /* set per entry */
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_DROP};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  doca_error_t err;

  match.parser_meta.port_meta = UINT32_MAX;      /* variable — set per entry      */
  match_mask.parser_meta.port_meta = UINT32_MAX; /* exact match on source port id */

  err = doca_flow_pipe_cfg_create(&cfg, port);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_create (demux)");
  err = doca_flow_pipe_cfg_set_name(cfg, "PORT_DEMUX");
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_name (demux)");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_type (demux)");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_domain (demux)");
  err = doca_flow_pipe_cfg_set_is_root(cfg, true);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_is_root (demux)");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 2);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_nr_entries (demux)");
  err = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_match (demux)");

  err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
  crash_if_unsuccessful(err, "doca_flow_pipe_create (demux)");
  doca_flow_pipe_cfg_destroy(cfg);

  struct doca_flow_match entry_match = {0};
  struct doca_flow_fwd entry_fwd;
  struct entry_batch_status status = {0};
  struct doca_flow_pipe_entry *entry;

  /* port 0 (p0 wire ingress) -> ECN mark pipe; batch, flush with next entry */
  entry_match.parser_meta.port_meta = 0;
  memset(&entry_fwd, 0, sizeof(entry_fwd));
  entry_fwd.type = DOCA_FLOW_FWD_PIPE;
  entry_fwd.next_pipe = ecn_pipe;
  err = doca_flow_pipe_add_entry(0, pipe, &entry_match, NULL, NULL, &entry_fwd, DOCA_FLOW_WAIT_FOR_BATCH, &status,
                                 &entry);
  crash_if_unsuccessful(err, "doca_flow_pipe_add_entry (demux wire->ecn)");

  /* port 1 (mlx5_2 SF egress) -> p0 wire; flags=0 flushes the batch */
  entry_match.parser_meta.port_meta = 1;
  memset(&entry_fwd, 0, sizeof(entry_fwd));
  entry_fwd.type = DOCA_FLOW_FWD_PORT;
  entry_fwd.port_id = 0;
  err = doca_flow_pipe_add_entry(0, pipe, &entry_match, NULL, NULL, &entry_fwd, 0, &status, &entry);
  crash_if_unsuccessful(err, "doca_flow_pipe_add_entry (demux sf->wire)");

  err = doca_flow_entries_process(port, 0, 10000, 2);
  crash_if_unsuccessful(err, "doca_flow_entries_process (demux)");
  err = (status.failure || status.nb_processed != 2) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS;
  crash_if_unsuccessful(err, "demux entry installation: %u/2 processed", status.nb_processed);

  DOCA_LOG_INFO("Port demux ready: p0 wire->ECN_MARK, mlx5_2 SF->p0 wire");
  return pipe;
}

static void print_stats(struct ecn_entries *entries) {
  struct doca_flow_resource_query resource_query;
  doca_error_t err;

  err = doca_flow_resource_query_entry(entries->ce, &resource_query);
  crash_if_unsuccessful(err, "doca_flow_resource_query_entry CE");
  uint64_t ce_pkts = resource_query.counter.total_pkts;

  DOCA_LOG_INFO("CE marks total (all IPv4 p0 ingress): %lu", ce_pkts);
}

int main(int argc, char **argv) {
  doca_error_t err;

  err = doca_log_backend_create_standard();
  crash_if_unsuccessful(err, "doca_log_backend_create_standard");

  struct doca_log_backend *sdk_log;
  err = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
  crash_if_unsuccessful(err, "doca_log_backend_create_with_file_sdk");

  err = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
  crash_if_unsuccessful(err, "doca_log_backend_set_sdk_level");

  err = doca_argp_init("doca_flow_ecn", NULL);
  crash_if_unsuccessful(err, "doca_argp_init");

  doca_argp_set_dpdk_program(initialize_dpdk);

  err = doca_argp_start(argc, argv);
  crash_if_unsuccessful(err, "doca_argp_start");

  /* PF0 only: DOCA manages PF0's FDB via HWS root pipes (highest priority).
   * fdb_def_rule_en=1 keeps the kernel's default FDB rules for PF1 so that
   * mlx5_3's egress traffic (unknown unicast dst MAC) exits via p1 wire uplink.
   * DOCA HWS root pipes on PF0 override the kernel defaults for PF0 ingress
   * and egress, so ECN_MARK and EGRESS_FORWARD fire correctly on the PF0 side.
   * mlx5_3's ARP entry for 10.0.0.1 must use an unknown MAC (not mlx5_2's real
   * MAC) to avoid the PF1 FDB L2 shortcircuit. DOCA Flow rewrites the dst MAC
   * to RECEIVER_MAC before forwarding to port 1 so that mlx5_2's ibverbs QP
   * hardware steering rule (which matches on dst MAC) fires and delivers the
   * packet to the QP's receive queue. */
  struct doca_dev *dev = open_and_probe_dev(0,
      "dv_flow_en=2,fdb_def_rule_en=1,repr_matching_en=0,representor=sf0");

  configure_and_start_dpdk_port(dev);

  initialize_doca_flow();

  struct doca_flow_port *port        = port_start(dev);   /* DPDK 0: PF0 uplink */
  struct doca_flow_port *sf_rep_port = rep_port_start(1); /* DPDK 1: PF0 SF rep (mlx5_2) */

  /* INGRESS_PASSTHROUGH: miss target of ECN_MARK — non-ECT / already-CE wire packets
   * still reach mlx5_2 (port 1), with dst MAC rewritten so the ibverbs QP rule fires. */
  struct doca_flow_pipe *ingress_pass = create_fwd_pipe(
      port, "INGRESS_PASSTHROUGH", DOCA_FLOW_PIPE_DOMAIN_DEFAULT, false, 1, RECEIVER_MAC);
  add_fwd_entry(ingress_pass, port, "ingress passthrough", RECEIVER_MAC);

  /* ECN_MARK (non-root): marks all IPv4 p0 wire ingress → CE, forwards to mlx5_2. */
  struct doca_flow_pipe *pipe = create_ecn_mark_pipe(port, ingress_pass);
  struct ecn_entries entries = add_ecn_entries(pipe, port);

  /* PORT_DEMUX (root): splits by source port. p0 wire ingress -> ECN_MARK; mlx5_2 SF
   * egress (ACKs/CNPs) -> p0 wire so they return to mlx5_3. Replaces the old
   * EGRESS_FORWARD pipe, which never fired because the DEFAULT-domain root pipe made
   * the forwarding decision before egress processing. */
  create_port_demux_pipe(port, pipe);

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  DOCA_LOG_INFO("ECN marking active — Ctrl-C to stop");
  while (g_running) {
    sleep(1);
    print_stats(&entries);
  }

  doca_flow_port_stop(sf_rep_port);
  doca_flow_port_stop(port);
  doca_flow_destroy();
  doca_argp_destroy();

  return EXIT_SUCCESS;
}
