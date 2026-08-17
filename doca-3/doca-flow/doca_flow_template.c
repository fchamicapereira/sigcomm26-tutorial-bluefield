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
#include <time.h>
#include <unistd.h>

DOCA_LOG_REGISTER(FLOW_ECN);

#define NB_QUEUES 1
#define RANDOM_FIELD_WIDTH 16

// The two DPDK ports this program forwards between.
//
//   PF_PORT_ID      p0, the uplink. THE WIRE -- not an endpoint, and not a side of the tutorial's
//                   client/server pair. The client is not on PF0 at all: it is an SF on PF1, and
//                   its traffic reaches this program over the cable between the two ports. So
//                   traffic ARRIVING from PF_PORT_ID came from the client, and traffic FORWARDED
//                   to PF_PORT_ID is on its way back to the client. Which end it stands for
//                   depends on the direction, and create_root_pipe() uses it both ways: once as a
//                   match (packets that arrived here) and once as a destination (send it out
//                   here).
//   SF_REP_PORT_ID  the receiver SF's representor. THE SERVER, always -- the local end, on this
//                   card's PF0, reached through mlx5_2 inside ns0.
//
// Why those numbers: DPDK numbers ethdevs in the order they are probed, and open_and_probe_dev()
// probes exactly two: doca_dpdk_port_probe() brings up the PF uplink itself, and its devargs ask
// for one representor ("representor=sf0"), the receiver SF's. The PF is therefore always 0 and
// that representor always 1 — it is the probe string that decides this, not anything about the
// hardware, so changing the devargs is what would change these ids. find_pf_port_id() derives the
// PF's id independently at startup, by finding the one ethdev that is not a representor, so a
// mismatch there would surface rather than corrupt the pipeline silently.
#define PF_PORT_ID 0
#define SF_REP_PORT_ID 1

// The queue every entry in this program is installed on. DOCA lets rules be added from several
// threads at once, each on its own queue; this program installs all of its entries once, at
// startup, from the main thread, so there is only ever queue 0.
#define PIPE_QUEUE 0

// How long doca_flow_entries_process() may wait, in microseconds, for the hardware to confirm the
// entries handed to it. Installation is asynchronous — the add-entry call returns as soon as the
// driver has taken the rule, and the verdict arrives later through entry_process_cb().
#define ENTRY_PROCESS_TIMEOUT_US 10000

// Descriptor ring sizes. Nothing is ever received on the CPU queues here -- every packet is
// forwarded inside the eSwitch -- but DPDK still requires both rings to exist before a port starts.
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 512

// mbuf pool shared by both DPDK ports (PF uplink + SF representor), plus a per-core cache.
#define MBUF_POOL_SIZE 8192
#define MBUF_CACHE_SIZE 256

// Largest frame the RX buffers must be able to hold. Sized for jumbo on purpose: an SF may be
// configured with any MTU, and this program cannot discover which.
#define MAX_FRAME_SIZE 9216

// HWS on 3.4 needs a per-port action-memory pool for any pipe carrying a modify action.
// Sized via DOCA's own next_pow2(entries * DOCA_FLOW_MAX_ENTRY_ACTIONS_MEM_SIZE + 1024).
#define ACTIONS_MEM_SIZE (16 * 1024)

struct app_config {
  // --percent, [0,100], default 100
  double random_percent;
};

// What build_pipeline() hands back to the rest of the program: the two counter entries the
// once-a-second report queries, and the sampling mask the startup banner prints. Nothing else
// outside the pipeline needs to know which pipes exist or how they are chained.
struct pipeline {
  // MARK's entry — counts CE-marked packets
  struct doca_flow_pipe_entry *ce_entry;
  // PASS's entry — counts unmarked packets
  struct doca_flow_pipe_entry *pass_entry;
  // 0 unless --percent is strictly between 0 and 100
  uint16_t sample_mask;
};

static volatile bool g_running = true;

static void signal_handler(int s) {
  if (s == SIGINT || s == SIGTERM) g_running = false;
}

static __attribute__((format(printf, 2, 3))) void doca_check(doca_error_t err, const char *fmt,
                                                             ...) {
  if (err == DOCA_SUCCESS) return;
  char msg[512];
  va_list a;
  va_start(a, fmt);
  vsnprintf(msg, sizeof(msg), fmt, a);
  va_end(a);
  DOCA_LOG_CRIT("%s: %s", msg, doca_error_get_descr(err));
  exit(EXIT_FAILURE);
}

// Run a DOCA call and abort if it fails. `ctx` names the pipe or the setup phase the call belongs
// to; the call's own source text supplies the rest, so the message points at the exact call that
// failed without a hand-written label to keep in step with it.
#define DOCA_CHECK(ctx, expr) doca_check((expr), "%s: %s", (ctx), #expr)

// percent -> nearest power-of-two random mask (same technique as doca_flow_ecn).
static uint16_t get_random_mask(double percentage) {
  double next = 50.0;
  uint8_t i;
  for (i = 1; i <= RANDOM_FIELD_WIDTH; ++i) {
    if (percentage >= next) break;
    next /= 2;
  }
  return (uint16_t)((1u << i) - 1);
}

struct entry_batch_status {
  bool failure;
  uint32_t nb_processed;
};
static void entry_process_cb(struct doca_flow_pipe_entry *e, uint16_t q,
                             enum doca_flow_entry_status st, enum doca_flow_entry_op op,
                             void *ctx) {
  (void)e;
  (void)q;
  (void)op;
  struct entry_batch_status *s = ctx;
  if (!s) return;
  if (st != DOCA_FLOW_ENTRY_STATUS_SUCCESS) s->failure = true;
  s->nb_processed++;
}

