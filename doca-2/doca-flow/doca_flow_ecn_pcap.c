#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_dpdk.h>
#include <doca_flow.h>
#include <doca_log.h>
#include <fcntl.h>
#include <pcap.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

DOCA_LOG_REGISTER(FLOW_ECN_PCAP);

#define NB_QUEUES 1
#define MIRROR_ID 1
#define RX_BURST 64
#define SNAPLEN 262144
#define RANDOM_FIELD_WIDTH 16

// The two DPDK ports this program forwards between, and why they hold these values.
//
// DPDK numbers ethdevs in the order they are probed, and open_and_probe_dev() probes exactly two:
// doca_dpdk_port_probe() brings up the PF uplink itself, and its devargs ask for one representor
// ("representor=sf0"), the receiver SF's. The PF is therefore always 0 and that representor always
// 1 — it is the probe string that decides this, not anything about the hardware, so changing the
// devargs is what would change these ids.
//
// Both directions of the eSwitch need one of them as a forwarding target: wire traffic is sent on
// to the SF, and what comes back from the SF is sent out of the uplink. find_pf_port_id() derives
// the PF's id independently at startup, by finding the one ethdev that is not a representor, so a
// mismatch there would surface rather than corrupt the pipeline silently.
#define PF_PORT_ID 0
#define SF_REP_PORT_ID 1

// The queue every entry in this program is installed on. DOCA lets rules be added from several
// threads at once, each on its own queue; this program installs all of its entries once, at
// startup, from the main thread, so there is only ever queue 0.
#define PIPE_QUEUE 0

// How long doca_flow_entries_process() may wait, in microseconds, for the hardware to confirm the
// entries handed to it. Installation is asynchronous — doca_flow_pipe_add_entry() returns as soon
// as the driver has taken the rule, and the verdict arrives later through entry_process_cb().
#define ENTRY_PROCESS_TIMEOUT_US 10000

// Descriptor ring sizes. RX is the capture path — every mirrored copy lands there and waits for
// the main loop to drain it — so it is deeper than TX, which this program never really uses.
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 512

// mbuf pool shared by both DPDK ports (PF uplink + SF representor), plus a per-core cache so the
// RX path does not hit the shared ring on every burst.
#define MBUF_POOL_SIZE 8192
#define MBUF_CACHE_SIZE 256

// Largest frame the capture path must be able to hold in a single mbuf. Sized for jumbo on
// purpose: an SF may be configured with any MTU, and this program cannot discover which.
#define CAPTURE_MAX_FRAME 9216

struct app_config {
  // --pcap: NULL => pure ECN-mark mode (no capture)
  const char *pcap_path;
  // --percent, [0,100], default 100
  double random_percent;
  // --sample N: write ~1-in-N captured packets to the pcap (default 1)
  uint32_t sample_n;
};

// What build_pipeline() hands back to the rest of the program: the two counter entries the
// once-a-second report queries, and the sampling mask the startup banner prints. Nothing else
// outside the pipeline needs to know which pipes exist or how they are chained.
struct pipeline {
  // MARK_CAPTURE's entry — counts CE-marked packets
  struct doca_flow_pipe_entry *ce_entry;
  // PASS_CAPTURE's entry — counts unmarked packets
  struct doca_flow_pipe_entry *pass_entry;
  // 0 unless --percent is strictly between 0 and 100
  uint16_t sample_mask;
};

// The pcap side of the capture path: the open file, and the running totals reported each second.
struct capture_sink {
  pcap_t *pd;
  pcap_dumper_t *dumper;
  // packets actually written to the pcap
  uint64_t written;
  // copies received from the hardware mirror
  uint64_t mirrored;
  // drives the ~1-in-N --sample decision
  uint64_t sample_ctr;
  // segmented frames seen (warned about once)
  uint64_t truncated;
};

static volatile bool g_running = true;
// starts OFF; SPACE or SIGUSR1 toggles it at runtime
static volatile bool g_capture_writing = false;
// Raised by the SIGUSR1 handler, serviced by the main loop. The flip logs, and DOCA_LOG_* is not
// async-signal-safe, so setting this flag is all the handler is allowed to do.
static volatile sig_atomic_t g_toggle_pending = 0;
// The terminal settings as we found them, so the SPACE key handling below can put them back.
static struct termios g_saved_termios;
static bool g_termios_saved = false;

static void signal_handler(int s) {
  if (s == SIGINT || s == SIGTERM) g_running = false;
}

static void toggle_signal_handler(int s) {
  (void)s;
  g_toggle_pending = 1;
}

