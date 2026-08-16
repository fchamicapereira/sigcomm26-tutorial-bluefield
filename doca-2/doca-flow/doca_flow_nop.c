// doca_flow_nop — the bare-minimum DOCA Flow program: ONE pipe with TWO entries that forward
// packets between the card's two ports, untouched — p0 wire -> receiver SF (mlx5_2), and the SF's
// egress (ACKs/CNPs) back out the wire. No header-modify action at all ("nop" = no operation):
// the smallest complete DOCA Flow forwarder.
//
// Two purposes:
//   1. Performance control group — measures the pipeline's raw forwarding cost with zero
//      actions attached, to compare against doca_flow_mac / doca_flow_ecn (see the throughput
//      bottleneck investigation).
//   2. Base file for tutorial participants to build their own pipeline on top of.
//
// Same environment as doca_flow_mac / doca_flow_ecn — see setup_roce_loopback.sh / README.md.
#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_dpdk.h>
#include <doca_flow.h>
#include <doca_log.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_mbuf.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

DOCA_LOG_REGISTER(FLOW_NOP);

#define NB_QUEUES 1
// two forward entries (one per direction), both counted so their traffic is observable
#define NB_COUNTERS 2

// Descriptor ring sizes. This program forwards entirely inside the eSwitch and never receives on
// the CPU queues, but DPDK still requires a queue to start a port — so these stay small.
#define RX_RING_SIZE 512
#define TX_RING_SIZE 512

// mbuf pool shared by both DPDK ports (PF uplink + SF representor).
#define MBUF_POOL_SIZE 8192

static volatile bool g_running = true;

static void signal_handler(int signum) {
  if (signum == SIGINT || signum == SIGTERM) {
    g_running = false;
  }
}