// EAL bring-up. Not called directly — doca_argp_set_dpdk_program() registers it, and doca_argp
// invokes it with the EAL half of the command line: everything BEFORE the "--" separator.
//
// By default EAL probes every device it can see, and binding the SFs out from under the kernel
// would break the RoCE traffic this program is meant to watch. Passing any -a (allowlist) flag
// flips EAL from "probe everything" to "probe only what is listed", and the address listed here
// does not exist — so the allowlist is empty and EAL probes nothing at all. The port actually
// wanted is attached afterwards, explicitly, by doca_dpdk_port_probe() on the device
// doca_dev_open() returned. This mirrors DOCA's own dpdk_init_without_probing() in dpdk_utils.c.
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
  char *nv[64];
  if (argc >= 60) {
    DOCA_LOG_ERR("Too many EAL arguments");
    return DOCA_ERROR_INVALID_VALUE;
  }
  for (int i = 0; i < argc; i++) nv[i] = argv[i];
  nv[argc] = allow_flag;
  nv[argc + 1] = dummy_pci;
  nv[argc + 2] = allow_flag;
  nv[argc + 3] = dummy_aux;
  if (rte_eal_init(argc + 4, nv) < 0) {
    DOCA_LOG_ERR("EAL initialization failed");
    return DOCA_ERROR_DRIVER;
  }
  return DOCA_SUCCESS;
}

// Open the DOCA device at `index` (0 is the first, which is PF0) and probe it into DPDK.
//
// The probe string is an mlx5 PMD device-argument list, and every token in it is load-bearing:
//
//   dv_flow_en=2       Hardware steering (HWS), which DOCA Flow's "switch,hws" mode requires.
//   fdb_def_rule_en=1  Keep mlx5's default FDB jump rule. Each PF owns a separate FDB domain and
//                      this program only ever programs PF0's; leaving the default in place is what
//                      keeps PF1 forwarding through its OVS bridge, which is how the sender SF's
//                      traffic reaches p1 and the wire. DOCA's own switch samples set this to 0
//                      because they own both sides — here it must stay 1.
//   representor=sf0    Also probe the receiver SF's representor, so it shows up as a second DPDK
//                      port. This is the line that gives PF_PORT_ID and SF_REP_PORT_ID their
//                      values: the PF is probed first and the representor second.
static struct doca_dev *open_and_probe_dev(uint32_t index) {
  struct doca_devinfo **list;
  uint32_t n;
  struct doca_dev *dev;
  DOCA_CHECK("device", doca_devinfo_create_list(&list, &n));
  if (index >= n) {
    DOCA_LOG_CRIT("Device index %u out of range (%u)", index, n);
    exit(EXIT_FAILURE);
  }
  DOCA_CHECK("device", doca_dev_open(list[index], &dev));
  doca_devinfo_destroy_list(list);
  DOCA_CHECK("device", doca_dpdk_port_probe(dev, "dv_flow_en=2,fdb_def_rule_en=1,representor=sf0"));
  return dev;
}

// Find PF0's SF representor and open it as a doca_dev_rep.
//
// 3.4 binds the representor to its DOCA Flow port through this handle, where the 2.x build simply
// passed the DPDK port index as a devargs string. Only the first net representor reporting an SF
// index is taken — setup_roce_loopback.sh imposes exactly one SF per PF, so there is no ambiguity.
static struct doca_dev_rep *open_sf_representor(struct doca_dev *pf_dev) {
  struct doca_devinfo_rep **rep_list;
  uint32_t nb_reps;
  struct doca_dev_rep *rep = NULL;
  doca_error_t err;

  DOCA_CHECK("sf representor", doca_devinfo_rep_create_list(pf_dev, DOCA_DEVINFO_REP_FILTER_NET,
                                                            &rep_list, &nb_reps));

  for (uint32_t i = 0; i < nb_reps; i++) {
    uint32_t sf_index;
    if (doca_devinfo_rep_get_sf_index(rep_list[i], &sf_index) == DOCA_SUCCESS) {
      err = doca_dev_rep_open(rep_list[i], &rep);
      doca_check(err, "doca_dev_rep_open (sf_index=%u)", sf_index);
      DOCA_LOG_INFO("Opened SF representor (sf_index=%u) as port 1", sf_index);
      break;
    }
  }
  doca_devinfo_rep_destroy_list(rep_list);

  if (rep == NULL) {
    DOCA_LOG_CRIT("SF representor not found on PF0");
    exit(EXIT_FAILURE);
  }
  return rep;
}