// Put STDIN into unbuffered, non-blocking mode so a single keypress (SPACE) toggles capture.
// Returns false when there is no tty — piped, nohup'd, or driven over ssh by a script — in which
// case SIGUSR1 is the only way to toggle, and the caller has to say so rather than advertise a key
// that will never be read.
static bool enable_key_toggle(void) {
  struct termios t;
  if (tcgetattr(STDIN_FILENO, &t) != 0) return false;
  g_saved_termios = t;
  g_termios_saved = true;
  t.c_lflag &= ~(ICANON | ECHO);
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &t);
  int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (fl != -1) fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
  return true;
}
static void restore_key_toggle(void) {
  if (g_termios_saved) tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
}
static void flip_capture_writing(const char *how) {
  g_capture_writing = !g_capture_writing;
  DOCA_LOG_INFO("[toggle] pcap writing %s (via %s; HW mirror stays active)",
                g_capture_writing ? "ENABLED" : "PAUSED", how);
}
// Service both toggle paths: a SIGUSR1 that arrived since the last pass, then any pending
// keypresses — SPACE / 'c' / 'p' flip whether packets are written to the pcap.
static void poll_capture_toggle(void) {
  if (g_toggle_pending) {
    g_toggle_pending = 0;
    flip_capture_writing("SIGUSR1");
  }
  if (!g_termios_saved) return;
  char c;
  while (read(STDIN_FILENO, &c, 1) == 1) {
    if (c == ' ' || c == 'c' || c == 'p') flip_capture_writing("SPACE");
  }
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
// The two appended strings are `static char[]` rather than string literals because EAL parses argv
// with getopt, which permutes and writes to it; the storage has to be writable and outlive the
// call. The fixed 64-entry array is why argc is capped at 62 — room for the caller's arguments
// plus the two added here.
static doca_error_t initialize_dpdk(int argc, char **argv) {
  static char allow_flag[] = "-a";
  static char dummy_pci[] = "pci:00:00.0";
  char *nv[64];
  if (argc >= 62) {
    DOCA_LOG_ERR("Too many EAL arguments");
    return DOCA_ERROR_INVALID_VALUE;
  }
  for (int i = 0; i < argc; i++) nv[i] = argv[i];
  nv[argc] = allow_flag;
  nv[argc + 1] = dummy_pci;
  if (rte_eal_init(argc + 2, nv) < 0) {
    DOCA_LOG_ERR("EAL initialization failed");
    return DOCA_ERROR_DRIVER;
  }
  return DOCA_SUCCESS;
}

// Open the DOCA device at `index` (0 is the first, which is PF0) and probe it into DPDK.
//
// The probe string is an mlx5 PMD device-argument list, and every token in it is load-bearing:
//
//   dv_flow_en=2       Hardware steering (HWS). DOCA Flow's "switch,hws" mode needs it, and it is
//                      also the precondition for repr_matching_en below — the driver refuses with
//                      "Disabling representor matching is valid only when HW Steering is enabled".
//   fdb_def_rule_en=1  Keep mlx5's default FDB jump rule. Each PF owns a separate FDB domain and
//                      this program only ever programs PF0's; leaving the default in place is what
//                      keeps PF1 forwarding through its OVS bridge, which is how the sender SF's
//                      traffic reaches p1 and the wire. DOCA's own switch samples set this to 0
//                      because they own both sides — here it must stay 1.
//   repr_matching_en=0 Drop the PMD's implicit per-representor match. Traffic is then selected
//                      only by the rules this program writes, which is exactly what PORT_DEMUX
//                      does with parser_meta.port_meta. The driver puts the port into isolated
//                      mode as a result: "ingress traffic is restricted to defined flow rules".
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
  DOCA_CHECK("device",
             doca_dpdk_port_probe(
                 dev, "dv_flow_en=2,fdb_def_rule_en=1,repr_matching_en=0,representor=sf0"));
  return dev;
}

static void configure_and_start_dpdk_port(struct doca_dev *dev) {
  uint16_t first;
  DOCA_CHECK("dpdk port", doca_dpdk_get_first_port_id(dev, &first));
  // Size the mbufs for the largest frame that can arrive, not for the DPDK ports' MTU.
  //
  // The captured copies are the frames flowing between the SFs, and an SF may be configured with
  // any MTU up to jumbo. That MTU is not discoverable from here: the DPDK ports are the PF uplink
  // and the SF representor, which report the uplink's MTU, while the SF netdev that determines the
  // frame size lives in a separate network namespace and is not a DPDK port at all.
  //
  // A frame that does not fit the mbuf's data room is the dangerous case: the PMD still reports
  // its full length while the data never lands, so the pcap writer copies that many bytes out of a
  // smaller buffer and writes adjacent mbuf memory into the file. The result is a pcap of
  // same-sized all-zero frames that tcpdump renders as "[|llc]". Sizing for jumbo removes the
  // whole class of problem, at the cost of a larger mempool.
  uint16_t data_room = RTE_PKTMBUF_HEADROOM + CAPTURE_MAX_FRAME;
  DOCA_LOG_INFO("mbuf data room %u bytes (jumbo-capable, max frame %u)", data_room,
                CAPTURE_MAX_FRAME);

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
    uint16_t want_mtu = CAPTURE_MAX_FRAME - RTE_ETHER_HDR_LEN - RTE_ETHER_CRC_LEN;
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
  //   switch              Program the eSwitch (FDB) rather than a plain NIC ingress domain, so
  //                       pipes forward between eSwitch ports — the wire, the SF representor.
  //                       This program becomes PF0's forwarding plane while it runs.
  //   hws                 Hardware steering, the counterpart of dv_flow_en=2 in the probe string.
  //   isolated            Ingress traffic is never delivered to the port's CPU queues on its own;
  //                       it goes only where a flow rule sends it. DOCA's own flow_switch sample
  //                       uses "switch,hws,isolated".
  //   disable_switch_rss  Stop DOCA Flow building its *internal* FDB RSS suffix context at port
  //                       start. On BlueField firmware that permits RSS actions on ingress only,
  //                       creating it makes doca_flow_port_start fail outright with "RSS action
  //                       supported for ingress only". "isolated" alone does not suppress it —
  //                       verified with --sdk-log-level 60, which still showed DOCA building
  //                       "rss_htbl_port_0_0" until this token was added.
  //
  // Note that disable_switch_rss does NOT stop this program from using RSS itself:
  // create_to_cpu_pipe builds an explicit DOCA_FLOW_FWD_RSS pipe afterwards, and that is how
  // mirrored copies reach CPU queue 0 for the pcap. The token only suppresses the context DOCA
  // would have created on its own.
  //
  // The last two are undocumented in doca_flow.h but are real parsed tokens — they appear in
  // libdoca_flow.so alongside "expert" and "hairpinq_num".
  DOCA_CHECK("doca_flow init",
             doca_flow_cfg_set_mode_args(cfg, "switch,hws,isolated,disable_switch_rss"));
  DOCA_CHECK("doca_flow init", doca_flow_cfg_set_nr_counters(cfg, 4));
  DOCA_CHECK("doca_flow init", doca_flow_cfg_set_nr_shared_resource(
                                   cfg, MIRROR_ID + 1, DOCA_FLOW_SHARED_RESOURCE_MIRROR));
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
  char s[8];
  snprintf(s, sizeof(s), "%u", pid);
  DOCA_CHECK("pf port", doca_flow_port_cfg_set_devargs(cfg, s));
  struct doca_flow_port *port;
  DOCA_CHECK("pf port", doca_flow_port_start(cfg, &port));
  doca_flow_port_cfg_destroy(cfg);
  return port;
}

