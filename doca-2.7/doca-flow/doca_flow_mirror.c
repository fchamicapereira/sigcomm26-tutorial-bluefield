/*
 * doca_flow_mirror — mirror wire-ingress packets to an RSS queue and write them to a pcap.
 *
 * Same eSwitch topology as doca_flow_ecn.c (PF0 switch/FDB mode):
 *   PORT_DEMUX (root, matches source port):
 *     port_meta 0 (p0 wire ingress) -> CAPTURE pipe
 *     port_meta 1 (mlx5_2 SF egress) -> p0 wire        (return path for ACKs/CNPs)
 *   CAPTURE pipe: all IPv4 -> mlx5_2 (port 1), AND a COPY is mirrored to RSS queue 0.
 *                 non-IPv4 miss -> PASSTHROUGH (port 1, no mirror).
 * A CPU RX loop drains RSS queue 0 and appends every packet to <pcap-file>.
 *
 * The original data path is untouched (traffic still reaches mlx5_2); the mirror only adds a copy.
 */
#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_dpdk.h>
#include <doca_flow.h>
#include <doca_log.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <pcap.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

DOCA_LOG_REGISTER(FLOW_MIRROR);

#define NB_QUEUES 1
#define MIRROR_ID 1     /* shared-mirror resource id (must be >= 1; 0 means "no mirror") */
#define RX_BURST 64
#define SNAPLEN 262144  /* pcap per-packet capture length cap */

struct mirror_app_config {
  const char *pcap_path; /* required: output pcap file (--pcap)                          */
  uint32_t sample_1_in;  /* mirror ~1 of every N packets (--sample N); 1 = capture all   */
};

/* parser_meta.random is a 16-bit HW per-packet value. Matching value 0 under a mask of
 * (2^i - 1) selects 1/2^i of packets. Sampling therefore only supports power-of-two rates;
 * pick the 2^i closest to the requested N (clamped to [2, 65536]) and return the mask. */
static uint16_t mask_for_one_in_n(uint32_t n, uint32_t *actual_n) {
  uint32_t best_pow = 2;
  uint64_t best_dist = UINT64_MAX;
  for (uint32_t i = 1; i <= 16; i++) {
    uint32_t p = 1u << i; /* 2 .. 65536 */
    uint64_t d = (p > n) ? (p - n) : (n - p);
    if (d < best_dist) {
      best_dist = d;
      best_pow = p;
    }
  }
  *actual_n = best_pow;
  return (uint16_t)(best_pow - 1);
}

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

/* EAL callback: dummy -a allowlist so EAL does not auto-probe real PCI devices (attached later). */
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

/* DPDK ports must be configured/started before DOCA Flow (HWS). RSS delivery needs the RX queue. */
static void configure_and_start_dpdk_port(struct doca_dev *dev) {
  uint16_t first_port_id;
  doca_error_t err = doca_dpdk_get_first_port_id(dev, &first_port_id);
  crash_if_unsuccessful(err, "doca_dpdk_get_first_port_id");

  struct rte_mempool *mp =
      rte_pktmbuf_pool_create("mbuf_pool", 8192, 256, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_eth_dev_socket_id(first_port_id));
  if (mp == NULL) {
    DOCA_LOG_CRIT("rte_pktmbuf_pool_create failed");
    exit(EXIT_FAILURE);
  }

  uint16_t port_id;
  RTE_ETH_FOREACH_DEV(port_id) {
    struct rte_eth_dev_info dev_info = {0};
    if (rte_eth_dev_info_get(port_id, &dev_info) < 0) {
      DOCA_LOG_CRIT("rte_eth_dev_info_get port %u failed", port_id);
      exit(EXIT_FAILURE);
    }
    struct rte_eth_conf eth_conf = {0};
    if (rte_eth_dev_configure(port_id, NB_QUEUES, NB_QUEUES, &eth_conf) < 0) {
      DOCA_LOG_CRIT("rte_eth_dev_configure port %u failed", port_id);
      exit(EXIT_FAILURE);
    }
    struct rte_eth_txconf tx_conf = dev_info.default_txconf;
    for (int q = 0; q < NB_QUEUES; q++) {
      if (rte_eth_rx_queue_setup(port_id, q, 1024, rte_eth_dev_socket_id(port_id), NULL, mp) < 0) {
        DOCA_LOG_CRIT("rte_eth_rx_queue_setup port %u q%d failed", port_id, q);
        exit(EXIT_FAILURE);
      }
      if (rte_eth_tx_queue_setup(port_id, q, 512, rte_eth_dev_socket_id(port_id), &tx_conf) < 0) {
        DOCA_LOG_CRIT("rte_eth_tx_queue_setup port %u q%d failed", port_id, q);
        exit(EXIT_FAILURE);
      }
    }
    if (rte_eth_dev_start(port_id) < 0) {
      DOCA_LOG_CRIT("rte_eth_dev_start port %u failed", port_id);
      exit(EXIT_FAILURE);
    }
  }
}