// Log at CRIT level and terminate if err != DOCA_SUCCESS — mirrors rte_exit().
static __attribute__((format(printf, 2, 3))) void doca_check(doca_error_t err, const char *fmt,
                                                             ...) {
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

// Shorthand for the common case: run a DOCA call, abort with a "ctx: call" message if it fails.
// Identical to the macro in doca_flow_ecn_pcap.c.
#define DOCA_CHECK(ctx, expr) doca_check((expr), "%s: %s", (ctx), #expr)

// DOCA Flow global entry-process callback — updates a per-batch status struct
struct entry_batch_status {
  bool failure;
  uint32_t nb_processed;
};

static void entry_process_cb(struct doca_flow_pipe_entry *entry, uint16_t pipe_queue,
                             enum doca_flow_entry_status status, enum doca_flow_entry_op op,
                             void *user_ctx) {
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

// EAL bring-up. Not called directly — doca_argp_set_dpdk_program() registers it, and doca_argp
// invokes it with the EAL half of the command line: everything BEFORE the "--" separator. That is
// why this program is run as `doca_flow_nop -- --sf-num 0`, with the app's own options after the
// dashes.
//
// The odd part is the two arguments it appends. By default EAL probes every device it can see, and
// binding the SFs out from under the kernel would break the RoCE traffic flowing through them.
// Passing any -a (allowlist) flag flips EAL from "probe everything" to "probe only what is
// listed", and the address listed here does not exist — so the allowlist is empty and EAL probes
// nothing at all. The port actually wanted is attached afterwards, explicitly, by
// doca_dpdk_port_probe() on the device doca_dev_open() returned. This mirrors DOCA's own
// dpdk_init_without_probing() in dpdk_utils.c.
//
// The two appended strings are `static char[]` rather than string literals because EAL parses argv
// with getopt, which permutes and writes to it; the storage has to be writable and outlive the
// call. The fixed 64-entry array is why argc is capped at 62 — room for the caller's arguments
// plus the two added here.
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

// SF number of the receiver-side Scalable Function — the N in the "en3f0pf0sfN" representor
// netdev, and in the "representor=sfN" mlx5 probe arg built below. Override with --sf-num.
//
// Default 0 is correct on any DPU prepared by setup_roce_loopback.sh, which imposes sfnum 0 on
// both PFs precisely so this never has to be passed. The flag is the escape hatch for a DPU whose
// SFs were created by hand: `sudo mlnx-sf -a show` might report en3f0pf0sf2/sf3, in which case the
// receiver SF is --sf-num 2. Probing a number that does not exist yields no representor ethdev at
// all (see find_sf_representor_port_id).
static uint32_t g_sf_num = 0;

// Open the Nth DOCA device (0 is the first, which is PF0) and probe it into DPDK.
//
// PF0 only: DOCA manages PF0's FDB via HWS root pipes, which take highest priority.
//
// The probe string is an mlx5 PMD device-argument list, and every token in it is load-bearing:
//
//   dv_flow_en=2       Hardware steering (HWS). DOCA Flow's "switch,hws" mode needs it, and it is
//                      also the precondition for repr_matching_en below — the driver refuses with
//                      "Disabling representor matching is valid only when HW Steering is enabled".
//   fdb_def_rule_en=1  Keep mlx5's default FDB jump rule. Each PF owns a separate FDB domain and
//                      this program only ever programs PF0's; leaving the default in place is what
//                      keeps PF1 forwarding through its OVS bridge, so mlx5_3's egress traffic
//                      still exits via the p1 uplink. DOCA's own switch samples set this to 0
//                      because they own both sides — here it must stay 1.
//   repr_matching_en=0 Drop the PMD's implicit per-representor match, so traffic is selected only
//                      by the rules this program writes. The driver puts the port into isolated
//                      mode as a result: "ingress traffic is restricted to defined flow rules".
//   representor=sfN    Also probe the receiver SF's representor, so it shows up as a second DPDK
//                      port. The PF is port 0 and this becomes port 1. N is --sf-num, so this is
//                      built at run time rather than being a fixed string.
static struct doca_dev *open_and_probe_dev(uint32_t index) {
  struct doca_devinfo **devinfo_list;
  uint32_t nb_devs;
  struct doca_dev *dev;

  DOCA_CHECK("device", doca_devinfo_create_list(&devinfo_list, &nb_devs));

  if (index >= nb_devs) {
    DOCA_LOG_CRIT("Device index %u out of range (%u devices found)", index, nb_devs);
    exit(EXIT_FAILURE);
  }

  DOCA_CHECK("device", doca_dev_open(devinfo_list[index], &dev));

  doca_devinfo_destroy_list(devinfo_list);

  char probe_args[128];
  snprintf(probe_args, sizeof(probe_args),
           "dv_flow_en=2,fdb_def_rule_en=1,repr_matching_en=0,representor=sf%u", g_sf_num);

  DOCA_CHECK("device", doca_dpdk_port_probe(dev, probe_args));

  return dev;
}

// DPDK must be configured and started before DOCA Flow calls rte_flow_configure (HWS requirement).
// DPDK also requires at least one RX queue to start a port.
static void configure_and_start_dpdk_port(struct doca_dev *dev) {
  uint16_t first_port_id;
  DOCA_CHECK("device", doca_dpdk_get_first_port_id(dev, &first_port_id));

  // One shared mempool for all ports (PF + SF rep).
  struct rte_mempool *mp =
      rte_pktmbuf_pool_create("mbuf_pool", MBUF_POOL_SIZE, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                              rte_eth_dev_socket_id(first_port_id));
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
      ret = rte_eth_rx_queue_setup(port_id, q, RX_RING_SIZE, rte_eth_dev_socket_id(port_id), NULL,
                                   mp);
      if (ret < 0) {
        DOCA_LOG_CRIT("rte_eth_rx_queue_setup port %u q%d failed (errno %d)", port_id, q, -ret);
        exit(EXIT_FAILURE);
      }
      ret = rte_eth_tx_queue_setup(port_id, q, TX_RING_SIZE, rte_eth_dev_socket_id(port_id),
                                   &tx_conf);
      if (ret < 0) {
        DOCA_LOG_CRIT("rte_eth_tx_queue_setup port %u q%d failed (errno %d)", port_id, q, -ret);
        exit(EXIT_FAILURE);
      }
    }

    // Isolated mode: no ingress traffic is delivered to this port's RSS/CPU queues — every
    // packet is steered by flow rules alone, which is exactly what this program does (it
    // forwards port-to-port inside the eSwitch and never uses DOCA_FLOW_FWD_RSS).
    // Must be set after the queues are configured but BEFORE rte_eth_dev_start, per DOCA's own
    // dpdk_utils.c. Paired with the ",isolated" mode arg in initialize_doca_flow(): together they
    // stop DOCA Flow from building an internal RSS suffix context in the FDB domain at port-start.
    // That context is unnecessary here, and on BlueField firmware that only permits RSS actions on
    // ingress it makes doca_flow_port_start fail outright with
    // "RSS action supported for ingress only".
    struct rte_flow_error flow_err = {0};
    ret = rte_flow_isolate(port_id, 1, &flow_err);
    if (ret < 0) {
      DOCA_LOG_CRIT("rte_flow_isolate port %u failed (errno %d): %s", port_id, -ret,
                    flow_err.message ? flow_err.message : "no details");
      exit(EXIT_FAILURE);
    }

    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
      DOCA_LOG_CRIT("rte_eth_dev_start port %u failed (errno %d)", port_id, -ret);
      exit(EXIT_FAILURE);
    }
  }
}