static struct doca_flow_port *rep_port_start(uint16_t pid) {
  struct doca_flow_port_cfg *cfg;
  char s[8];
  snprintf(s, sizeof(s), "%u", pid);
  DOCA_CHECK("rep port", doca_flow_port_cfg_create(&cfg));
  DOCA_CHECK("rep port", doca_flow_port_cfg_set_devargs(cfg, s));
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

static doca_error_t pcap_cb(void *p, void *c) {
  struct app_config *cfg = c;
  cfg->pcap_path = strdup((const char *)p);
  return cfg->pcap_path ? DOCA_SUCCESS : DOCA_ERROR_NO_MEMORY;
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
static doca_error_t sample_cb(void *p, void *c) {
  struct app_config *cfg = c;
  long v = atol((const char *)p);
  if (v < 1) {
    DOCA_LOG_ERR("--sample must be >= 1");
    return DOCA_ERROR_INVALID_VALUE;
  }
  cfg->sample_n = (uint32_t)v;
  return DOCA_SUCCESS;
}
static void register_params(void) {
  struct doca_argp_param *p;
  DOCA_CHECK("argp", doca_argp_param_create(&p));
  doca_argp_param_set_long_name(p, "pcap");
  doca_argp_param_set_description(
      p, "Output pcap file. Omit to run in pure ECN-mark mode (no capture, full goodput).");
  doca_argp_param_set_callback(p, pcap_cb);
  doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
  DOCA_CHECK("argp", doca_argp_register_param(p));
  DOCA_CHECK("argp", doca_argp_param_create(&p));
  doca_argp_param_set_long_name(p, "percent");
  doca_argp_param_set_description(
      p,
      "Percent of packets to CE-mark [0,100] (rounded down to a power-of-2 fraction; "
      "default 100). All packets are captured regardless.");
  doca_argp_param_set_callback(p, percent_cb);
  doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
  DOCA_CHECK("argp", doca_argp_register_param(p));
  DOCA_CHECK("argp", doca_argp_param_create(&p));
  doca_argp_param_set_long_name(p, "sample");
  doca_argp_param_set_description(p,
                                  "Write only ~1-in-N captured packets to the pcap (default 1 = "
                                  "every packet). Marking/forwarding are unaffected.");
  doca_argp_param_set_callback(p, sample_cb);
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
  DOCA_CHECK("argp", doca_argp_init("doca_flow_ecn_pcap", cfg));
  doca_argp_set_dpdk_program(initialize_dpdk);
  register_params();
  DOCA_CHECK("argp", doca_argp_start(argc, argv));
}

// No --pcap means pure ECN-mark mode, so there is nothing to open. Mirrors close_capture_pcap,
// which decides the same thing from sink->dumper — both are safe to call unconditionally.
static void open_capture_pcap(const char *path, struct capture_sink *sink) {
  if (!path) return;
  sink->pd = pcap_open_dead(DLT_EN10MB, SNAPLEN);
  if (!sink->pd) {
    DOCA_LOG_CRIT("pcap_open_dead");
    exit(EXIT_FAILURE);
  }
  sink->dumper = pcap_dump_open(sink->pd, path);
  if (!sink->dumper) {
    DOCA_LOG_CRIT("pcap_dump_open('%s'): %s", path, pcap_geterr(sink->pd));
    exit(EXIT_FAILURE);
  }
}

static void close_capture_pcap(const char *path, struct capture_sink *sink) {
  if (!sink->dumper) return;
  pcap_dump_flush(sink->dumper);
  pcap_dump_close(sink->dumper);
  pcap_close(sink->pd);
  DOCA_LOG_INFO("Wrote %lu packets to %s", sink->written, path);
}

static void install_signal_handlers(bool capture) {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  if (capture) signal(SIGUSR1, toggle_signal_handler);
}

// The banner, and — when capturing — the line that tells the operator how to start the pcap.
// enable_key_toggle() is called from here because what it returns decides what that line says.
static void log_startup(const struct app_config *cfg, const struct pipeline *pl) {
  const char *dst = cfg->pcap_path ? cfg->pcap_path : "(none — pure ECN-mark mode)";
  if (cfg->random_percent >= 100.0)
    DOCA_LOG_INFO("Marking ALL IPv4 | capture: %s — Ctrl-C to stop", dst);
  else if (cfg->random_percent <= 0.0)
    DOCA_LOG_INFO("Marking NONE     | capture: %s — Ctrl-C to stop", dst);
  else
    DOCA_LOG_INFO("Marking ~%.4g%%   | capture: %s — Ctrl-C to stop", 100.0 / (pl->sample_mask + 1),
                  dst);
  if (!cfg->pcap_path) return;
  if (cfg->sample_n > 1) DOCA_LOG_INFO("Capturing ~1-in-%u packets to the pcap", cfg->sample_n);
  if (enable_key_toggle())
    DOCA_LOG_INFO(
        "pcap writing starts PAUSED — press SPACE (or 'c'/'p'), or `kill -USR1 %d`, to start/stop "
        "writing to '%s'",
        (int)getpid(), cfg->pcap_path);
  else
    DOCA_LOG_INFO(
        "pcap writing starts PAUSED — no tty, so SPACE cannot be read: `kill -USR1 %d` to "
        "start/stop writing to '%s'",
        (int)getpid(), cfg->pcap_path);
}

// Drain the mirrored copies off CPU queue 0 into the pcap, and report the counters once a second.
// Runs until SIGINT/SIGTERM. In pure ECN-mark mode there is nothing to drain, so it only reports.
static void run_capture_loop(uint16_t pf0, const struct app_config *cfg, const struct pipeline *pl,
                             struct capture_sink *sink) {
  bool capture = (cfg->pcap_path != NULL);
  struct rte_mbuf *bufs[RX_BURST];
  time_t last = time(NULL);
  while (g_running) {
    uint16_t nb = 0;
    if (capture) {
      nb = rte_eth_rx_burst(pf0, 0, bufs, RX_BURST);
      for (uint16_t i = 0; i < nb; i++) {
        sink->mirrored++;
        // ~1-in-N (N==1 => every packet)
        bool take = (++sink->sample_ctr % cfg->sample_n == 0);
        if (g_capture_writing && take) {
          struct pcap_pkthdr h;
          struct timeval tv;
          gettimeofday(&tv, NULL);
          h.ts = tv;
          // Only ever write what this segment really holds. data_len is the first segment; if the
          // frame were ever chained across mbufs, pkt_len would exceed it and copying pkt_len
          // bytes would run off the end of the buffer.
          h.caplen = rte_pktmbuf_data_len(bufs[i]);
          h.len = rte_pktmbuf_pkt_len(bufs[i]);
          if (h.caplen > h.len) h.caplen = h.len;
          if (h.len > h.caplen && sink->truncated++ == 0)
            DOCA_LOG_WARN(
                "captured frame is segmented (%u of %u bytes) — pcap entries will be truncated",
                h.caplen, h.len);
          pcap_dump((u_char *)sink->dumper, &h, rte_pktmbuf_mtod(bufs[i], const u_char *));
          // Flush every record, the way `tcpdump -U` does. pcap_dump goes through stdio's 4 KB
          // buffer, so without this a partial record sits on disk between flushes — and a frame
          // larger than that buffer leaves one there essentially always. Anything following the
          // file (check_ecn_bits_from_pcap.sh, tcpdump -r) then hits a truncated record and stops
          // after the first packet. Flushing per record keeps the file readable while it grows.
          pcap_dump_flush(sink->dumper);
          sink->written++;
        }
        rte_pktmbuf_free(bufs[i]);
      }
    }
    if (nb == 0) usleep(200);
    poll_capture_toggle();
    time_t now = time(NULL);
    if (now != last) {
      last = now;
      if (capture) pcap_dump_flush(sink->dumper);
      uint64_t ce = query_pkts(pl->ce_entry), pass = query_pkts(pl->pass_entry), tot = ce + pass;
      if (capture)
        DOCA_LOG_INFO(
            "CE marked: %lu, passthrough: %lu (%.4g%% marked) | mirrored: %lu -> pcap: %lu%s", ce,
            pass, tot ? 100.0 * (double)ce / (double)tot : 0.0, sink->mirrored, sink->written,
            g_capture_writing ? "" : " [PAUSED]");
      else
        DOCA_LOG_INFO("CE marked: %lu, passthrough: %lu (%.4g%% marked)", ce, pass,
                      tot ? 100.0 * (double)ce / (double)tot : 0.0);
    }
  }
}

// ================================================================================
// The DOCA Flow pipeline.
//
// Everything above this point is setup and runtime plumbing. Everything from here
// down to build_pipeline() is the eSwitch pipeline itself.
// ================================================================================

// PASSTHROUGH — the fallback forward, and one of the two worked examples for the exercise.
//
// Matches IPv4 (the DSCP/ECN byte is declared but wildcarded) and forwards to port 1, the receiver
// SF. No counter, no CE marking, no mirror: it moves the packet and does nothing else, which makes
// it the smallest complete instance of the five-part shape every pipe in this file follows.
//
// build_pipeline() hands it to both forwarding pipes as their miss target, so whatever they do not
// match still reaches the receiver rather than being dropped.
static struct doca_flow_pipe *create_passthrough_pipe(struct doca_flow_port *port) {
  struct doca_flow_pipe_cfg *cfg;

  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_cfg_create(&cfg, port));
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_cfg_set_name(cfg, "PASSTHROUGH"));
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC));
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT));
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_cfg_set_is_root(cfg, false));

  const uint32_t nb_entries = 1;
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_cfg_set_nr_entries(cfg, nb_entries));

  struct doca_flow_match match = {0}, match_mask = {0};
  match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  match.outer.ip4.dscp_ecn = 0xFF;
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask));

  struct doca_flow_fwd fwd_hit = {.type = DOCA_FLOW_FWD_PORT, .port_id = SF_REP_PORT_ID};
  struct doca_flow_pipe *pipe;
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_create(cfg, &fwd_hit, NULL, &pipe));

  doca_flow_pipe_cfg_destroy(cfg);

  // `match` is reused as this entry's values, so drop the template's 0xFF placeholder.
  match.outer.ip4.dscp_ecn = 0x00;

  struct entry_batch_status install_status = {0};
  struct doca_flow_pipe_entry *entry;
  DOCA_CHECK("PASSTHROUGH", doca_flow_pipe_add_entry(PIPE_QUEUE, pipe, &match, NULL, NULL, NULL,
                                                     DOCA_FLOW_NO_WAIT, &install_status, &entry));

  DOCA_CHECK("PASSTHROUGH",
             doca_flow_entries_process(port, PIPE_QUEUE, ENTRY_PROCESS_TIMEOUT_US, nb_entries));
  doca_check((install_status.failure || install_status.nb_processed != nb_entries)
                 ? DOCA_ERROR_BAD_STATE
                 : DOCA_SUCCESS,
             "PASSTHROUGH: install");
  return pipe;
}