static void configure_and_start_dpdk_port(struct doca_dev *dev) {
  uint16_t first;
  DOCA_CHECK("dpdk port", doca_dpdk_get_first_port_id(dev, &first));
  // Size the mbufs for the largest frame that can arrive, not for the DPDK ports' MTU. The frames
  // crossing this card are whatever the SFs are configured to send, and that MTU is not
  // discoverable from here: the DPDK ports are the PF uplink and the SF representor, which report
  // the uplink's MTU, while the SF netdev that determines the frame size lives in a separate
  // network namespace and is not a DPDK port at all. Sizing for jumbo removes the question, at the
  // cost of a larger mempool.
  uint16_t data_room = RTE_PKTMBUF_HEADROOM + MAX_FRAME_SIZE;
  DOCA_LOG_INFO("mbuf data room %u bytes (jumbo-capable, max frame %u)", data_room, MAX_FRAME_SIZE);

  struct rte_mempool *mp = rte_pktmbuf_pool_create("mbuf_pool", MBUF_POOL_SIZE, MBUF_CACHE_SIZE, 0,
                                                   data_room, rte_eth_dev_socket_id(first));
  if (!mp) {
    DOCA_LOG_CRIT("rte_pktmbuf_pool_create failed");
    exit(EXIT_FAILURE);
  }
  uint16_t pid;
  RTE_ETH_FOREACH_DEV(pid) {
    struct rte_eth_dev_info di = {0};
    if (rte_eth_dev_info_get(pid, &di) < 0) {
      DOCA_LOG_CRIT("dev_info port %u", pid);
      exit(EXIT_FAILURE);
    }
    struct rte_eth_conf ec = {0};
    // Ask the PMD for the largest MTU it will accept, so its RQ is not sized for 1500 either.
    uint16_t want_mtu = MAX_FRAME_SIZE - RTE_ETHER_HDR_LEN - RTE_ETHER_CRC_LEN;
    ec.rxmode.mtu = (di.max_mtu && want_mtu > di.max_mtu) ? di.max_mtu : want_mtu;
    if (rte_eth_dev_configure(pid, NB_QUEUES, NB_QUEUES, &ec) < 0) {
      DOCA_LOG_CRIT("configure %u", pid);
      exit(EXIT_FAILURE);
    }
    struct rte_eth_txconf tx = di.default_txconf;
    for (int q = 0; q < NB_QUEUES; q++) {
      if (rte_eth_rx_queue_setup(pid, q, RX_RING_SIZE, rte_eth_dev_socket_id(pid), NULL, mp) < 0) {
        DOCA_LOG_CRIT("rxq %u", pid);
        exit(EXIT_FAILURE);
      }
      if (rte_eth_tx_queue_setup(pid, q, TX_RING_SIZE, rte_eth_dev_socket_id(pid), &tx) < 0) {
        DOCA_LOG_CRIT("txq %u", pid);
        exit(EXIT_FAILURE);
      }
    }
    if (rte_eth_dev_start(pid) < 0) {
      DOCA_LOG_CRIT("start %u", pid);
      exit(EXIT_FAILURE);
    }
  }
}

static void initialize_doca_flow(void) {
  struct doca_flow_cfg *cfg;
  DOCA_CHECK("doca_flow init", doca_flow_cfg_create(&cfg));
  DOCA_CHECK("doca_flow init", doca_flow_cfg_set_pipe_queues(cfg, NB_QUEUES));
  // Mode args, one comma-separated token at a time:
  //
  //   switch   Program the eSwitch (FDB) rather than a plain NIC ingress domain, so pipes
  //            forward between eSwitch ports — the wire, the SF representor. This program
  //            becomes PF0's forwarding plane while it runs.
  //   hws      Hardware steering, the counterpart of dv_flow_en=2 in the probe string.
  //
  // Plain "switch,hws" here, unlike the 2.x build which also passes "isolated,
  // disable_switch_rss". Those exist to stop DOCA creating an internal FDB RSS context that
  // BlueField firmware rejects, and this is the mode doca_flow_nop is known to work with.
  DOCA_CHECK("doca_flow init", doca_flow_cfg_set_mode_args(cfg, "switch,hws"));
  DOCA_CHECK("doca_flow init", doca_flow_cfg_set_nr_counters(cfg, 4));
  DOCA_CHECK("doca_flow init", doca_flow_cfg_set_cb_entry_process(cfg, entry_process_cb));
  DOCA_CHECK("doca_flow init", doca_flow_init(cfg));
  doca_flow_cfg_destroy(cfg);
}

// DPDK port id of the PF uplink — the eSwitch "proxy" port, which DOCA Flow requires to be the
// FIRST port started in switch mode.
//
// Deliberately not doca_dpdk_get_first_port_id(): on DOCA 2.7 that returns the SF representor
// (DPDK port 1) rather than the PF (port 0). Starting the representor first makes
// doca_flow_port_start fail with failed getting is_switch_manager property - proxy port 0 not found
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
  uint16_t pid = find_pf_port_id();
  struct doca_flow_port_cfg *cfg;
  DOCA_CHECK("pf port", doca_flow_port_cfg_create(&cfg));
  DOCA_CHECK("pf port", doca_flow_port_cfg_set_dev(cfg, dev));
  DOCA_CHECK("pf port", doca_flow_port_cfg_set_port_id(cfg, pid));
  DOCA_CHECK("pf port", doca_flow_port_cfg_set_actions_mem_size(cfg, ACTIONS_MEM_SIZE));
  struct doca_flow_port *port;
  DOCA_CHECK("pf port", doca_flow_port_start(cfg, &port));
  doca_flow_port_cfg_destroy(cfg);
  return port;
}

static struct doca_flow_port *rep_port_start(uint16_t pid, struct doca_dev_rep *dev_rep) {
  struct doca_flow_port_cfg *cfg;
  DOCA_CHECK("rep port", doca_flow_port_cfg_create(&cfg));
  DOCA_CHECK("rep port", doca_flow_port_cfg_set_dev_rep(cfg, dev_rep));
  DOCA_CHECK("rep port", doca_flow_port_cfg_set_port_id(cfg, pid));
  struct doca_flow_port *port;
  DOCA_CHECK("rep port", doca_flow_port_start(cfg, &port));
  doca_flow_port_cfg_destroy(cfg);
  return port;
}

static uint64_t query_pkts(struct doca_flow_pipe_entry *e) {
  if (!e) return 0;
  struct doca_flow_resource_query q;
  return (doca_flow_resource_query_entry(e, &q) == DOCA_SUCCESS) ? q.counter.total_pkts : 0;
}