static void initialize_doca_flow(void) {
  struct doca_flow_cfg *cfg;
  DOCA_CHECK("flow", doca_flow_cfg_create(&cfg));
  DOCA_CHECK("flow", doca_flow_cfg_set_pipe_queues(cfg, NB_QUEUES));

  // switch mode: traffic forwarded between eSwitch ports; no CPU RSS queues needed.
  // "isolated" keeps ingress traffic off this port's RSS queues (see rte_flow_isolate above) —
  // DOCA's own flow_switch sample uses "switch,hws,isolated".
  // "disable_switch_rss" is what actually stops DOCA Flow from building its internal FDB RSS
  // suffix context at port start. That context is useless to us (we never use DOCA_FLOW_FWD_RSS),
  // and on BlueField firmware that permits RSS actions on ingress only, creating it makes
  // doca_flow_port_start fail outright with "RSS action supported for ingress only". "isolated"
  // alone does NOT suppress it — verified via --sdk-log-level 60, which still showed DOCA build
  // "rss_htbl_port_0_0" until this token was added.
  // Both are undocumented in doca_flow.h but are parsed mode-args tokens (see the token table in
  // libdoca_flow.so alongside "isolated", "expert", "hairpinq_num").
  DOCA_CHECK("flow", doca_flow_cfg_set_mode_args(cfg, "switch,hws,isolated,disable_switch_rss"));
  DOCA_CHECK("flow", doca_flow_cfg_set_nr_counters(cfg, NB_COUNTERS));
  DOCA_CHECK("flow", doca_flow_cfg_set_cb_entry_process(cfg, entry_process_cb));
  DOCA_CHECK("flow", doca_flow_init(cfg));

  doca_flow_cfg_destroy(cfg);
}

// DPDK port id of the PF uplink — the eSwitch "proxy" port, which DOCA Flow requires to be the
// FIRST port started in switch mode.
//
// Deliberately not doca_dpdk_get_first_port_id(): on DOCA 2.7 that returns the SF representor
// (DPDK port 1) rather than the PF (port 0). Starting the representor first makes
// doca_flow_port_start fail with
//     failed getting is_switch_manager property - proxy port 0 not found
// because the proxy it resolves to (the PF) has not been started yet. DOCA 2.9 happens to return
// the PF from that call, so relying on it works there and silently breaks on 2.7.
static uint16_t find_pf_port_id(void) {
  uint16_t port_id;
  RTE_ETH_FOREACH_DEV(port_id) {
    struct rte_eth_dev_info dev_info = {0};
    if (rte_eth_dev_info_get(port_id, &dev_info) < 0) {
      continue;
    }
    if (dev_info.dev_flags == NULL || (*dev_info.dev_flags & RTE_ETH_DEV_REPRESENTOR) == 0) {
      return port_id;
    }
  }
  DOCA_LOG_CRIT("No non-representor (PF) ethdev found — cannot identify the eSwitch proxy port.");
  exit(EXIT_FAILURE);
}