// TO_CPU — the only exit from the eSwitch into this process.
//
// Matches IPv4 and forwards with DOCA_FLOW_FWD_RSS to CPU RX queue 0, where run_capture_loop()
// collects packets with rte_eth_rx_burst() and writes them to the pcap. RSS ("receive side
// scaling") normally means hashing traffic across many CPU queues; with num_of_queues = 1 there is
// nothing to scale and it simply means "deliver to the CPU".
//
// No pipe forwards here. This one is reached only through the shared mirror that
// bind_capture_mirror() aims at it, which is also why it is not a root pipe.
static struct doca_flow_pipe *create_to_cpu_pipe(struct doca_flow_port *port) {
  struct doca_flow_pipe_cfg *cfg;

  DOCA_CHECK("TO_CPU", doca_flow_pipe_cfg_create(&cfg, port));
  DOCA_CHECK("TO_CPU", doca_flow_pipe_cfg_set_name(cfg, "TO_CPU"));
  DOCA_CHECK("TO_CPU", doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC));
  DOCA_CHECK("TO_CPU", doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT));
  DOCA_CHECK("TO_CPU", doca_flow_pipe_cfg_set_is_root(cfg, false));

  const uint32_t nb_entries = 1;
  DOCA_CHECK("TO_CPU", doca_flow_pipe_cfg_set_nr_entries(cfg, nb_entries));

  struct doca_flow_match match = {0};
  match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
  DOCA_CHECK("TO_CPU", doca_flow_pipe_cfg_set_match(cfg, &match, NULL));

  static uint16_t rss_queues[1] = {0};
  struct doca_flow_fwd fwd_hit = {0};
  fwd_hit.type = DOCA_FLOW_FWD_RSS;
  fwd_hit.rss_queues = rss_queues;
  fwd_hit.num_of_queues = 1;
  fwd_hit.rss_outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
  struct doca_flow_pipe *pipe;
  DOCA_CHECK("TO_CPU", doca_flow_pipe_create(cfg, &fwd_hit, NULL, &pipe));

  doca_flow_pipe_cfg_destroy(cfg);

  struct entry_batch_status install_status = {0};
  struct doca_flow_pipe_entry *entry;
  DOCA_CHECK("TO_CPU", doca_flow_pipe_add_entry(PIPE_QUEUE, pipe, &match, NULL, NULL, NULL,
                                                DOCA_FLOW_NO_WAIT, &install_status, &entry));

  DOCA_CHECK("TO_CPU",
             doca_flow_entries_process(port, PIPE_QUEUE, ENTRY_PROCESS_TIMEOUT_US, nb_entries));
  doca_check((install_status.failure || install_status.nb_processed != nb_entries)
                 ? DOCA_ERROR_BAD_STATE
                 : DOCA_SUCCESS,
             "TO_CPU: install");
  DOCA_LOG_INFO("TO_CPU pipe ready -> CPU queue 0");
  return pipe;
}

