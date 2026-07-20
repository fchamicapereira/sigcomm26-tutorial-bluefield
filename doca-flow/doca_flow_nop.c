/*
 * doca_flow_nop — the bare minimum DOCA Flow pipeline: forwards p0 wire ingress straight to
 * the receiver's SF (mlx5_2) and routes the SF's egress (ACKs/CNPs) back out the wire. No
 * header-modify action at all.
 *
 * Two purposes:
 *   1. Performance control group — measures the pipeline's raw forwarding cost with zero
 *      actions attached, to compare against doca_flow_mac / doca_flow_ecn (see the throughput
 *      bottleneck investigation).
 *   2. Base file for tutorial participants to build their own pipeline on top of.
 *
 * Same environment as doca_flow_mac / doca_flow_ecn — see setup_roce_loopback.sh / README.md.
 */
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

DOCA_LOG_REGISTER(FLOW_NOP);

#define NB_QUEUES 1
#define NB_COUNTERS 1 /* single forward entry, counted so its traffic is observable */

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

  err = doca_flow_cfg_set_nr_counters(cfg, NB_COUNTERS);
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
 * Forward-only pipe: passes all IPv4 wire ingress to dest_port_id, completely untouched.
 * No action template at all — this is the whole point of doca_flow_nop.
 *
 * HWS still requires a non-empty match template. dscp_ecn is declared as a variable field
 * with mask=0x00, making it a wildcard purely to satisfy that requirement — it carries no
 * ECN meaning here.
 */
static struct doca_flow_pipe *create_fwd_pipe(struct doca_flow_port *port, uint16_t dest_port_id, bool with_counter) {
  struct doca_flow_match match = {0};
  struct doca_flow_match match_mask = {0};
  struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = dest_port_id};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  doca_error_t err;

  match.outer.l3_type      = DOCA_FLOW_L3_TYPE_IP4;
  match.outer.ip4.dscp_ecn = 0xFF; /* variable field → non-empty HWS template */
  /* match_mask.outer.ip4.dscp_ecn = 0x00 (zero mask → match any TOS) */

  err = doca_flow_pipe_cfg_create(&cfg, port);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_create (fwd pipe)");
  err = doca_flow_pipe_cfg_set_name(cfg, "FORWARD");
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_name (fwd pipe)");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_type (fwd pipe)");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_domain (fwd pipe)");
  err = doca_flow_pipe_cfg_set_is_root(cfg, false); /* root is PORT_DEMUX; reached via FWD_PIPE */
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_is_root (fwd pipe)");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_nr_entries (fwd pipe)");
  err = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
  crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_match (fwd pipe)");
  if (with_counter) {
    err = doca_flow_pipe_cfg_set_monitor(cfg, &monitor);
    crash_if_unsuccessful(err, "doca_flow_pipe_cfg_set_monitor (fwd pipe)");
  }

  err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
  crash_if_unsuccessful(err, "doca_flow_pipe_create (fwd pipe)");

  doca_flow_pipe_cfg_destroy(cfg);
  return pipe;
}