static doca_error_t percent_cb(void *p, void *c) {
  struct app_config *cfg = c;
  double v = atof((const char *)p);
  if (v < 0.0 || v > 100.0) {
    DOCA_LOG_ERR("--percent must be [0,100]");
    return DOCA_ERROR_INVALID_VALUE;
  }
  cfg->random_percent = v;
  return DOCA_SUCCESS;
}
static void register_params(void) {
  struct doca_argp_param *p;
  DOCA_CHECK("argp", doca_argp_param_create(&p));
  doca_argp_param_set_long_name(p, "percent");
  doca_argp_param_set_description(p,
                                  "Percent of packets to CE-mark [0,100] (rounded down to a "
                                  "power-of-2 fraction; default 100).");
  doca_argp_param_set_callback(p, percent_cb);
  doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
  DOCA_CHECK("argp", doca_argp_register_param(p));
}

static void setup_logging(void) {
  DOCA_CHECK("logging", doca_log_backend_create_standard());
  struct doca_log_backend *sdk;
  DOCA_CHECK("logging", doca_log_backend_create_with_file_sdk(stderr, &sdk));
  DOCA_CHECK("logging", doca_log_backend_set_sdk_level(sdk, DOCA_LOG_LEVEL_WARNING));
}

static void parse_args(int argc, char **argv, struct app_config *cfg) {
  DOCA_CHECK("argp", doca_argp_init("doca_flow_ecn", cfg));
  doca_argp_set_dpdk_program(initialize_dpdk);
  register_params();
  DOCA_CHECK("argp", doca_argp_start(argc, argv));
}

static void install_signal_handlers(void) {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
}

// The banner: what the pipeline was built to do, and how to stop it.
static void log_startup(const struct app_config *cfg, const struct pipeline *pl) {
  if (cfg->random_percent >= 100.0)
    DOCA_LOG_INFO("Marking ALL IPv4 -- Ctrl-C to stop");
  else if (cfg->random_percent <= 0.0)
    DOCA_LOG_INFO("Marking NONE -- Ctrl-C to stop");
  else
    DOCA_LOG_INFO("Marking ~%.4g%% of IPv4 -- Ctrl-C to stop", 100.0 / (pl->sample_mask + 1));
}

// Report the counters once a second, until SIGINT/SIGTERM. Every packet is handled inside the
// eSwitch, so this loop touches no packet data at all -- it only reads the hardware counters the
// pipeline's entries carry.
static void run_report_loop(const struct pipeline *pl) {
  time_t last = time(NULL);
  while (g_running) {
    usleep(1000);
    time_t now = time(NULL);
    if (now == last) continue;
    last = now;
    uint64_t ce = query_pkts(pl->ce_entry), pass = query_pkts(pl->pass_entry), tot = ce + pass;
    DOCA_LOG_INFO("CE marked: %lu, passthrough: %lu (%.4g%% marked)", ce, pass,
                  tot ? 100.0 * (double)ce / (double)tot : 0.0);
  }
}

// ================================================================================
// The DOCA Flow pipeline.
//
// Everything above this point is setup and runtime plumbing. Everything from here
// down to build_pipeline() is the eSwitch pipeline itself.
// ================================================================================

// PASSTHROUGH -- the fallback forward, and the worked example the exercise is modelled on.
//
// It declares no match field at all, so every packet reaches its single entry, and forwards the lot
// to port 1, the receiver SF. No counter and no CE marking: it moves the packet and does nothing
// else, which makes it the smallest complete instance of the shape every pipe in this file follows.
//
// Matching everything is what lets it serve as MARK's and PASS's miss target. Those two match IPv4,
// so their miss is ARP and the like, and this pipe has to carry that traffic rather than drop it:
// create_root_pipe_nop() forwards everything, so a pipeline that dropped non-IPv4 would be less
// transparent than the no-op it replaces, and a RoCE connection established while the pipeline is
// live needs its ARP to get through.
//
// build_pipeline() aims the root pipe here in Step 4.1, before there is anything else to aim it at.
static struct doca_flow_pipe *create_passthrough_pipe(struct doca_flow_port *port) {
  struct doca_flow_pipe_cfg *cfg;

  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_cfg_create(&cfg, port));
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_cfg_set_name(cfg, "PASSTHROUGH"));
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC));
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT));
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_cfg_set_is_root(cfg, false));

  const uint32_t nb_entries = 1;
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_cfg_set_nr_entries(cfg, nb_entries));

  // Both left all-zero: no field is declared, so nothing is compared and everything matches.
  struct doca_flow_match match = {0}, match_mask = {0};
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask));

  struct doca_flow_fwd fwd_hit = {.type = DOCA_FLOW_FWD_PORT, .port_id = SF_REP_PORT_ID};
  struct doca_flow_pipe *pipe;
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_create(cfg, &fwd_hit, NULL, &pipe));

  doca_flow_pipe_cfg_destroy(cfg);

  // `match` is reused as this entry's values -- all zero, like the template above.
  struct entry_batch_status install_status = {0};
  struct doca_flow_pipe_entry *entry;
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_basic_add_entry(PIPE_QUEUE, pipe, &match, 0, NULL, NULL,
                                                           NULL, DOCA_FLOW_ENTRY_FLAGS_NO_WAIT,
                                                           &install_status, &entry));

  DOCA_CHECK("PASSTHROUGH",
             doca_flow_entries_process(port, PIPE_QUEUE, ENTRY_PROCESS_TIMEOUT_US, nb_entries));
  doca_check((install_status.failure || install_status.nb_processed != nb_entries)
                 ? DOCA_ERROR_BAD_STATE
                 : DOCA_SUCCESS,
             "PASSTHROUGH: install");
  return pipe;
}