// The shared mirror — what duplicates a packet towards the pcap. Note that this creates no pipe.
//
// A mirror is a port-level SHARED resource rather than pipe state: its slot is reserved back in
// initialize_doca_flow() (set_nr_shared_resource), it is configured and bound to the port here, and
// then any pipe that wants a copy references it by id. Both PASS_CAPTURE and MARK_CAPTURE point at
// this single MIRROR_ID via monitor.shared_mirror_id — which is precisely why it cannot be folded
// into one pipe's creation.
//
// The two halves are easy to mix up: target.fwd decides where the COPY goes (into cpu_pipe, and so
// to the pcap), while DOCA_TUT_MIRROR_SET_ORIG_FWD below decides where the ORIGINAL carries on.
static void bind_capture_mirror(struct doca_flow_port *port, struct doca_flow_pipe *cpu_pipe) {
  // Where the COPY goes: into the TO_CPU pipe, and from there to the pcap.
  struct doca_flow_mirror_target target = {0};
  target.fwd.type = DOCA_FLOW_FWD_PIPE;
  target.fwd.next_pipe = cpu_pipe;

  struct doca_flow_resource_mirror_cfg mirror_cfg = {0};
  mirror_cfg.nr_targets = 1;
  mirror_cfg.target = &target;
  // Where the ORIGINAL packet goes after a copy is mirrored. DOCA 2.7 requires this; 2.9 refuses
  // to bind the mirror if it is set. See doca_flow_compat.h for the measurements.
  DOCA_TUT_MIRROR_SET_ORIG_FWD(mirror_cfg, SF_REP_PORT_ID);

  struct doca_flow_shared_resource_cfg shared_cfg = {0};
  shared_cfg.mirror_cfg = mirror_cfg;
  DOCA_CHECK("MIRROR", doca_flow_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_MIRROR,
                                                         MIRROR_ID, &shared_cfg));

  // Configuring the id is not enough on its own: binding is what attaches it to this port, and
  // only then can a pipe on that port reference MIRROR_ID from its monitor.
  uint32_t mirror_ids[1] = {MIRROR_ID};
  DOCA_CHECK("MIRROR", doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_MIRROR, mirror_ids,
                                                       1, port));
  DOCA_LOG_INFO("Shared mirror %u -> TO_CPU pipe -> queue 0", MIRROR_ID);
}