static struct doca_flow_port *port_start(struct doca_dev *dev) {
  uint16_t port_id = find_pf_port_id();

  struct doca_flow_port_cfg *cfg;
  DOCA_CHECK("port", doca_flow_port_cfg_create(&cfg));
  DOCA_CHECK("port", doca_flow_port_cfg_set_dev(cfg, dev));

  char port_id_str[8];
  snprintf(port_id_str, sizeof(port_id_str), "%u", port_id);
  DOCA_CHECK("port", doca_flow_port_cfg_set_devargs(cfg, port_id_str));

  struct doca_flow_port *port;
  DOCA_CHECK("port", doca_flow_port_start(cfg, &port));

  doca_flow_port_cfg_destroy(cfg);

  return port;
}

static doca_error_t sf_num_callback(void *param, void *config) {
  (void)config;
  long v = atol((const char *)param);

  if (v < 0 || v > UINT16_MAX) {
    DOCA_LOG_ERR("--sf-num must be in [0, %u] (got '%s')", UINT16_MAX, (const char *)param);
    return DOCA_ERROR_INVALID_VALUE;
  }
  g_sf_num = (uint32_t)v;
  return DOCA_SUCCESS;
}

static void register_sf_num_param(void) {
  struct doca_argp_param *param;
  DOCA_CHECK("argp", doca_argp_param_create(&param));
  doca_argp_param_set_long_name(param, "sf-num");
  doca_argp_param_set_description(
      param,
      "SF number of the receiver-side Scalable Function, i.e. the N in the "
      "'en3f0pf0sfN' representor netdev listed by 'mlnx-sf -a show'. Default: 0");
  doca_argp_param_set_callback(param, sf_num_callback);
  doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
  DOCA_CHECK("argp", doca_argp_register_param(param));
}

// Find the DPDK port id of PF0's SF representor (probed above via "representor=sf0").
//
// Do NOT assume it is port 1. The probe only yields a representor ethdev if the SF actually
// exists on this DPU, and its port id depends on probe order. Ask DPDK which port it flagged as
// a representor instead, and fail with something actionable if there is none — otherwise
// doca_flow_port_start just reports the opaque "Invalid port_id=1 ... No such device".
static uint16_t find_sf_representor_port_id(void) {
  uint16_t port_id;
  uint16_t nb_ports = 0;

  RTE_ETH_FOREACH_DEV(port_id) {
    struct rte_eth_dev_info dev_info = {0};
    nb_ports++;
    if (rte_eth_dev_info_get(port_id, &dev_info) < 0) {
      continue;
    }
    if (dev_info.dev_flags != NULL && (*dev_info.dev_flags & RTE_ETH_DEV_REPRESENTOR) != 0) {
      DOCA_LOG_INFO("SF representor found on DPDK port %u", port_id);
      return port_id;
    }
  }

  DOCA_LOG_CRIT(
      "No SF representor ethdev found (%u DPDK port(s) probed, expected the PF + its SF rep).",
      nb_ports);
  DOCA_LOG_CRIT("Probed 'representor=sf%u', so this DPU has no SF numbered %u on PF0.", g_sf_num,
                g_sf_num);
  DOCA_LOG_CRIT(
      "Run 'sudo mlnx-sf -a show' and look at the 'Representor netdev: en3f0pf0sf<N>' lines:");
  DOCA_LOG_CRIT("  - if it lists a different N (e.g. en3f0pf0sf2), re-run with --sf-num <N>;");
  DOCA_LOG_CRIT(
      "  - if it lists no SF on PF0 at all, create the SFs first (see README, 'Scalable Functions "
      "(SFs)').");
  exit(EXIT_FAILURE);
}