static struct doca_flow_pipe_entry *add_fwd_entry(struct doca_flow_pipe *pipe, struct doca_flow_port *port,
                                                  bool with_counter) {
  struct doca_flow_match match = {0};
  struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
  struct doca_flow_pipe_entry *entry;
  struct entry_batch_status status = {0};
  doca_error_t err;

  match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  /* dscp_ecn left 0; mask=0x00 means any dscp_ecn matches → catches all IPv4 */

  err = doca_flow_pipe_basic_add_entry(0, pipe, &match, 0, NULL, with_counter ? &monitor : NULL, NULL, 0, &status,
                                       &entry);
  crash_if_unsuccessful(err, "doca_flow_pipe_basic_add_entry (fwd)");

  err = doca_flow_entries_process(port, 0, 10000, 1);
  crash_if_unsuccessful(err, "doca_flow_entries_process (fwd)");

  err = (status.failure || status.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS;
  crash_if_unsuccessful(err, "fwd entry installation: %u processed", status.nb_processed);

  DOCA_LOG_INFO("Forward pipe ready");
  return entry;
}

/*
 * Root pipe of the eSwitch FDB. Demuxes on source port (parser_meta.port_id):
 *   port_id == 0  (ingress from p0 wire)   -> the FORWARD pipe (straight to mlx5_2)
 *   port_id == 1  (egress from mlx5_2 SF)   -> port 0 (p0 wire), so mlx5_2's RoCE
 *                                                ACKs/CNPs cross the cable back to mlx5_3
 *
 * See doca_flow_ecn.c's create_port_demux_pipe for the full rationale (same design).
 */
static struct doca_flow_pipe *create_port_demux_pipe(struct doca_flow_port *port, struct doca_flow_pipe *fwd_pipe) {
  struct doca_flow_match match = {0};
  struct doca_flow_match match_mask = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE}; /* set per entry */
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_DROP};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  doca_error_t err;

  match.parser_meta.port_id = UINT16_MAX;      /* variable — set per entry      */
  match_mask.parser_meta.port_id = UINT16_MAX; /* exact match on source port id */

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

  /* port 0 (p0 wire ingress) -> FORWARD; batch, flush with next entry */
  entry_match.parser_meta.port_id = 0;
  memset(&entry_fwd, 0, sizeof(entry_fwd));
  entry_fwd.type = DOCA_FLOW_FWD_PIPE;
  entry_fwd.next_pipe = fwd_pipe;
  err = doca_flow_pipe_basic_add_entry(0, pipe, &entry_match, 0, NULL, NULL, &entry_fwd,
                                       DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH, &status, &entry);
  crash_if_unsuccessful(err, "doca_flow_pipe_basic_add_entry (demux wire->fwd)");

  /* port 1 (mlx5_2 SF egress) -> p0 wire; flags=0 flushes the batch */
  entry_match.parser_meta.port_id = 1;
  memset(&entry_fwd, 0, sizeof(entry_fwd));
  entry_fwd.type = DOCA_FLOW_FWD_PORT;
  entry_fwd.port_id = 0;
  err = doca_flow_pipe_basic_add_entry(0, pipe, &entry_match, 0, NULL, NULL, &entry_fwd, 0, &status, &entry);
  crash_if_unsuccessful(err, "doca_flow_pipe_basic_add_entry (demux sf->wire)");

  err = doca_flow_entries_process(port, 0, 10000, 2);
  crash_if_unsuccessful(err, "doca_flow_entries_process (demux)");
  err = (status.failure || status.nb_processed != 2) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS;
  crash_if_unsuccessful(err, "demux entry installation: %u/2 processed", status.nb_processed);

  DOCA_LOG_INFO("Port demux ready: p0 wire->FORWARD, mlx5_2 SF->p0 wire");
  return pipe;
}

static void print_stats(struct doca_flow_pipe_entry *fwd_entry) {
  struct doca_flow_resource_query query;
  doca_error_t err = doca_flow_resource_query_entry(fwd_entry, &query);
  crash_if_unsuccessful(err, "doca_flow_resource_query_entry");

  DOCA_LOG_INFO("Forwarded: %lu", query.counter.total_pkts);
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

  err = doca_argp_init("doca_flow_nop", NULL);
  crash_if_unsuccessful(err, "doca_argp_init");

  doca_argp_set_dpdk_program(initialize_dpdk);

  err = doca_argp_start(argc, argv);
  crash_if_unsuccessful(err, "doca_argp_start");

  /* PF0 only: DOCA manages PF0's FDB via HWS root pipes (highest priority).
   * fdb_def_rule_en=1 keeps the kernel's default FDB rules for PF1 so that
   * mlx5_3's egress traffic exits via p1 wire uplink. */
  struct doca_dev *dev = open_and_probe_dev(0,
      "dv_flow_en=2,fdb_def_rule_en=1,repr_matching_en=0,representor=sf0");

  configure_and_start_dpdk_port(dev);

  initialize_doca_flow();

  struct doca_flow_port *port        = port_start(dev);   /* DPDK 0: PF0 uplink */
  struct doca_flow_port *sf_rep_port = rep_port_start(1); /* DPDK 1: PF0 SF rep (mlx5_2) */

  struct doca_flow_pipe *fwd_pipe = create_fwd_pipe(port, 1, /*with_counter=*/true);
  struct doca_flow_pipe_entry *fwd_entry = add_fwd_entry(fwd_pipe, port, /*with_counter=*/true);

  struct doca_flow_pipe *demux_pipe = create_port_demux_pipe(port, fwd_pipe);
  (void)demux_pipe;

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  DOCA_LOG_INFO("doca_flow_nop: forwarding all IPv4 p0 wire ingress untouched — Ctrl-C to stop");

  while (g_running) {
    sleep(1);
    print_stats(fwd_entry);
  }

  doca_flow_port_stop(sf_rep_port);
  doca_flow_port_stop(port);
  doca_flow_destroy();
  doca_argp_destroy();

  return EXIT_SUCCESS;
}