// The main forwarding pipe: wire IPv4 -> the receiver SF. build_pipeline() builds it TWICE:
//
//   PASS_CAPTURE   mark=false   forward the packet unchanged
//   MARK_CAPTURE   mark=true    forward it, setting ECN CE on the way through
//
// Whichever instance, the constant behaviour is the same: match IPv4 regardless of the ECN bits it
// arrived with, count it, and forward to port 1. Anything that does not match goes to miss_pipe.
// The counter is not incidental — it is what the once-a-second "CE marked: / passthrough:" report
// queries, and without it there is no way to see whether the pipeline is doing anything.
//
// Two independent options ride on top. `mirror` attaches the shared mirror so a copy also reaches
// the pcap; `mark` attaches the action that rewrites dscp_ecn to CE. Neither is what the pipe is
// FOR — forwarding is — which is why the ECN part of the exercise is only the action.
//
// out_entry hands the installed entry back so the report can query its counter.
static struct doca_flow_pipe *create_forward_to_sf_pipe(struct doca_flow_port *port, bool mark,
                                                        bool mirror,
                                                        struct doca_flow_pipe *miss_pipe,
                                                        struct doca_flow_pipe_entry **out_entry) {
  const char *name = mark ? "MARK_CAPTURE" : "PASS_CAPTURE";
  struct doca_flow_pipe_cfg *cfg;

  DOCA_CHECK(name, doca_flow_pipe_cfg_create(&cfg, port));
  DOCA_CHECK(name, doca_flow_pipe_cfg_set_name(cfg, name));
  DOCA_CHECK(name, doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC));

  // Default pipe domain for actions on the ingress traffic.
  DOCA_CHECK(name, doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT));

  // Not the root pipe (i.e. not the first pipe to see the packet). The root pipe is PORT_DEMUX,
  // which is built in build_pipeline() and is the only pipe with is_root=true.
  DOCA_CHECK(name, doca_flow_pipe_cfg_set_is_root(cfg, false));

  // One entry: the match template. The actual entry is added below with doca_flow_pipe_add_entry().
  const uint32_t nb_entries = 1;
  DOCA_CHECK(name, doca_flow_pipe_cfg_set_nr_entries(cfg, nb_entries));

  // A field set in the template but zeroed in the mask is declared without being compared, which is
  // how dscp_ecn is treated here: MARK_CAPTURE has to catch packets that arrive already CE-marked
  // as readily as fresh ones. l3_type has no mask entry, so it is compared exactly.
  struct doca_flow_match match = {0}, match_mask = {0};
  match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  match.outer.ip4.dscp_ecn = 0xFF;
  match_mask.outer.ip4.dscp_ecn = 0x00;
  DOCA_CHECK(name, doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask));

  // 0xFF is the action template ("entries may write this field"); the per-entry value follows below
  struct doca_flow_actions action_template = {0}, *action_templates[1] = {&action_template};
  if (mark) {
    action_template.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    action_template.outer.ip4.dscp_ecn = 0xFF;
    DOCA_CHECK(name, doca_flow_pipe_cfg_set_actions(cfg, action_templates, NULL, NULL, 1));
  }

  // The counter is what query_pkts() reads for the once-a-second report; the mirror id, when
  // capturing, is what sends a copy of every hit towards the pcap.
  struct doca_flow_monitor monitor = {0};
  monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
  if (mirror) monitor.shared_mirror_id = MIRROR_ID;
  DOCA_CHECK(name, doca_flow_pipe_cfg_set_monitor(cfg, &monitor));

  // Finally create the pipe itself.
  // We will later add the single entry to it, which is what actually makes it do anything.
  struct doca_flow_fwd fwd_hit = {.type = DOCA_FLOW_FWD_PORT, .port_id = SF_REP_PORT_ID};
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = miss_pipe};
  struct doca_flow_pipe *pipe;
  DOCA_CHECK(name, doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe));

  // pipe_create() has read the whole cfg and the pipe keeps no reference to it.
  doca_flow_pipe_cfg_destroy(cfg);

  // What the template above allowed to be written, written: 0x03 is both ECN bits set, CE
  // ("Congestion Experienced"), the mark the PCC exercise in Part IV reacts to. action_idx picks
  // which of the cfg's action templates this fills in — there is only one, at index 0.
  struct doca_flow_actions entry_actions = {0};
  if (mark) {
    entry_actions.action_idx = 0;
    entry_actions.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    entry_actions.outer.ip4.dscp_ecn = 0x03;
  }

  // `match` is reused below as this entry's values, so drop the template's 0xFF placeholder.
  match.outer.ip4.dscp_ecn = 0x00;

  // The NULL is this entry's own forward: it has none, so it inherits the pipe's. install_status is
  // an opaque context, handed straight back to entry_process_cb().
  struct entry_batch_status install_status = {0};
  DOCA_CHECK(name, doca_flow_pipe_add_entry(PIPE_QUEUE, pipe, &match, mark ? &entry_actions : NULL,
                                            &monitor, NULL, DOCA_FLOW_NO_WAIT, &install_status,
                                            out_entry));

  // add_entry() only queued the work; this drives it to completion.
  DOCA_CHECK(name,
             doca_flow_entries_process(port, PIPE_QUEUE, ENTRY_PROCESS_TIMEOUT_US, nb_entries));

  // Success above means entries_process() ran, not that the hardware accepted anything — that
  // verdict arrives through the callback. A pipe that exists but installed nothing forwards
  // nothing, silently, which is the hardest failure here to spot from the outside.
  doca_check((install_status.failure || install_status.nb_processed != nb_entries)
                 ? DOCA_ERROR_BAD_STATE
                 : DOCA_SUCCESS,
             "%s install", name);
  DOCA_LOG_INFO("%s pipe ready (%s + capture)", name, mark ? "CE-mark" : "no-mark");

  return pipe;
}