static void initialize_doca_flow(void) {
  struct doca_flow_cfg *cfg;
  doca_error_t err = doca_flow_cfg_create(&cfg);
  crash_if_unsuccessful(err, "doca_flow_cfg_create");

  err = doca_flow_cfg_set_pipe_queues(cfg, NB_QUEUES);
  crash_if_unsuccessful(err, "doca_flow_cfg_set_pipe_queues");
  /* disable_switch_rss: same fix doca_flow_ecn/_nop/_mac use (see doca_flow_ecn.c's comment) — it
   * only skips DOCA Flow's own eager, automatic internal FDB RSS suffix context built at port
   * start, which some firmware (confirmed on this tutorial's DPU) rejects outright. It does NOT
   * block this program's own explicit RSS pipe below (create_rss_pipe/setup_capture_mirror) --
   * that one is properly scoped and works fine once the eager one is out of the way. Without this
   * flag, doca_flow_port_start fails before we ever get to build our own pipe. */
  err = doca_flow_cfg_set_mode_args(cfg, "switch,hws,isolated,disable_switch_rss");
  crash_if_unsuccessful(err, "doca_flow_cfg_set_mode_args");
  err = doca_flow_cfg_set_nr_counters(cfg, 2);
  crash_if_unsuccessful(err, "doca_flow_cfg_set_nr_counters");
  /* shared-mirror resources: ids are 0-indexed and id 0 is the "no-mirror" sentinel, so to use
   * MIRROR_ID=1 we must reserve at least 2 (valid ids then span [0, nr-1] = [0,1]). */
  err = doca_flow_cfg_set_nr_shared_resource(cfg, MIRROR_ID + 1, DOCA_FLOW_SHARED_RESOURCE_MIRROR);
  crash_if_unsuccessful(err, "doca_flow_cfg_set_nr_shared_resource (mirror)");
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

static struct doca_flow_port *rep_port_start(uint16_t dpdk_port_id) {
  struct doca_flow_port_cfg *cfg;
  char port_id_str[8];
  snprintf(port_id_str, sizeof(port_id_str), "%u", dpdk_port_id);

  doca_error_t err = doca_flow_port_cfg_create(&cfg);
  crash_if_unsuccessful(err, "doca_flow_port_cfg_create (rep %u)", dpdk_port_id);
  err = doca_flow_port_cfg_set_devargs(cfg, port_id_str);
  crash_if_unsuccessful(err, "doca_flow_port_cfg_set_devargs (rep %u)", dpdk_port_id);

  struct doca_flow_port *port;
  err = doca_flow_port_start(cfg, &port);
  crash_if_unsuccessful(err, "doca_flow_port_start (rep %u)", dpdk_port_id);
  doca_flow_port_cfg_destroy(cfg);
  return port;
}

/*
 * Leaf pipe that RSSes matched packets to CPU RX queue 0. A mirror target cannot carry an inline
 * RSS fwd (HWS rejects it as "invalid mirror list format"); it must point at a PORT or a PIPE. So
 * the mirror targets THIS pipe, and this pipe does the RSS. Match = all IPv4 (RoCEv2 is IPv4/UDP).
 */
static struct doca_flow_pipe *create_rss_pipe(struct doca_flow_port *port) {
  static uint16_t rss_queues[1] = {0};
  struct doca_flow_match match = {0};
  struct doca_flow_fwd fwd = {0};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  struct doca_flow_pipe_entry *entry;
  struct entry_batch_status status = {0};
  doca_error_t err;

  match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;

  fwd.type = DOCA_FLOW_FWD_RSS;
  fwd.rss_queues = rss_queues;
  fwd.num_of_queues = 1;
  fwd.rss_outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP; /* RoCEv2 = UDP/4791 */

  err = doca_flow_pipe_cfg_create(&cfg, port);
  crash_if_unsuccessful(err, "pipe_cfg_create (rss)");
  err = doca_flow_pipe_cfg_set_name(cfg, "MIRROR_RSS");
  crash_if_unsuccessful(err, "pipe_cfg_set_name (rss)");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  crash_if_unsuccessful(err, "pipe_cfg_set_type (rss)");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  crash_if_unsuccessful(err, "pipe_cfg_set_domain (rss)");
  err = doca_flow_pipe_cfg_set_is_root(cfg, false);
  crash_if_unsuccessful(err, "pipe_cfg_set_is_root (rss)");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
  crash_if_unsuccessful(err, "pipe_cfg_set_nr_entries (rss)");
  err = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
  crash_if_unsuccessful(err, "pipe_cfg_set_match (rss)");

  err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
  crash_if_unsuccessful(err, "pipe_create (rss)");
  doca_flow_pipe_cfg_destroy(cfg);

  err = doca_flow_pipe_add_entry(0, pipe, &match, NULL, NULL, NULL, 0, &status, &entry);
  crash_if_unsuccessful(err, "pipe_add_entry (rss)");
  err = doca_flow_entries_process(port, 0, 10000, 1);
  crash_if_unsuccessful(err, "entries_process (rss)");
  err = (status.failure || status.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS;
  crash_if_unsuccessful(err, "rss entry install: %u processed", status.nb_processed);
  DOCA_LOG_INFO("RSS pipe ready: matched IPv4 -> CPU RX queue 0");
  return pipe;
}

/*
 * Configure shared mirror MIRROR_ID to copy matched packets to the RSS pipe (which delivers them to
 * CPU queue 0), and bind it to the port. The mirror only makes a COPY — the original still follows
 * the capture pipe's own fwd, so mcfg.fwd is left empty.
 */
static void setup_capture_mirror(struct doca_flow_port *port, struct doca_flow_pipe *rss_pipe) {
  struct doca_flow_mirror_target target = {0};
  struct doca_flow_resource_mirror_cfg mirror_cfg = {0};
  struct doca_flow_shared_resource_cfg cfg = {0};
  uint32_t ids[1] = {MIRROR_ID};
  doca_error_t err;

  target.fwd.type = DOCA_FLOW_FWD_PIPE; /* mirror -> RSS pipe -> CPU queue 0 */
  target.fwd.next_pipe = rss_pipe;

  mirror_cfg.nr_targets = 1;
  mirror_cfg.target = &target;
  cfg.mirror_cfg = mirror_cfg;

  err = doca_flow_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_MIRROR, MIRROR_ID, &cfg);
  crash_if_unsuccessful(err, "doca_flow_shared_resource_set_cfg (mirror->RSS pipe)");
  err = doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_MIRROR, ids, 1, port);
  crash_if_unsuccessful(err, "doca_flow_shared_resources_bind (mirror)");
  DOCA_LOG_INFO("Shared mirror %u -> RSS pipe -> queue 0 configured", MIRROR_ID);
}