// Start an arbitrary DPDK port as a DOCA Flow port (used for SF representors).
static struct doca_flow_port *rep_port_start(uint16_t dpdk_port_id) {
  struct doca_flow_port_cfg *cfg;
  char port_id_str[8];
  snprintf(port_id_str, sizeof(port_id_str), "%u", dpdk_port_id);

  DOCA_CHECK("rep port", doca_flow_port_cfg_create(&cfg));
  DOCA_CHECK("rep port", doca_flow_port_cfg_set_devargs(cfg, port_id_str));

  struct doca_flow_port *port;
  DOCA_CHECK("rep port", doca_flow_port_start(cfg, &port));

  doca_flow_port_cfg_destroy(cfg);
  return port;
}

// What build_pipeline() hands back to main(): the two counted entries the report loop reads.
struct pipeline {
  struct doca_flow_pipe_entry *to_sf;    // wire (port 0) -> receiver SF (port 1)
  struct doca_flow_pipe_entry *to_wire;  // receiver SF (port 1) -> wire (port 0)
};

// --- Shared boilerplate: the same setup/report helpers doca_flow_ecn_pcap.c wraps its main() in,
// so the two programs read the same top to bottom. The pipeline below (create_root_pipe +
// build_pipeline) is the only part that differs. ---
static void print_stats(struct doca_flow_pipe_entry *to_sf, struct doca_flow_pipe_entry *to_wire) {
  struct doca_flow_resource_query q_sf, q_wire;
  DOCA_CHECK("query wire->SF", doca_flow_resource_query_entry(to_sf, &q_sf));
  DOCA_CHECK("query SF->wire", doca_flow_resource_query_entry(to_wire, &q_wire));

  DOCA_LOG_INFO("Forwarded  wire->SF: %lu   SF->wire: %lu", q_sf.counter.total_pkts,
                q_wire.counter.total_pkts);
}

static void setup_logging(void) {
  DOCA_CHECK("logging", doca_log_backend_create_standard());
  struct doca_log_backend *sdk_log;
  DOCA_CHECK("logging", doca_log_backend_create_with_file_sdk(stderr, &sdk_log));
  DOCA_CHECK("logging", doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING));
}

static void parse_args(int argc, char **argv) {
  DOCA_CHECK("argp", doca_argp_init("doca_flow_nop", NULL));
  doca_argp_set_dpdk_program(initialize_dpdk);
  register_sf_num_param();
  DOCA_CHECK("argp", doca_argp_start(argc, argv));
}

static void install_signal_handlers(void) {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
}

static void log_startup(void) {
  DOCA_LOG_INFO("doca_flow_nop: forwarding p0 wire <-> receiver SF untouched — Ctrl-C to stop");
}

// Report the per-direction counters once a second, until SIGINT/SIGTERM. (doca_flow_ecn_pcap.c's
// run_capture_loop does the same, and also drains mirrored copies into a pcap; nothing to capture
// here.)
static void run_report_loop(const struct pipeline *pl) {
  while (g_running) {
    sleep(1);
    print_stats(pl->to_sf, pl->to_wire);
  }
}