// RANDOM_SAMPLE — splits wire traffic between the marking and non-marking paths, in hardware.
//
// Only built when --percent is strictly between 0 and 100. At either extreme build_pipeline() aims
// the wire straight at one forwarding pipe and skips this stage altogether.
//
// The parser stamps every packet with a random 16-bit value in parser_meta.random. Matching that
// field against 0 under `mask` — a power of two minus one — therefore hits for 1 packet in
// (mask + 1). Hits go to `hit` (MARK_CAPTURE), misses to `miss` (PASS_CAPTURE). Both of those
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
  match.parser_meta.random = 0;
  match_mask.parser_meta.random = mask;
  DOCA_CHECK("RANDOM_SAMPLE", doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask));

  struct doca_flow_fwd fwd_hit = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = hit};
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = miss};
  struct doca_flow_pipe *pipe;
  DOCA_CHECK("RANDOM_SAMPLE", doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe));

  doca_flow_pipe_cfg_destroy(cfg);

  // The entry adds nothing to the template: no actions, no counter, and no forward of its own, so
  // both outcomes are decided by the pipe's own two forwards.
  struct entry_batch_status install_status = {0};
  struct doca_flow_pipe_entry *entry;
  DOCA_CHECK("RANDOM_SAMPLE", doca_flow_pipe_add_entry(PIPE_QUEUE, pipe, &match, NULL, NULL, NULL,
                                                       DOCA_FLOW_NO_WAIT, &install_status, &entry));

  DOCA_CHECK("RANDOM_SAMPLE",
             doca_flow_entries_process(port, PIPE_QUEUE, ENTRY_PROCESS_TIMEOUT_US, nb_entries));
  doca_check((install_status.failure || install_status.nb_processed != nb_entries)
                 ? DOCA_ERROR_BAD_STATE
                 : DOCA_SUCCESS,
             "RANDOM_SAMPLE: install");
  DOCA_LOG_INFO("Random-sample pipe ready: mask 0x%04x", mask);
  return pipe;
}