/*
 * Forward-only pipe: all IPv4 -> dest_port_id, untouched. Used as the CAPTURE miss target so
 * non-IPv4 still reaches mlx5_2. (No mirror, no counter.)
 */
static struct doca_flow_pipe *create_passthrough_pipe(struct doca_flow_port *port, uint16_t dest_port_id) {
  struct doca_flow_match match = {0};
  struct doca_flow_match match_mask = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = dest_port_id};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  struct doca_flow_pipe_entry *entry;
  struct entry_batch_status status = {0};
  doca_error_t err;

  match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  match.outer.ip4.dscp_ecn = 0xFF; /* variable field -> non-empty HWS template */

  err = doca_flow_pipe_cfg_create(&cfg, port);
  crash_if_unsuccessful(err, "pipe_cfg_create (passthrough)");
  err = doca_flow_pipe_cfg_set_name(cfg, "PASSTHROUGH");
  crash_if_unsuccessful(err, "pipe_cfg_set_name (passthrough)");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  crash_if_unsuccessful(err, "pipe_cfg_set_type (passthrough)");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  crash_if_unsuccessful(err, "pipe_cfg_set_domain (passthrough)");
  err = doca_flow_pipe_cfg_set_is_root(cfg, false);
  crash_if_unsuccessful(err, "pipe_cfg_set_is_root (passthrough)");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
  crash_if_unsuccessful(err, "pipe_cfg_set_nr_entries (passthrough)");
  err = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
  crash_if_unsuccessful(err, "pipe_cfg_set_match (passthrough)");

  err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
  crash_if_unsuccessful(err, "pipe_create (passthrough)");
  doca_flow_pipe_cfg_destroy(cfg);

  match.outer.ip4.dscp_ecn = 0x00; /* don't-care (mask 0) */
  err = doca_flow_pipe_add_entry(0, pipe, &match, NULL, NULL, NULL, 0, &status, &entry);
  crash_if_unsuccessful(err, "pipe_add_entry (passthrough)");
  err = doca_flow_entries_process(port, 0, 10000, 1);
  crash_if_unsuccessful(err, "entries_process (passthrough)");
  err = (status.failure || status.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS;
  crash_if_unsuccessful(err, "passthrough entry install: %u processed", status.nb_processed);
  return pipe;
}