// The one pipe this program builds: a root pipe that forwards by direction, with exactly two entries.
//
// It matches only the port a packet arrived on (parser_meta.port_meta) and sends it to the other
// port — nothing in the packet is changed ("nop" = no operation):
//   port_meta == 0  (arrived from the p0 wire)     -> port 1  (the receiver SF, mlx5_2)
//   port_meta == 1  (arrived from the receiver SF) -> port 0  (back out the p0 wire)
//
// The pipe's forward is CHANGEABLE, so each entry fills in its own destination port. Both entries
// are counted, so the main loop can report how many packets crossed in each direction. matching on
// port_meta is a non-empty match template, which is all HWS requires.
static void create_root_pipe(struct doca_flow_port *port, struct doca_flow_pipe_entry **to_sf,
                             struct doca_flow_pipe_entry **to_wire) {
  struct doca_flow_match match = {0};
  struct doca_flow_match match_mask = {0};
  struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};  // destination set per entry
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_DROP};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  doca_error_t err;

  // The pipe matches on the source port; the exact value is supplied by each entry.
  match.parser_meta.port_meta = UINT32_MAX;
  match_mask.parser_meta.port_meta = UINT32_MAX;

  DOCA_CHECK("pipe", doca_flow_pipe_cfg_create(&cfg, port));
  DOCA_CHECK("pipe", doca_flow_pipe_cfg_set_name(cfg, "FORWARDER"));
  DOCA_CHECK("pipe", doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC));
  DOCA_CHECK("pipe", doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT));
  DOCA_CHECK("pipe", doca_flow_pipe_cfg_set_is_root(cfg, true));
  DOCA_CHECK("pipe", doca_flow_pipe_cfg_set_nr_entries(cfg, 2));
  DOCA_CHECK("pipe", doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask));
  DOCA_CHECK("pipe", doca_flow_pipe_cfg_set_monitor(cfg, &monitor));

  DOCA_CHECK("pipe", doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe));
  doca_flow_pipe_cfg_destroy(cfg);

  struct doca_flow_match entry_match = {0};
  struct doca_flow_fwd entry_fwd = {0};
  struct entry_batch_status status = {0};

  // Entry 1 — from the p0 wire (port 0) to the receiver SF (port 1). WAIT_FOR_BATCH queues it.
  entry_match.parser_meta.port_meta = 0;
  entry_fwd.type = DOCA_FLOW_FWD_PORT;
  entry_fwd.port_id = 1;
  DOCA_CHECK("wire -> SF", doca_flow_pipe_add_entry(0, pipe, &entry_match, NULL, &monitor,
                                                    &entry_fwd, DOCA_FLOW_WAIT_FOR_BATCH, &status,
                                                    to_sf));

  // Entry 2 — from the receiver SF (port 1) back out the p0 wire (port 0). flags=0 flushes the batch.
  entry_match.parser_meta.port_meta = 1;
  entry_fwd.type = DOCA_FLOW_FWD_PORT;
  entry_fwd.port_id = 0;
  DOCA_CHECK("SF -> wire", doca_flow_pipe_add_entry(0, pipe, &entry_match, NULL, &monitor,
                                                    &entry_fwd, 0, &status, to_wire));

  DOCA_CHECK("pipe", doca_flow_entries_process(port, 0, 10000, 2));
  err = (status.failure || status.nb_processed != 2) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS;
  doca_check(err, "entry installation: %u/2 processed", status.nb_processed);

  DOCA_LOG_INFO("Forwarder ready: two entries, p0 wire <-> receiver SF (mlx5_2)");
}

// Assemble the eSwitch pipeline. Same main() -> build_pipeline() -> create_*() shape as
// doca_flow_ecn_pcap.c, only smaller: one forwarder pipe instead of several. Everything before this
// call in main() is device/DPDK/DOCA setup; everything reachable from here down is the pipeline.
static void build_pipeline(struct doca_flow_port *port, struct pipeline *out) {
  create_root_pipe(port, &out->to_sf, &out->to_wire);
}

int main(int argc, char **argv) {
  struct pipeline pl = {0};

  setup_logging();
  parse_args(argc, argv);

  // Device and library bring-up. None of this is DOCA Flow pipeline work.
  struct doca_dev *dev = open_and_probe_dev(0);
  configure_and_start_dpdk_port(dev);
  initialize_doca_flow();
  struct doca_flow_port *port = port_start(dev);
  struct doca_flow_port *sf_rep = rep_port_start(find_sf_representor_port_id());

  build_pipeline(port, &pl);

  install_signal_handlers();
  log_startup();
  run_report_loop(&pl);

  doca_flow_port_stop(sf_rep);
  doca_flow_port_stop(port);
  doca_flow_destroy();
  doca_argp_destroy();

  return EXIT_SUCCESS;
}