// PORT_DEMUX — the root pipe. Every packet entering PF0's eSwitch is looked up here first, and it
// is the only pipe in this file with is_root set. Until it exists, none of the others are
// reachable however correct they are.
//
// It sorts by direction, matching parser_meta.port_meta, the port a packet arrived on:
//
//   port_meta 0   from the wire (p0)     -> wire_target, the head of the marking chain
//   port_meta 1   from the receiver SF   -> straight out port 0, back onto the wire
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
  match.parser_meta.port_meta = UINT32_MAX;
  match_mask.parser_meta.port_meta = UINT32_MAX;
  DOCA_CHECK("PORT_DEMUX", doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask));

  struct doca_flow_fwd fwd_hit = {.type = DOCA_FLOW_FWD_CHANGEABLE};
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_DROP};
  struct doca_flow_pipe *pipe;
  DOCA_CHECK("PORT_DEMUX", doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe));

  doca_flow_pipe_cfg_destroy(cfg);

  struct entry_batch_status install_status = {0};
  struct doca_flow_pipe_entry *entry;
  struct doca_flow_match entry_match = {0};
  struct doca_flow_fwd entry_fwd = {0};

  // From the wire: on to the head of the marking chain. WAIT_FOR_BATCH holds this entry back so it
  // reaches the hardware together with the one below.
  entry_match.parser_meta.port_meta = PF_PORT_ID;
  entry_fwd.type = DOCA_FLOW_FWD_PIPE;
  entry_fwd.next_pipe = wire_target;
  DOCA_CHECK("PORT_DEMUX",
             doca_flow_pipe_add_entry(PIPE_QUEUE, pipe, &entry_match, NULL, NULL, &entry_fwd,
                                      DOCA_FLOW_WAIT_FOR_BATCH, &install_status, &entry));

  // From the receiver SF: straight back out of the uplink, untouched.
  entry_match.parser_meta.port_meta = SF_REP_PORT_ID;
  memset(&entry_fwd, 0, sizeof(entry_fwd));
  entry_fwd.type = DOCA_FLOW_FWD_PORT;
  entry_fwd.port_id = PF_PORT_ID;
  DOCA_CHECK("PORT_DEMUX",
             doca_flow_pipe_add_entry(PIPE_QUEUE, pipe, &entry_match, NULL, NULL, &entry_fwd,
                                      DOCA_FLOW_NO_WAIT, &install_status, &entry));

  DOCA_CHECK("PORT_DEMUX",
             doca_flow_entries_process(port, PIPE_QUEUE, ENTRY_PROCESS_TIMEOUT_US, nb_entries));
  doca_check((install_status.failure || install_status.nb_processed != nb_entries)
                 ? DOCA_ERROR_BAD_STATE
                 : DOCA_SUCCESS,
             "PORT_DEMUX: install");
  DOCA_LOG_INFO("Port demux ready");
}

// Build the PF0 pipeline, and report back the handles the rest of the program needs. This is the
// whole of the DOCA Flow work: everything before it is device and library setup, everything after
// it is the runtime loop.
//
// Which pipes exist depends on the options: the RSS pipe and the mirror only when --pcap asked for
// a capture, MARK_CAPTURE only when --percent is above zero, and RANDOM_SAMPLE only when --percent
// is strictly between the two extremes (at 0 or 100 the wire feeds one capture pipe directly, with
// no sampling stage to pay for).
static void build_pipeline(struct doca_flow_port *port, const struct app_config *cfg,
                           struct pipeline *out) {
  bool capture = (cfg->pcap_path != NULL);

  if (capture) {
    struct doca_flow_pipe *cpu = create_to_cpu_pipe(port);
    bind_capture_mirror(port, cpu);
  }
  struct doca_flow_pipe *passthrough = create_passthrough_pipe(port);

  // PASS_CAPTURE (no mark) and MARK_CAPTURE (CE-mark); both mirror to pcap only when capturing.
  struct doca_flow_pipe *pass_cap =
      create_forward_to_sf_pipe(port, false, capture, passthrough, &out->pass_entry);
  struct doca_flow_pipe *mark_cap = NULL;
  if (cfg->random_percent > 0.0)
    mark_cap = create_forward_to_sf_pipe(port, true, capture, passthrough, &out->ce_entry);

  // wire-ingress entry point per --percent
  struct doca_flow_pipe *wire_target;
  if (cfg->random_percent >= 100.0)
    // mark+capture all
    wire_target = mark_cap;
  else if (cfg->random_percent <= 0.0)
    // capture all, mark none
    wire_target = pass_cap;
  else {
    out->sample_mask = get_random_mask(cfg->random_percent);
    wire_target = create_sampling_pipe(port, mark_cap, pass_cap, out->sample_mask);
  }

  create_root_pipe(port, wire_target);
}

int main(int argc, char **argv) {
  struct app_config cfg = {.pcap_path = NULL, .random_percent = 100.0, .sample_n = 1};
  struct capture_sink sink = {0};
  struct pipeline pl = {0};

  setup_logging();
  parse_args(argc, argv, &cfg);
  open_capture_pcap(cfg.pcap_path, &sink);

  // Device and library bring-up. None of this is DOCA Flow pipeline work.
  struct doca_dev *dev = open_and_probe_dev(0);

  configure_and_start_dpdk_port(dev);
  initialize_doca_flow();
  struct doca_flow_port *port = port_start(dev);
  struct doca_flow_port *sf_rep = rep_port_start(SF_REP_PORT_ID);

  build_pipeline(port, &cfg, &pl);

  install_signal_handlers(cfg.pcap_path != NULL);
  log_startup(&cfg, &pl);
  run_capture_loop(find_pf_port_id(), &cfg, &pl, &sink);

  restore_key_toggle();
  close_capture_pcap(cfg.pcap_path, &sink);
  doca_flow_port_stop(sf_rep);
  doca_flow_port_stop(port);
  doca_flow_destroy();
  doca_argp_destroy();

  return EXIT_SUCCESS;
}