/*
 * CAPTURE pipe: match all IPv4, forward to mlx5_2 (port 1) as normal, and (via the monitor's
 * shared_mirror_id) mirror a COPY to RSS queue 0. Counter attached so we can report how many were
 * captured. Non-IPv4 miss -> passthrough.
 */
static struct doca_flow_pipe *create_capture_pipe(struct doca_flow_port *port, struct doca_flow_pipe *miss_pipe,
                                                  struct doca_flow_pipe_entry **out_entry) {
  struct doca_flow_match match = {0};
  struct doca_flow_match match_mask = {0};
  struct doca_flow_monitor monitor = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = 1};
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = miss_pipe};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  struct entry_batch_status status = {0};
  doca_error_t err;

  match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  match.outer.ip4.dscp_ecn = 0xFF;      /* wildcard field -> non-empty template */
  match_mask.outer.ip4.dscp_ecn = 0x00; /* mask 0 -> match any IPv4             */

  monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
  monitor.shared_mirror_id = MIRROR_ID; /* mirror a copy to RSS q0 */

  err = doca_flow_pipe_cfg_create(&cfg, port);
  crash_if_unsuccessful(err, "pipe_cfg_create (capture)");
  err = doca_flow_pipe_cfg_set_name(cfg, "CAPTURE");
  crash_if_unsuccessful(err, "pipe_cfg_set_name (capture)");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  crash_if_unsuccessful(err, "pipe_cfg_set_type (capture)");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  crash_if_unsuccessful(err, "pipe_cfg_set_domain (capture)");
  err = doca_flow_pipe_cfg_set_is_root(cfg, false);
  crash_if_unsuccessful(err, "pipe_cfg_set_is_root (capture)");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
  crash_if_unsuccessful(err, "pipe_cfg_set_nr_entries (capture)");
  err = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
  crash_if_unsuccessful(err, "pipe_cfg_set_match (capture)");
  err = doca_flow_pipe_cfg_set_monitor(cfg, &monitor);
  crash_if_unsuccessful(err, "pipe_cfg_set_monitor (capture)");

  err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
  crash_if_unsuccessful(err, "pipe_create (capture)");
  doca_flow_pipe_cfg_destroy(cfg);

  /* single entry — repeat shared_mirror_id at entry level too (matches flow_shared_mirror sample) */
  match.outer.ip4.dscp_ecn = 0x00;
  err = doca_flow_pipe_add_entry(0, pipe, &match, NULL, &monitor, NULL, 0, &status, out_entry);
  crash_if_unsuccessful(err, "pipe_add_entry (capture)");
  err = doca_flow_entries_process(port, 0, 10000, 1);
  crash_if_unsuccessful(err, "entries_process (capture)");
  err = (status.failure || status.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS;
  crash_if_unsuccessful(err, "capture entry install: %u processed", status.nb_processed);

  DOCA_LOG_INFO("Capture pipe ready: all IPv4 -> mlx5_2, copy -> RSS q0");
  return pipe;
}

