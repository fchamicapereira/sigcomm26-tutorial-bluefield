// doca_flow_nop — the bare minimum DOCA Flow pipeline: forwards p0 wire ingress straight to
// the receiver's SF (mlx5_2) and routes the SF's egress (ACKs/CNPs) back out the wire. No
// header-modify action at all.
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
#include <rte_mbuf.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

DOCA_LOG_REGISTER(FLOW_NOP);

#define NB_QUEUES 1
// single forward entry, counted so its traffic is observable
#define NB_COUNTERS 1

// Descriptor ring sizes. This program forwards entirely inside the eSwitch and never receives on
// the CPU queues, but DPDK still requires a queue to start a port — so these stay small.
#define RX_RING_SIZE 512
#define TX_RING_SIZE 512

// mbuf pool shared by both DPDK ports (PF uplink + SF representor).
#define MBUF_POOL_SIZE 8192

// DOCA 3.4 HWS needs a per-port action-memory pool for any pipe that carries a modify action.
// nop has none, but keeping it here means a participant who adds one to this base file won't hit
// the cryptic rc=-12 "failed to create actions_template". Sized via DOCA's own formula
// next_pow2(entries * DOCA_FLOW_MAX_ENTRY_ACTIONS_MEM_SIZE + 1024); 16 KB is ample here.
#define ACTIONS_MEM_SIZE (16 * 1024)

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
// The odd part is the arguments it appends. By default EAL probes every device it can see, and
// binding the SFs out from under the kernel would break the RoCE traffic flowing through them.
// Passing any -a (allowlist) flag flips EAL from "probe everything" to "probe only what is
// listed", and the address listed here does not exist — so the allowlist is empty and EAL probes
// nothing at all. The port actually wanted is attached afterwards, explicitly, by
// doca_dpdk_port_probe() on the device doca_dev_open() returned. This mirrors DOCA's own
// dpdk_init_without_probing() in dpdk_utils.c.
//
// The second allowlist entry, "auxiliary:", is what keeps EAL off the auxiliary bus. Without it
// EAL still scans that bus and tries to probe the SFs, whose verbs now live in ns0/ns1 after
// setup_roce_loopback.sh moved them, and the probe fails with "Verbs device not found".
//
// The appended strings are `static char[]` rather than string literals because EAL parses argv
// with getopt, which permutes and writes to it; the storage has to be writable and outlive the
// call. The fixed 64-entry array is why argc is capped at 60 — room for the caller's arguments
// plus the four added here.
static doca_error_t initialize_dpdk(int argc, char **argv) {
  static char allow_flag[] = "-a";
  static char dummy_pci[] = "pci:00:00.0";
  static char dummy_aux[] = "auxiliary:";
  char *new_argv[64];

  if (argc >= 60) {
    DOCA_LOG_ERR("Too many EAL arguments");
    return DOCA_ERROR_INVALID_VALUE;
  }
  for (int i = 0; i < argc; i++) {
    new_argv[i] = argv[i];
  }
  new_argv[argc] = allow_flag;
  new_argv[argc + 1] = dummy_pci;
  new_argv[argc + 2] = allow_flag;
  new_argv[argc + 3] = dummy_aux;

  if (rte_eal_init(argc + 4, new_argv) < 0) {
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
//   dv_flow_en=2       Hardware steering (HWS), which DOCA Flow's "switch,hws" mode requires.
//   fdb_def_rule_en=1  Keep mlx5's default FDB jump rule. Each PF owns a separate FDB domain and
//                      this program only ever programs PF0's; leaving the default in place is what
//                      keeps PF1 forwarding through its OVS bridge, so mlx5_3's egress traffic
//                      still exits via the p1 uplink. DOCA's own switch samples set this to 0
//                      because they own both sides — here it must stay 1.
//   representor=sfN    Also probe the receiver SF's representor, so it shows up as a second DPDK
//                      port. The PF is port 0 and this becomes port 1. N is --sf-num, so this is
//                      built at run time rather than being a fixed string.
static struct doca_dev *open_and_probe_dev(uint32_t index) {
  struct doca_devinfo **devinfo_list;
  uint32_t nb_devs;
  struct doca_dev *dev;
  doca_error_t err;

  err = doca_devinfo_create_list(&devinfo_list, &nb_devs);
  doca_check(err, "doca_devinfo_create_list");

  if (index >= nb_devs) {
    DOCA_LOG_CRIT("Device index %u out of range (%u devices found)", index, nb_devs);
    exit(EXIT_FAILURE);
  }

  err = doca_dev_open(devinfo_list[index], &dev);
  doca_check(err, "doca_dev_open");

  doca_devinfo_destroy_list(devinfo_list);

  char probe_args[128];
  snprintf(probe_args, sizeof(probe_args), "dv_flow_en=2,fdb_def_rule_en=1,representor=sf%u",
           g_sf_num);

  err = doca_dpdk_port_probe(dev, probe_args);
  doca_check(err, "doca_dpdk_port_probe (index=%u)", index);

  return dev;
}

// DPDK must be configured and started before DOCA Flow calls rte_flow_configure (HWS requirement).
// DPDK also requires at least one RX queue to start a port.
static void configure_and_start_dpdk_port(struct doca_dev *dev) {
  uint16_t first_port_id;
  doca_error_t err = doca_dpdk_get_first_port_id(dev, &first_port_id);
  doca_check(err, "doca_dpdk_get_first_port_id");

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

    // No rte_flow_isolate() here, unlike the 2.x build. That call exists there to pair with the
    // ",isolated" mode arg and stop DOCA Flow building an internal FDB RSS suffix context that
    // BlueField firmware rejects at port start; on 3.4 that context is not a problem, and
    // doca_flow_ecn_pcap needs RSS in the FDB domain for its capture path — so both programs in
    // this directory run the plain "switch,hws" mode instead. See initialize_doca_flow().

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
  doca_check(err, "doca_flow_cfg_create");

  err = doca_flow_cfg_set_pipe_queues(cfg, NB_QUEUES);
  doca_check(err, "doca_flow_cfg_set_pipe_queues");

  // switch mode: traffic forwarded between eSwitch ports; no CPU RSS queues needed.
  //
  // Plain "switch,hws", unlike the 2.x build which also passes "isolated,disable_switch_rss".
  // Those two tokens exist there to suppress DOCA's internal FDB RSS context (see
  // configure_and_start_dpdk_port); on 3.4 they are not needed, and this is the mode
  // doca_flow_ecn_pcap runs in as well.
  err = doca_flow_cfg_set_mode_args(cfg, "switch,hws");
  doca_check(err, "doca_flow_cfg_set_mode_args");

  err = doca_flow_cfg_set_nr_counters(cfg, NB_COUNTERS);
  doca_check(err, "doca_flow_cfg_set_nr_counters");

  err = doca_flow_cfg_set_cb_entry_process(cfg, entry_process_cb);
  doca_check(err, "doca_flow_cfg_set_cb_entry_process");

  err = doca_flow_init(cfg);
  doca_check(err, "doca_flow_init");

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
  doca_error_t err;

  struct doca_flow_port_cfg *cfg;
  err = doca_flow_port_cfg_create(&cfg);
  doca_check(err, "doca_flow_port_cfg_create");

  err = doca_flow_port_cfg_set_dev(cfg, dev);
  doca_check(err, "doca_flow_port_cfg_set_dev");

  err = doca_flow_port_cfg_set_port_id(cfg, port_id);
  doca_check(err, "doca_flow_port_cfg_set_port_id");

  err = doca_flow_port_cfg_set_actions_mem_size(cfg, ACTIONS_MEM_SIZE);
  doca_check(err, "doca_flow_port_cfg_set_actions_mem_size");

  struct doca_flow_port *port;
  err = doca_flow_port_start(cfg, &port);
  doca_check(err, "doca_flow_port_start");

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
  doca_error_t err;

  err = doca_argp_param_create(&param);
  doca_check(err, "doca_argp_param_create (sf-num)");
  doca_argp_param_set_long_name(param, "sf-num");
  doca_argp_param_set_description(
      param,
      "SF number of the receiver-side Scalable Function, i.e. the N in the "
      "'en3f0pf0sfN' representor netdev listed by 'mlnx-sf -a show'. Default: 0");
  doca_argp_param_set_callback(param, sf_num_callback);
  doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
  err = doca_argp_register_param(param);
  doca_check(err, "doca_argp_register_param (sf-num)");
}

// Find the DPDK port id of PF0's SF representor (probed above via "representor=sfN").
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

// Open PF0's SF representor as a doca_dev_rep.
//
// 3.4 binds the representor to its DOCA Flow port through this handle, where the 2.x build simply
// passed the DPDK port index as a devargs string. The DOCA representor list is enumerated
// independently of the DPDK probe, so pick out the one whose SF index is --sf-num rather than
// taking whichever comes first.
static struct doca_dev_rep *open_sf_representor(struct doca_dev *pf_dev) {
  struct doca_devinfo_rep **rep_list;
  uint32_t nb_reps;
  struct doca_dev_rep *rep = NULL;
  doca_error_t err;

  err = doca_devinfo_rep_create_list(pf_dev, DOCA_DEVINFO_REP_FILTER_NET, &rep_list, &nb_reps);
  doca_check(err, "doca_devinfo_rep_create_list");

  for (uint32_t i = 0; i < nb_reps; i++) {
    uint32_t sf_index;
    if (doca_devinfo_rep_get_sf_index(rep_list[i], &sf_index) != DOCA_SUCCESS) {
      continue;
    }
    if (sf_index != g_sf_num) {
      continue;
    }
    err = doca_dev_rep_open(rep_list[i], &rep);
    doca_check(err, "doca_dev_rep_open (sf_index=%u)", sf_index);
    DOCA_LOG_INFO("Opened SF representor (sf_index=%u)", sf_index);
    break;
  }
  doca_devinfo_rep_destroy_list(rep_list);

  if (rep == NULL) {
    DOCA_LOG_CRIT("No DOCA representor with SF index %u found on PF0 (%u net representor(s)).",
                  g_sf_num, nb_reps);
    DOCA_LOG_CRIT("Run 'sudo mlnx-sf -a show' and re-run with --sf-num <N> if it lists another N.");
    exit(EXIT_FAILURE);
  }
  return rep;
}

// Start an arbitrary DPDK port as a DOCA Flow port (used for SF representors).
static struct doca_flow_port *rep_port_start(uint16_t dpdk_port_id, struct doca_dev_rep *dev_rep) {
  struct doca_flow_port_cfg *cfg;

  doca_error_t err = doca_flow_port_cfg_create(&cfg);
  doca_check(err, "doca_flow_port_cfg_create (rep port %u)", dpdk_port_id);

  err = doca_flow_port_cfg_set_dev_rep(cfg, dev_rep);
  doca_check(err, "doca_flow_port_cfg_set_dev_rep (rep port %u)", dpdk_port_id);

  err = doca_flow_port_cfg_set_port_id(cfg, dpdk_port_id);
  doca_check(err, "doca_flow_port_cfg_set_port_id (rep port %u)", dpdk_port_id);

  struct doca_flow_port *port;
  err = doca_flow_port_start(cfg, &port);
  doca_check(err, "doca_flow_port_start (rep port %u)", dpdk_port_id);

  doca_flow_port_cfg_destroy(cfg);
  return port;
}

// Forward-only pipe: passes all IPv4 wire ingress to dest_port_id, completely untouched.
// No action template at all — this is the whole point of doca_flow_nop.
//
// HWS still requires a non-empty match template. dscp_ecn is declared as a variable field
// with mask=0x00, making it a wildcard purely to satisfy that requirement — it carries no
// ECN meaning here.
static struct doca_flow_pipe *create_fwd_pipe(struct doca_flow_port *port, uint16_t dest_port_id,
                                              bool with_counter) {
  struct doca_flow_match match = {0};
  struct doca_flow_match match_mask = {0};
  struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = dest_port_id};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  doca_error_t err;

  match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  // variable field → non-empty HWS template
  match.outer.ip4.dscp_ecn = 0xFF;
  // match_mask.outer.ip4.dscp_ecn = 0x00 (zero mask → match any TOS)

  err = doca_flow_pipe_cfg_create(&cfg, port);
  doca_check(err, "doca_flow_pipe_cfg_create (fwd pipe)");
  err = doca_flow_pipe_cfg_set_name(cfg, "FORWARD");
  doca_check(err, "doca_flow_pipe_cfg_set_name (fwd pipe)");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  doca_check(err, "doca_flow_pipe_cfg_set_type (fwd pipe)");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  doca_check(err, "doca_flow_pipe_cfg_set_domain (fwd pipe)");
  // root is PORT_DEMUX; reached via FWD_PIPE
  err = doca_flow_pipe_cfg_set_is_root(cfg, false);
  doca_check(err, "doca_flow_pipe_cfg_set_is_root (fwd pipe)");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
  doca_check(err, "doca_flow_pipe_cfg_set_nr_entries (fwd pipe)");
  err = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
  doca_check(err, "doca_flow_pipe_cfg_set_match (fwd pipe)");
  if (with_counter) {
    err = doca_flow_pipe_cfg_set_monitor(cfg, &monitor);
    doca_check(err, "doca_flow_pipe_cfg_set_monitor (fwd pipe)");
  }

  err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
  doca_check(err, "doca_flow_pipe_create (fwd pipe)");

  doca_flow_pipe_cfg_destroy(cfg);
  return pipe;
}

static struct doca_flow_pipe_entry *add_fwd_entry(struct doca_flow_pipe *pipe,
                                                  struct doca_flow_port *port, bool with_counter) {
  struct doca_flow_match match = {0};
  struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
  struct doca_flow_pipe_entry *entry;
  struct entry_batch_status status = {0};
  doca_error_t err;

  match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  // dscp_ecn left 0; mask=0x00 means any dscp_ecn matches → catches all IPv4

  err = doca_flow_pipe_basic_add_entry(0, pipe, &match, 0, NULL, with_counter ? &monitor : NULL,
                                       NULL, DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, &status, &entry);
  doca_check(err, "doca_flow_pipe_basic_add_entry (fwd)");

  err = doca_flow_entries_process(port, 0, 10000, 1);
  doca_check(err, "doca_flow_entries_process (fwd)");

  err = (status.failure || status.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS;
  doca_check(err, "fwd entry installation: %u processed", status.nb_processed);

  DOCA_LOG_INFO("Forward pipe ready");
  return entry;
}

// Root pipe of the eSwitch FDB. Demuxes on source port (parser_meta.port_id):
//   port_id == 0  (ingress from p0 wire)   -> the FORWARD pipe (straight to mlx5_2)
//   port_id == 1  (egress from mlx5_2 SF)   -> port 0 (p0 wire), so mlx5_2's RoCE
//                                                ACKs/CNPs cross the cable back to mlx5_3
//
// See doca_flow_ecn_pcap.c's create_root_pipe for the full rationale (same design).
static struct doca_flow_pipe *create_root_pipe(struct doca_flow_port *port,
                                               struct doca_flow_pipe *fwd_pipe) {
  struct doca_flow_match match = {0};
  struct doca_flow_match match_mask = {0};
  // set per entry
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_DROP};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  doca_error_t err;

  // variable — set per entry
  match.parser_meta.port_id = UINT16_MAX;
  // exact match on source port id
  match_mask.parser_meta.port_id = UINT16_MAX;

  err = doca_flow_pipe_cfg_create(&cfg, port);
  doca_check(err, "doca_flow_pipe_cfg_create (demux)");
  err = doca_flow_pipe_cfg_set_name(cfg, "PORT_DEMUX");
  doca_check(err, "doca_flow_pipe_cfg_set_name (demux)");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  doca_check(err, "doca_flow_pipe_cfg_set_type (demux)");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  doca_check(err, "doca_flow_pipe_cfg_set_domain (demux)");
  err = doca_flow_pipe_cfg_set_is_root(cfg, true);
  doca_check(err, "doca_flow_pipe_cfg_set_is_root (demux)");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 2);
  doca_check(err, "doca_flow_pipe_cfg_set_nr_entries (demux)");
  err = doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
  doca_check(err, "doca_flow_pipe_cfg_set_match (demux)");

  err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
  doca_check(err, "doca_flow_pipe_create (demux)");
  doca_flow_pipe_cfg_destroy(cfg);

  struct doca_flow_match entry_match = {0};
  struct doca_flow_fwd entry_fwd;
  struct entry_batch_status status = {0};
  struct doca_flow_pipe_entry *entry;

  // port 0 (p0 wire ingress) -> FORWARD; batch, flush with next entry
  entry_match.parser_meta.port_id = 0;
  memset(&entry_fwd, 0, sizeof(entry_fwd));
  entry_fwd.type = DOCA_FLOW_FWD_PIPE;
  entry_fwd.next_pipe = fwd_pipe;
  err = doca_flow_pipe_basic_add_entry(0, pipe, &entry_match, 0, NULL, NULL, &entry_fwd,
                                       DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH, &status, &entry);
  doca_check(err, "doca_flow_pipe_basic_add_entry (demux wire->fwd)");

  // port 1 (mlx5_2 SF egress) -> p0 wire; NO_WAIT flushes the batch
  entry_match.parser_meta.port_id = 1;
  memset(&entry_fwd, 0, sizeof(entry_fwd));
  entry_fwd.type = DOCA_FLOW_FWD_PORT;
  entry_fwd.port_id = 0;
  err = doca_flow_pipe_basic_add_entry(0, pipe, &entry_match, 0, NULL, NULL, &entry_fwd,
                                       DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, &status, &entry);
  doca_check(err, "doca_flow_pipe_basic_add_entry (demux sf->wire)");

  err = doca_flow_entries_process(port, 0, 10000, 2);
  doca_check(err, "doca_flow_entries_process (demux)");
  err = (status.failure || status.nb_processed != 2) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS;
  doca_check(err, "demux entry installation: %u/2 processed", status.nb_processed);

  DOCA_LOG_INFO("Port demux ready: p0 wire->FORWARD, mlx5_2 SF->p0 wire");
  return pipe;
}