// The main forwarding pipe: wire IPv4 -> the receiver SF. build_pipeline() builds it TWICE:
//
//   PASS   mark=false   forward the packet unchanged
//   MARK   mark=true    forward it, setting ECN CE on the way through
//
// Whichever instance, the constant behaviour is the same: match IPv4 regardless of the ECN bits it
// arrived with, count it, and forward to port 1.
// The counter is not incidental -- it is what the once-a-second "CE marked: / passthrough:" report
// queries, and without it there is no way to see whether the pipeline is doing anything.
//
// NOTHING IS DROPPED HERE: a miss goes to miss_pipe, which is PASSTHROUGH, which forwards it to the
// SF untouched. What the match decides is what gets COUNTED, and when `mark` is set, MARKED --
// non-IPv4 (ARP and the like) misses and travels on uncounted and unmarked.
//
// The miss cannot name a port directly. DOCA only accepts a pipe or a drop as a miss forward, and
// rejects DOCA_FLOW_FWD_PORT with "invalid fwd_miss type 2" at pipe_create() time. Handing the miss
// to a pipe that forwards everything is how you say "carry on" -- which is what PASSTHROUGH is for.
//
// `mark` is the only thing that differs between the two: it attaches the action that rewrites
// dscp_ecn to CE. That is not what the pipe is FOR -- forwarding is -- which is why the ECN part of
// the exercise is only the action.
//
// out_entry hands the installed entry back so the report can query its counter.
static struct doca_flow_pipe *create_forward_to_sf_pipe(struct doca_flow_port *port, bool mark,
                                                        struct doca_flow_pipe *miss_pipe,
                                                        struct doca_flow_pipe_entry **out_entry) {
  const char *name = mark ? "MARK" : "PASS";
  struct doca_flow_pipe_cfg *cfg;

  DOCA_CHECK(name, doca_flow_pipe_cfg_create(&cfg, port));
  DOCA_CHECK(name, doca_flow_pipe_cfg_set_name(cfg, name));
  DOCA_CHECK(name, doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC));

  // Default pipe domain for actions on the ingress traffic.
  DOCA_CHECK(name, doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT));

  // Not the root pipe (i.e. not the first pipe to see the packet). The root pipe is PORT_DEMUX,
  // which is built in build_pipeline() and is the only pipe with is_root=true.
  DOCA_CHECK(name, doca_flow_pipe_cfg_set_is_root(cfg, false));

  // One entry: the match template. The actual entry is added below with the add-entry call.
  const uint32_t nb_entries = 1;
  DOCA_CHECK(name, doca_flow_pipe_cfg_set_nr_entries(cfg, nb_entries));

  // A field set in the template but zeroed in the mask is declared without being compared, which is
  // how dscp_ecn is treated here: MARK has to catch packets that arrive already CE-marked as
  // readily as fresh ones. l3_type has no mask entry, so it is compared exactly.
  struct doca_flow_match match = {0}, match_mask = {0};
  struct doca_flow_actions action_template = {0}, *action_templates[1] = {&action_template};
  struct doca_flow_monitor monitor = {0};
  struct doca_flow_fwd fwd_hit = {0};
  struct doca_flow_fwd fwd_miss = {0};
  struct doca_flow_pipe *pipe = NULL;

  // TODO 2a -- build the pipe. In order:
  //   1. the match, any IPv4 packet whatever its ECN bits:
  //        match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  //        match.outer.ip4.dscp_ecn = 0xFF;        -- the byte participates...
  //        match_mask.outer.ip4.dscp_ecn = 0x00;   -- ...but is not compared
  //        doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask)
  //   2. the action, ONLY when `mark` is true:
  //        action_template.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  //        action_template.outer.ip4.dscp_ecn = 0xFF;   -- entries may rewrite this byte
  //        doca_flow_pipe_cfg_set_actions(cfg, action_templates, NULL, NULL, 1)
  //   3. the counter, which is what the CE marked: report reads:
  //        monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
  //        doca_flow_pipe_cfg_set_monitor(cfg, &monitor)
  //   4. the forwards -- a hit goes to the SF, a miss carries on into miss_pipe
  //      (PASSTHROUGH), which forwards everything, so nothing is dropped here:
  //        fwd_hit.type = DOCA_FLOW_FWD_PORT;   fwd_hit.port_id = SF_REP_PORT_ID;
  //        fwd_miss.type = DOCA_FLOW_FWD_PIPE;  fwd_miss.next_pipe = miss_pipe;
  //        -- a miss forward may only be a pipe or a drop. DOCA_FLOW_FWD_PORT is
  //           rejected here with "invalid fwd_miss type 2".
  //   5. doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe)
  // Wrap each call in DOCA_CHECK(name, ...).

  // pipe_create() has read the whole cfg and the pipe keeps no reference to it.
  doca_flow_pipe_cfg_destroy(cfg);

  struct doca_flow_actions entry_actions = {0};
  struct entry_batch_status install_status = {0};

  // TODO 2b -- add the one entry and install it. In order:
  //   1. the value to write, ONLY when `mark` is true:
  //      entry_actions.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  //      entry_actions.outer.ip4.dscp_ecn = 0x03;   -- both ECN bits set: CE
  //   2. match.outer.ip4.dscp_ecn = 0x00;   -- `match` is reused as this entry's values,
  //                                            so drop the 0xFF placeholder from step 1 above
  //   3. doca_flow_pipe_basic_add_entry(PIPE_QUEUE, pipe, &match, 0,
  //                               mark ? &entry_actions : NULL, &monitor, NULL,
  //                               DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, &install_status, out_entry)
  //      -- out_entry, not &entry: the counter report queries the entry you hand back
  //   4. doca_flow_entries_process(port, PIPE_QUEUE, ENTRY_PROCESS_TIMEOUT_US, nb_entries)
  //   5. fail unless install_status.failure is false and .nb_processed == nb_entries
  DOCA_LOG_INFO("%s pipe ready (%s)", name, mark ? "CE-mark" : "no-mark");
  return pipe;
}