/*
 * Pre-filter pipe: match ONLY parser_meta.random (no header fields, no actions). The selected
 * ~1/N fraction -> hit_pipe (CAPTURE: mirror+fwd); everything else -> miss_pipe (PASSTHROUGH:
 * fwd only, no mirror). Keeps the mirror volume low while the full data path still reaches mlx5_2.
 */
static struct doca_flow_pipe *create_random_sample_pipe(struct doca_flow_port *port, struct doca_flow_pipe *hit_pipe,
                                                        struct doca_flow_pipe *miss_pipe, uint16_t random_mask) {
  struct doca_flow_match match = {0};
  struct doca_flow_match match_mask = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = hit_pipe};
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = miss_pipe};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  struct doca_flow_pipe_entry *entry;
  struct entry_batch_status status = {0};
  doca_error_t err;

  match.parser_meta.random = 0;
  match_mask.parser_meta.random = random_mask;

  err = doca_flow_pipe_cfg_create(&cfg, port);
  crash_if_unsuccessful(err, "pipe_cfg_create (sample)");
  err = doca_flow_pipe_cfg_set_name(cfg, "RANDOM_SAMPLE");
  crash_if_unsuccessful(err, "pipe_cfg_set_name (sample)");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  crash_if_unsuccessful(err, "pipe_cfg_set_type (sample)");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  crash_if_unsuccessful(err, "pipe_cfg_set_domain (sample)");
  err = doca_flow_pipe_cfg_set_is_root(cfg, false);
  crash_if_unsuccessful(err, "pipe_cfg_set_is_root (sample)");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
  crash_if_unsuccessful(err, "pipe_cfg_set_nr_entries (sample)");
  err = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
  crash_if_unsuccessful(err, "pipe_cfg_set_match (sample)");

  err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
  crash_if_unsuccessful(err, "pipe_create (sample)");
  doca_flow_pipe_cfg_destroy(cfg);

  err = doca_flow_pipe_add_entry(0, pipe, &match, NULL, NULL, NULL, 0, &status, &entry);
  crash_if_unsuccessful(err, "pipe_add_entry (sample)");
  err = doca_flow_entries_process(port, 0, 10000, 1);
  crash_if_unsuccessful(err, "entries_process (sample)");
  err = (status.failure || status.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS;
  crash_if_unsuccessful(err, "sample entry install: %u processed", status.nb_processed);
  DOCA_LOG_INFO("Random-sample pipe ready: mask 0x%04x", random_mask);
  return pipe;
}