static void print_stats(struct doca_flow_pipe_entry *fwd_entry) {
  struct doca_flow_resource_query query;
  doca_error_t err = doca_flow_resource_query_entry(fwd_entry, &query);
  doca_check(err, "doca_flow_resource_query_entry");

  DOCA_LOG_INFO("Forwarded: %lu", query.counter.total_pkts);
}

int main(int argc, char **argv) {
  doca_error_t err;

  err = doca_log_backend_create_standard();
  doca_check(err, "doca_log_backend_create_standard");

  struct doca_log_backend *sdk_log;
  err = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
  doca_check(err, "doca_log_backend_create_with_file_sdk");

  err = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
  doca_check(err, "doca_log_backend_set_sdk_level");

  err = doca_argp_init("doca_flow_nop", NULL);
  doca_check(err, "doca_argp_init");

  doca_argp_set_dpdk_program(initialize_dpdk);
  register_sf_num_param();

  err = doca_argp_start(argc, argv);
  doca_check(err, "doca_argp_start");

  struct doca_dev *dev = open_and_probe_dev(0);

  configure_and_start_dpdk_port(dev);

  initialize_doca_flow();

  // DPDK 0: PF0 uplink
  struct doca_flow_port *port = port_start(dev);
  struct doca_dev_rep *sf_rep = open_sf_representor(dev);
  struct doca_flow_port *sf_rep_port =
      // PF0 SF rep (mlx5_2)
      rep_port_start(find_sf_representor_port_id(), sf_rep);

  struct doca_flow_pipe *fwd_pipe = create_fwd_pipe(port, 1, /*with_counter=*/true);
  struct doca_flow_pipe_entry *fwd_entry = add_fwd_entry(fwd_pipe, port, /*with_counter=*/true);

  struct doca_flow_pipe *demux_pipe = create_root_pipe(port, fwd_pipe);
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