// RANDOM_SAMPLE — splits wire traffic between the marking and non-marking paths, in hardware.
//
// Only built when --percent is strictly between 0 and 100. At either extreme build_pipeline() aims
// the wire straight at one forwarding pipe and skips this stage altogether.
//
// The parser stamps every packet with a random 16-bit value in parser_meta.random. Matching that
// field against 0 under `mask` — a power of two minus one — therefore hits for 1 packet in
// (mask + 1). Hits go to `hit` (MARK), misses to `miss` (PASS). Both of those
// forward the packet onward, so "miss" here means "not selected for marking", not an error path.
static struct doca_flow_pipe *create_sampling_pipe(struct doca_flow_port *port,
                                                   struct doca_flow_pipe *hit,
                                                   struct doca_flow_pipe *miss, uint16_t mask) {
  struct doca_flow_pipe_cfg *cfg;

  DOCA_CHECK("RANDOM_SAMPLE", doca_flow_pipe_cfg_create(&cfg, port));
  DOCA_CHECK("RANDOM_SAMPLE", doca_flow_pipe_cfg_set_name(cfg, "RANDOM_SAMPLE"));
  DOCA_CHECK("RANDOM_SAMPLE", doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC));
  DOCA_CHECK("RANDOM_SAMPLE", doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT));
  DOCA_CHECK("RANDOM_SAMPLE", doca_flow_pipe_cfg_set_is_root(cfg, false));

  const uint32_t nb_entries = 1;
  DOCA_CHECK("RANDOM_SAMPLE", doca_flow_pipe_cfg_set_nr_entries(cfg, nb_entries));

  struct doca_flow_match match = {0}, match_mask = {0};
  struct doca_flow_fwd fwd_hit = {0};
  struct doca_flow_fwd fwd_miss = {0};
  struct doca_flow_pipe *pipe = NULL;

  // TODO 3a -- build the pipe. In order:
  //   1. match.parser_meta.random = 0;
  //   2. match_mask.parser_meta.random = mask;   -- `mask` is a power of two minus one,
  //                                                 so this hits 1 packet in (mask + 1)
  //   3. doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask)
  //   4. fwd_hit.type = DOCA_FLOW_FWD_PIPE;   fwd_hit.next_pipe = hit;
  //      fwd_miss.type = DOCA_FLOW_FWD_PIPE;  fwd_miss.next_pipe = miss;
  //      -- both are pipes, and both go on to the receiver; a miss here means
  //         `not selected for marking', not an error
  //   5. doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe)
  // Wrap each call in DOCA_CHECK("RANDOM_SAMPLE", ...).

  doca_flow_pipe_cfg_destroy(cfg);

  // The entry adds nothing to the template: no actions, no counter, and no forward of its own, so
  // both outcomes are decided by the pipe's own two forwards.
  struct entry_batch_status install_status = {0};
  struct doca_flow_pipe_entry *entry;

  // TODO 3b -- add the one entry and install it. In order:
  //   1. doca_flow_pipe_basic_add_entry(PIPE_QUEUE, pipe, &match, 0, NULL, NULL, NULL,
  //                               DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, &install_status, &entry)
  //   2. doca_flow_entries_process(port, PIPE_QUEUE, ENTRY_PROCESS_TIMEOUT_US, nb_entries)
  //   3. fail unless install_status.failure is false and .nb_processed == nb_entries
  DOCA_LOG_INFO("Random-sample pipe ready: mask 0x%04x", mask);
  return pipe;
}