/* Root: demux by source port. wire ingress (0) -> capture; mlx5_2 SF egress (1) -> p0 wire. */
static void create_port_demux_pipe(struct doca_flow_port *port, struct doca_flow_pipe *wire_ingress_target) {
  struct doca_flow_match match = {0};
  struct doca_flow_match match_mask = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_DROP};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  doca_error_t err;

  match.parser_meta.port_meta = UINT32_MAX;
  match_mask.parser_meta.port_meta = UINT32_MAX;

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
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 2);
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

  entry_match.parser_meta.port_meta = 0; /* p0 wire ingress -> capture */
  memset(&entry_fwd, 0, sizeof(entry_fwd));
  entry_fwd.type = DOCA_FLOW_FWD_PIPE;
  entry_fwd.next_pipe = wire_ingress_target;
  err = doca_flow_pipe_add_entry(0, pipe, &entry_match, NULL, NULL, &entry_fwd, DOCA_FLOW_WAIT_FOR_BATCH, &status, &entry);
  crash_if_unsuccessful(err, "pipe_add_entry (demux wire)");

  entry_match.parser_meta.port_meta = 1; /* mlx5_2 SF egress -> p0 wire */
  memset(&entry_fwd, 0, sizeof(entry_fwd));
  entry_fwd.type = DOCA_FLOW_FWD_PORT;
  entry_fwd.port_id = 0;
  err = doca_flow_pipe_add_entry(0, pipe, &entry_match, NULL, NULL, &entry_fwd, 0, &status, &entry);
  crash_if_unsuccessful(err, "pipe_add_entry (demux sf)");

  err = doca_flow_entries_process(port, 0, 10000, 2);
  crash_if_unsuccessful(err, "entries_process (demux)");
  err = (status.failure || status.nb_processed != 2) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS;
  crash_if_unsuccessful(err, "demux entry install: %u/2 processed", status.nb_processed);
  DOCA_LOG_INFO("Port demux ready: wire->capture, mlx5_2 SF->p0 wire");
}

static doca_error_t pcap_callback(void *param, void *config) {
  struct mirror_app_config *cfg = config;
  cfg->pcap_path = strdup((const char *)param);
  return cfg->pcap_path ? DOCA_SUCCESS : DOCA_ERROR_NO_MEMORY;
}

static doca_error_t sample_callback(void *param, void *config) {
  struct mirror_app_config *cfg = config;
  int n = *(int *)param;
  if (n < 1) {
    DOCA_LOG_ERR("--sample must be >= 1 (got %d)", n);
    return DOCA_ERROR_INVALID_VALUE;
  }
  cfg->sample_1_in = (uint32_t)n;
  return DOCA_SUCCESS;
}

static void register_params(void) {
  struct doca_argp_param *p;
  doca_error_t err = doca_argp_param_create(&p);
  crash_if_unsuccessful(err, "doca_argp_param_create (pcap)");
  doca_argp_param_set_long_name(p, "pcap");
  doca_argp_param_set_description(p, "Output pcap file for mirrored packets (required, e.g. --pcap capture.pcap)");
  doca_argp_param_set_callback(p, pcap_callback);
  doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
  doca_argp_param_set_mandatory(p);
  err = doca_argp_register_param(p);
  crash_if_unsuccessful(err, "doca_argp_register_param (pcap)");

  err = doca_argp_param_create(&p);
  crash_if_unsuccessful(err, "doca_argp_param_create (sample)");
  doca_argp_param_set_long_name(p, "sample");
  doca_argp_param_set_description(p,
                                  "Mirror ~1 of every N packets to the pcap (default 1 = all). Rounded to the nearest "
                                  "power-of-two rate, e.g. --sample 10000 -> 1/8192.");
  doca_argp_param_set_callback(p, sample_callback);
  doca_argp_param_set_type(p, DOCA_ARGP_TYPE_INT);
  err = doca_argp_register_param(p);
  crash_if_unsuccessful(err, "doca_argp_register_param (sample)");
}

int main(int argc, char **argv) {
  doca_error_t err = doca_log_backend_create_standard();
  crash_if_unsuccessful(err, "doca_log_backend_create_standard");

  struct doca_log_backend *sdk_log;
  err = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
  crash_if_unsuccessful(err, "doca_log_backend_create_with_file_sdk");
  err = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
  crash_if_unsuccessful(err, "doca_log_backend_set_sdk_level");

  struct mirror_app_config app_cfg = {.sample_1_in = 1}; /* default: capture all */
  err = doca_argp_init("doca_flow_mirror", &app_cfg);
  crash_if_unsuccessful(err, "doca_argp_init");
  doca_argp_set_dpdk_program(initialize_dpdk);
  register_params();
  err = doca_argp_start(argc, argv);
  crash_if_unsuccessful(err, "doca_argp_start");

  /* open the pcap sink up front so we fail fast on a bad path */
  pcap_t *pd = pcap_open_dead(DLT_EN10MB, SNAPLEN);
  if (pd == NULL) {
    DOCA_LOG_CRIT("pcap_open_dead failed");
    return EXIT_FAILURE;
  }
  pcap_dumper_t *dumper = pcap_dump_open(pd, app_cfg.pcap_path);
  if (dumper == NULL) {
    DOCA_LOG_CRIT("pcap_dump_open('%s') failed: %s", app_cfg.pcap_path, pcap_geterr(pd));
    return EXIT_FAILURE;
  }

  struct doca_dev *dev = open_and_probe_dev(0, "dv_flow_en=2,fdb_def_rule_en=1,repr_matching_en=0,representor=sf0");
  configure_and_start_dpdk_port(dev);
  initialize_doca_flow();

  struct doca_flow_port *port = port_start(dev);          /* DPDK 0: PF0 uplink       */
  struct doca_flow_port *sf_rep_port = rep_port_start(1); /* DPDK 1: PF0 SF rep (mlx5_2) */

  uint16_t pf0_port_id;
  err = doca_dpdk_get_first_port_id(dev, &pf0_port_id);
  crash_if_unsuccessful(err, "doca_dpdk_get_first_port_id (pf0)");

  struct doca_flow_pipe *rss_pipe = create_rss_pipe(port);
  setup_capture_mirror(port, rss_pipe);
  struct doca_flow_pipe *passthrough = create_passthrough_pipe(port, 1);
  struct doca_flow_pipe_entry *capture_entry = NULL;
  struct doca_flow_pipe *capture = create_capture_pipe(port, passthrough, &capture_entry);

  /* wire-ingress entry point: capture-all, or a 1-in-N random pre-filter in front of capture */
  struct doca_flow_pipe *wire_ingress_target = capture;
  if (app_cfg.sample_1_in > 1) {
    uint32_t actual_n = 1;
    uint16_t mask = mask_for_one_in_n(app_cfg.sample_1_in, &actual_n);
    wire_ingress_target = create_random_sample_pipe(port, capture, passthrough, mask);
    DOCA_LOG_INFO("Sampling ~1 in %u packets (requested 1 in %u)", actual_n, app_cfg.sample_1_in);
  } else {
    DOCA_LOG_INFO("Capturing ALL wire-ingress IPv4 packets");
  }
  create_port_demux_pipe(port, wire_ingress_target);

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  DOCA_LOG_INFO("Capturing mirrored packets to '%s' — Ctrl-C to stop", app_cfg.pcap_path);

  struct rte_mbuf *bufs[RX_BURST];
  uint64_t captured = 0;
  time_t last_report = time(NULL);

  while (g_running) {
    uint16_t nb = rte_eth_rx_burst(pf0_port_id, 0, bufs, RX_BURST);
    for (uint16_t i = 0; i < nb; i++) {
      struct pcap_pkthdr h;
      struct timeval tv;
      gettimeofday(&tv, NULL);
      h.ts = tv;
      h.caplen = rte_pktmbuf_data_len(bufs[i]); /* first (contiguous) segment */
      h.len = rte_pktmbuf_pkt_len(bufs[i]);
      pcap_dump((u_char *)dumper, &h, rte_pktmbuf_mtod(bufs[i], const u_char *));
      rte_pktmbuf_free(bufs[i]);
      captured++;
    }
    if (nb == 0) {
      usleep(200);
    }
    time_t now = time(NULL);
    if (now != last_report) {
      last_report = now;
      pcap_dump_flush(dumper);
      struct doca_flow_resource_query q;
      uint64_t fwded = 0;
      if (doca_flow_resource_query_entry(capture_entry, &q) == DOCA_SUCCESS) {
        fwded = q.counter.total_pkts;
      }
      DOCA_LOG_INFO("captured(pcap): %lu | capture-pipe pkts: %lu", captured, fwded);
    }
  }

  pcap_dump_flush(dumper);
  pcap_dump_close(dumper);
  pcap_close(pd);
  DOCA_LOG_INFO("Wrote %lu packets to %s", captured, app_cfg.pcap_path);

  doca_flow_port_stop(sf_rep_port);
  doca_flow_port_stop(port);
  doca_flow_destroy();
  doca_argp_destroy();
  return EXIT_SUCCESS;
}