// PORT_DEMUX_NOP -- the root pipe this program ships with in the exercise template, and the worked
// example for the one you write next.
//
// On its own it is already a complete, working forwarder: the program takes ownership of PF0's
// eSwitch and then moves packets between the wire and the receiver SF exactly as the card would
// have done on its own, marking nothing and counting nothing.
//
// It differs from create_root_pipe() below by two lines. Both sort by parser_meta.port_id, the
// port a packet arrived on, and both drop whatever matches neither direction. But this one sends
// wire traffic STRAIGHT OUT to the receiver SF (DOCA_FLOW_FWD_PORT), where create_root_pipe() sends
// it INTO ANOTHER PIPE (DOCA_FLOW_FWD_PIPE) -- the head of the marking chain. That is the whole
// difference between a forwarder and a pipeline.
//
// build_pipeline() below calls exactly one of the two roots: this one in the exercise as shipped,
// and create_root_pipe() once Step 4.1 is done. __attribute__((unused)) is there because whichever
// of those two states you are in leaves the other function uncalled, and -Wall would say so.
static void __attribute__((unused)) create_root_pipe_nop(struct doca_flow_port *port) {
  struct doca_flow_pipe_cfg *cfg;

  DOCA_CHECK("PORT_DEMUX_NOP", doca_flow_pipe_cfg_create(&cfg, port));
  DOCA_CHECK("PORT_DEMUX_NOP", doca_flow_pipe_cfg_set_name(cfg, "PORT_DEMUX_NOP"));
  DOCA_CHECK("PORT_DEMUX_NOP", doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC));
  DOCA_CHECK("PORT_DEMUX_NOP", doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT));
  DOCA_CHECK("PORT_DEMUX_NOP", doca_flow_pipe_cfg_set_is_root(cfg, true));

  // One entry per direction: in from the wire, and back from the receiver SF.
  const uint32_t nb_entries = 2;
  DOCA_CHECK("PORT_DEMUX_NOP", doca_flow_pipe_cfg_set_nr_entries(cfg, nb_entries));

  // A full mask on the ingress port: it is compared exactly, and each entry supplies the port it
  // matches.
  struct doca_flow_match match = {0}, match_mask = {0};
  struct doca_flow_fwd fwd_hit = {0};
  struct doca_flow_fwd fwd_miss = {0};
  struct doca_flow_pipe *pipe = NULL;

  match.parser_meta.port_id = 0xFFFF;
  match_mask.parser_meta.port_id = 0xFFFF;
  DOCA_CHECK("PORT_DEMUX_NOP", doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask));

  fwd_hit.type = DOCA_FLOW_FWD_CHANGEABLE;
  fwd_miss.type = DOCA_FLOW_FWD_DROP;
  DOCA_CHECK("PORT_DEMUX_NOP", doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe));

  doca_flow_pipe_cfg_destroy(cfg);

  struct entry_batch_status install_status = {0};
  struct doca_flow_pipe_entry *entry;
  struct doca_flow_match entry_match = {0};
  struct doca_flow_fwd entry_fwd = {0};

  // From the wire: straight out to the receiver SF. This is the entry create_root_pipe() changes.
  entry_match.parser_meta.port_id = PF_PORT_ID;
  entry_fwd.type = DOCA_FLOW_FWD_PORT;
  entry_fwd.port_id = SF_REP_PORT_ID;
  DOCA_CHECK("PORT_DEMUX_NOP", doca_flow_pipe_basic_add_entry(
                                   PIPE_QUEUE, pipe, &entry_match, 0, NULL, NULL, &entry_fwd,
                                   DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH, &install_status, &entry));

  // From the receiver SF: straight back out of the uplink, untouched.
  entry_match.parser_meta.port_id = SF_REP_PORT_ID;
  memset(&entry_fwd, 0, sizeof(entry_fwd));
  entry_fwd.type = DOCA_FLOW_FWD_PORT;
  entry_fwd.port_id = PF_PORT_ID;
  DOCA_CHECK("PORT_DEMUX_NOP", doca_flow_pipe_basic_add_entry(
                                   PIPE_QUEUE, pipe, &entry_match, 0, NULL, NULL, &entry_fwd,
                                   DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, &install_status, &entry));

  DOCA_CHECK("PORT_DEMUX_NOP",
             doca_flow_entries_process(port, PIPE_QUEUE, ENTRY_PROCESS_TIMEOUT_US, nb_entries));
  doca_check((install_status.failure || install_status.nb_processed != nb_entries)
                 ? DOCA_ERROR_BAD_STATE
                 : DOCA_SUCCESS,
             "PORT_DEMUX_NOP: install");
  DOCA_LOG_INFO("No-op forwarder ready: wire <-> receiver SF, nothing marked");
}

// PORT_DEMUX — the root pipe. Every packet entering PF0's eSwitch is looked up here first, and it
// is the only pipe in this file with is_root set. Until it exists, none of the others are
// reachable however correct they are.
//
// It sorts by direction, matching parser_meta.port_id, the port a packet arrived on:
//
//   port_id 0     from the wire (p0)     -> wire_target, the head of the marking chain
//   port_id 1     from the receiver SF   -> straight out port 0, back onto the wire
//   miss                                 -> dropped
//
// Separating the two directions is what keeps the RoCE return path clean: ACKs and CNPs coming back
// from the SF are forwarded untouched. Marking those would corrupt the very congestion feedback the
// PCC exercise in Part IV reacts to.
//
// The pipe-level forward is FWD_CHANGEABLE, which is DOCA's way of saying "each entry brings its
// own" — that is what lets the two directions go different places.
static void create_root_pipe(struct doca_flow_port *port, struct doca_flow_pipe *wire_target) {
  struct doca_flow_pipe_cfg *cfg;

  DOCA_CHECK("PORT_DEMUX", doca_flow_pipe_cfg_create(&cfg, port));
  DOCA_CHECK("PORT_DEMUX", doca_flow_pipe_cfg_set_name(cfg, "PORT_DEMUX"));
  DOCA_CHECK("PORT_DEMUX", doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC));
  DOCA_CHECK("PORT_DEMUX", doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT));
  DOCA_CHECK("PORT_DEMUX", doca_flow_pipe_cfg_set_is_root(cfg, true));

  // One entry per direction: in from the wire, and back from the receiver SF.
  const uint32_t nb_entries = 2;
  DOCA_CHECK("PORT_DEMUX", doca_flow_pipe_cfg_set_nr_entries(cfg, nb_entries));

  // A full mask on the ingress port: it is compared exactly, and each entry supplies the port it
  // matches.
  struct doca_flow_match match = {0}, match_mask = {0};
  struct doca_flow_fwd fwd_hit = {0};
  struct doca_flow_fwd fwd_miss = {0};
  struct doca_flow_pipe *pipe = NULL;

  // TODO 1a -- build the pipe. In order:
  //   1. match.parser_meta.port_id = 0xFFFF;       -- this field participates
  //   2. match_mask.parser_meta.port_id = 0xFFFF;  -- and is compared exactly
  //   3. doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask)
  //   4. fwd_hit.type = DOCA_FLOW_FWD_CHANGEABLE     -- each entry names its own target
  //   5. fwd_miss.type = DOCA_FLOW_FWD_DROP
  //   6. doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe)
  // Wrap each call in DOCA_CHECK("PORT_DEMUX", ...). create_root_pipe_nop() above builds
  // exactly this pipe -- the two differ only in one entry, in 1b.

  doca_flow_pipe_cfg_destroy(cfg);

  struct entry_batch_status install_status = {0};
  struct doca_flow_pipe_entry *entry;
  struct doca_flow_match entry_match = {0};
  struct doca_flow_fwd entry_fwd = {0};

  // TODO 1b -- add one entry per direction, then install both. In order:
  //   1. from the wire, on into your marking chain:
  //        entry_match.parser_meta.port_id = PF_PORT_ID;
  //        entry_fwd.type = DOCA_FLOW_FWD_PIPE;
  //        entry_fwd.next_pipe = wire_target;   <-- a PIPE, not a port. THIS is the one
  //                                                 line that differs from the no-op.
  //        doca_flow_pipe_basic_add_entry(PIPE_QUEUE, pipe, &entry_match, NULL, NULL, &entry_fwd,
  //                                 DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH, &install_status, &entry)
  //   2. back from the receiver SF, straight out of the uplink:
  //        entry_match.parser_meta.port_id = SF_REP_PORT_ID;
  //        memset(&entry_fwd, 0, sizeof(entry_fwd));   -- reused, so clear it first
  //        entry_fwd.type = DOCA_FLOW_FWD_PORT;
  //        entry_fwd.port_id = PF_PORT_ID;
  //        the same add-entry call, but with DOCA_FLOW_ENTRY_FLAGS_NO_WAIT
  //   3. doca_flow_entries_process(port, PIPE_QUEUE, ENTRY_PROCESS_TIMEOUT_US, nb_entries)
  //   4. fail unless install_status.failure is false and .nb_processed == nb_entries
  DOCA_LOG_INFO("Port demux ready");
}

// Build the PF0 pipeline, and report back the handles the rest of the program needs. This is the
// whole of the DOCA Flow work: everything before it is device and library setup, everything after
// it is the runtime loop.
//
// Which pipes exist depends on --percent: MARK only when it is above zero, and RANDOM_SAMPLE only
// when it is strictly between the two extremes (at 0 or 100 the wire feeds one forwarding pipe
// directly, with no sampling stage to pay for).
static void build_pipeline(struct doca_flow_port *port, const struct app_config *cfg,
                           struct pipeline *out) {
  // ---------------- NO-OP CONFIGURATION: comment out this line in Step 4.1. --------------------
  // As shipped this line IS the whole pipeline: one root pipe moving packets between the wire and
  // the receiver SF, so the program runs at line rate with nothing marked and nothing counted.
  // Until you uncomment the block below, the compiler reports the functions it would have called
  // as "defined but not used" -- expected, and how you know what is still unwired.
  create_root_pipe_nop(port);

  // ---------------- YOUR PIPELINE ---------------------------------------------------------------
  // Step 4.1: uncomment the two lines tagged [1] at the end.
  // Step 4.2 and Step 4.3: uncomment the rest of the block as well.
  //
  // struct doca_flow_pipe *wire_target = create_passthrough_pipe(port);  // [1]
  //
  // // PASS forwards and counts; MARK also rewrites the ECN bits to CE. Anything they do not
  // // match misses into PASSTHROUGH and reaches the SF anyway, so the match only decides what is
  // // counted and marked, never what gets through.
  // struct doca_flow_pipe *pass =
  //     create_forward_to_sf_pipe(port, false, wire_target, &out->pass_entry);
  // struct doca_flow_pipe *mark = NULL;
  // if (cfg->random_percent > 0.0)
  //   mark = create_forward_to_sf_pipe(port, true, wire_target, &out->ce_entry);
  //
  // // Where wire traffic actually enters, per --percent.
  // if (cfg->random_percent >= 100.0)
  //   // mark everything
  //   wire_target = mark;
  // else if (cfg->random_percent <= 0.0)
  //   // mark nothing
  //   wire_target = pass;
  // else {
  //   out->sample_mask = get_random_mask(cfg->random_percent);
  //   wire_target = create_sampling_pipe(port, mark, pass, out->sample_mask);
  // }
  //
  // create_root_pipe(port, wire_target);  // [1]
}

int main(int argc, char **argv) {
  struct app_config cfg = {.random_percent = 100.0};
  struct pipeline pl = {0};

  setup_logging();
  parse_args(argc, argv, &cfg);

  // Device and library bring-up. None of this is DOCA Flow pipeline work.
  struct doca_dev *dev = open_and_probe_dev(0);

  configure_and_start_dpdk_port(dev);
  initialize_doca_flow();
  struct doca_flow_port *port = port_start(dev);
  struct doca_dev_rep *sf_rep_dev = open_sf_representor(dev);
  struct doca_flow_port *sf_rep = rep_port_start(SF_REP_PORT_ID, sf_rep_dev);

  build_pipeline(port, &cfg, &pl);

  install_signal_handlers();
  log_startup(&cfg, &pl);
  run_report_loop(&pl);

  doca_flow_port_stop(sf_rep);
  doca_flow_port_stop(port);
  doca_flow_destroy();
  doca_argp_destroy();

  return EXIT_SUCCESS;
}
