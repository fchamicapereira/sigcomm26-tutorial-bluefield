/*
 * doca_flow_ecn_pcap — ECN-mark AND capture-to-pcap in ONE PF0 program.
 *
 * Combines doca_flow_ecn (set CE on ~--percent of wire packets) with a copy-to-CPU capture path,
 * so you don't have to fight over PF0 with two separate programs.
 *
 * Topology (PF0 switch/FDB):
 *   PORT_DEMUX (root, by source port):
 *     p0 wire ingress  -> [RANDOM_SAMPLE] -> MARK_CAPTURE (set CE, then FLOOD)
 *                                          \-> PASS_CAPTURE (no mark, then FLOOD)
 *     mlx5_2 SF egress -> p0 wire (return path for ACKs/CNPs)
 *
 *   FLOOD (hash pipe, flooding algorithm) duplicates each packet to BOTH of its entries:
 *     entry 0 -> PASSTHROUGH -> mlx5_2   (the production path)
 *     entry 1 -> MIRROR_RSS  -> CPU q0   (the copy this program writes to <pcap>)
 *
 * HOW THIS DIFFERS FROM THE DOCA 2.x VERSION
 * ------------------------------------------
 * DOCA Flow 3.2 removed the shared mirror resource: DOCA_FLOW_SHARED_RESOURCE_MIRROR,
 * struct doca_flow_resource_mirror_cfg and doca_flow_mirror_target are all gone from 3.4 (no DOCA
 * header mentions "mirror", and libdoca_flow.so exports no mirror symbol). The doca-2 build gets
 * its copy by attaching monitor.shared_mirror_id to the marking entry; that mechanism no longer
 * exists here.
 *
 * The documented replacement is a DOCA_FLOW_PIPE_HASH pipe running
 * DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING: flooding delivers the packet to EVERY entry in the
 * pipe, so entry 0 carries the real destination and entries 1..N are the copies. Two constraints
 * shape the code below:
 *   - packet order is only guaranteed for the destination of the FIRST entry, so entry 0 is the
 *     production path (to the SF) and the pcap copy is entry 1;
 *   - a hash pipe's entry count must be a power of two (2 here), and flooding tops out at 254.
 * Both entries forward with the same fwd TYPE (FWD_PIPE), which is why entry 0 goes through the
 * PASSTHROUGH pipe rather than straight to FWD_PORT — the pipe is created from a single fwd
 * template and mixing types across its entries is not expressible.
 *
 * Everything else is the DOCA 3.4 API shift already made in doca_flow_nop.c: port_cfg_set_port_id
 * instead of set_devargs, a doca_dev_rep for the SF representor, an actions-memory pool, and
 * doca_flow_pipe_basic_add_entry with its explicit action_idx and DOCA_FLOW_ENTRY_FLAGS_* flags.
 *
 * Same environment as doca_flow_nop — see admin/local_scripts/setup_roce_loopback.sh / README.md.
 */
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
#define RX_BURST 64
#define SNAPLEN 262144
#define RANDOM_FIELD_WIDTH 16

/* Largest frame the capture path must be able to hold in a single mbuf. Sized for jumbo on
 * purpose: an SF may be configured with any MTU, and this program cannot discover which. */
#define CAPTURE_MAX_FRAME 9216

/* Flooding duplicates the packet to every entry; two is all we need (production + pcap copy) and
 * a hash pipe's entry count must be a power of two. */
#define FLOOD_NB_ENTRIES 2
#define FLOOD_ENTRY_PRODUCTION 0 /* order is only guaranteed for entry 0 — keep the real path here */
#define FLOOD_ENTRY_CAPTURE 1

/* DOCA 3.4 HWS needs a per-port action-memory pool for any pipe carrying a modify action (the CE
 * mark). Sized via DOCA's own formula next_pow2(entries * 128 + 1024); 16 KB is ample here. */
#define ACTIONS_MEM_SIZE (16 * 1024)

struct app_config {
  const char *pcap_path; /* --pcap: NULL => pure ECN-mark mode (no capture) */
  double random_percent; /* --percent, [0,100], default 100 */
  uint32_t sample_n;     /* --sample N: write ~1-in-N captured packets to the pcap (default 1) */
};

static volatile bool g_running = true;
static volatile bool g_capture_writing = false; /* starts OFF; SPACE or SIGUSR1 toggles it at runtime */
/* Raised by the SIGUSR1 handler, serviced by the main loop. The flip logs, and DOCA_LOG_* is not
 * async-signal-safe, so setting this flag is all the handler is allowed to do. */
static volatile sig_atomic_t g_toggle_pending = 0;
static struct termios g_saved_termios;
static bool g_termios_saved = false;

static void signal_handler(int s) {
  if (s == SIGINT || s == SIGTERM) g_running = false;
}

static void toggle_signal_handler(int s) {
  (void)s;
  g_toggle_pending = 1;
}

/* Put STDIN into unbuffered, non-blocking mode so a single keypress (SPACE) toggles capture.
 * Returns false when there is no tty — piped, nohup'd, or driven over ssh by a script — in which
 * case SIGUSR1 is the only way to toggle, and the caller has to say so rather than advertise a key
 * that will never be read. */
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
  DOCA_LOG_INFO("[toggle] pcap writing %s (via %s; HW flooding stays active)", g_capture_writing ? "ENABLED" : "PAUSED", how);
}
/* Service both toggle paths: a SIGUSR1 that arrived since the last pass, then any pending
 * keypresses — SPACE / 'c' / 'p' flip whether packets are written to the pcap. */
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

static __attribute__((format(printf, 2, 3))) void crash_if_unsuccessful(doca_error_t err, const char *fmt, ...) {
  if (err == DOCA_SUCCESS) return;
  char msg[512];
  va_list a;
  va_start(a, fmt);
  vsnprintf(msg, sizeof(msg), fmt, a);
  va_end(a);
  DOCA_LOG_CRIT("%s: %s", msg, doca_error_get_descr(err));
  exit(EXIT_FAILURE);
}

/* percent -> nearest power-of-two random mask (same technique as doca_flow_ecn). */
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
static void entry_process_cb(struct doca_flow_pipe_entry *e, uint16_t q, enum doca_flow_entry_status st, enum doca_flow_entry_op op,
                             void *ctx) {
  (void)e;
  (void)q;
  (void)op;
  struct entry_batch_status *s = ctx;
  if (!s) return;
  if (st != DOCA_FLOW_ENTRY_STATUS_SUCCESS) s->failure = true;
  s->nb_processed++;
}

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
  /* Two dummy allowlist entries so EAL auto-probes nothing on either bus. The "auxiliary:" one is
   * essential once setup_roce_loopback.sh has moved the SFs into their own netns: without it EAL
   * still scans the auxiliary bus and tries to probe those SFs, whose verbs now live in ns0/ns1. */
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

static struct doca_dev *open_and_probe_dev(uint32_t index, const char *probe_args) {
  struct doca_devinfo **list;
  uint32_t n;
  struct doca_dev *dev;
  doca_error_t err;
  err = doca_devinfo_create_list(&list, &n);
  crash_if_unsuccessful(err, "doca_devinfo_create_list");
  if (index >= n) {
    DOCA_LOG_CRIT("Device index %u out of range (%u)", index, n);
    exit(EXIT_FAILURE);
  }
  err = doca_dev_open(list[index], &dev);
  crash_if_unsuccessful(err, "doca_dev_open");
  doca_devinfo_destroy_list(list);
  err = doca_dpdk_port_probe(dev, probe_args);
  crash_if_unsuccessful(err, "doca_dpdk_port_probe");
  return dev;
}

static void configure_and_start_dpdk_port(struct doca_dev *dev) {
  uint16_t first;
  doca_error_t err = doca_dpdk_get_first_port_id(dev, &first);
  crash_if_unsuccessful(err, "doca_dpdk_get_first_port_id");

  /* Size the mbufs for the largest frame that can arrive, not for the DPDK ports' MTU.
   *
   * The captured copies are the frames flowing between the SFs, and an SF may be configured with
   * any MTU up to jumbo. That MTU is not discoverable from here: the DPDK ports are the PF uplink
   * and the SF representor, which report the uplink's MTU, while the SF netdev that determines the
   * frame size lives in a separate network namespace and is not a DPDK port at all.
   *
   * A frame that does not fit the mbuf's data room is the dangerous case: the PMD still reports
   * its full length while the data never lands, so the pcap writer copies that many bytes out of a
   * smaller buffer and writes adjacent mbuf memory into the file. The result is a pcap of
   * same-sized all-zero frames that tcpdump renders as "[|llc]". Sizing for jumbo removes the
   * whole class of problem, at the cost of a larger mempool. */
  uint16_t data_room = RTE_PKTMBUF_HEADROOM + CAPTURE_MAX_FRAME;
  DOCA_LOG_INFO("mbuf data room %u bytes (jumbo-capable, max frame %u)", data_room, CAPTURE_MAX_FRAME);

  struct rte_mempool *mp = rte_pktmbuf_pool_create("mbuf_pool", 8192, 256, 0, data_room, rte_eth_dev_socket_id(first));
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
    /* Ask the PMD for the largest MTU it will accept, so its RQ is not sized for 1500 either. */
    uint16_t want_mtu = CAPTURE_MAX_FRAME - RTE_ETHER_HDR_LEN - RTE_ETHER_CRC_LEN;
    ec.rxmode.mtu = (di.max_mtu && want_mtu > di.max_mtu) ? di.max_mtu : want_mtu;
    if (rte_eth_dev_configure(pid, NB_QUEUES, NB_QUEUES, &ec) < 0) {
      DOCA_LOG_CRIT("configure %u", pid);
      exit(EXIT_FAILURE);
    }
    struct rte_eth_txconf tx = di.default_txconf;
    for (int q = 0; q < NB_QUEUES; q++) {
      if (rte_eth_rx_queue_setup(pid, q, 1024, rte_eth_dev_socket_id(pid), NULL, mp) < 0) {
        DOCA_LOG_CRIT("rxq %u", pid);
        exit(EXIT_FAILURE);
      }
      if (rte_eth_tx_queue_setup(pid, q, 512, rte_eth_dev_socket_id(pid), &tx) < 0) {
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
  doca_error_t err = doca_flow_cfg_create(&cfg);
  crash_if_unsuccessful(err, "doca_flow_cfg_create");
  err = doca_flow_cfg_set_pipe_queues(cfg, NB_QUEUES);
  crash_if_unsuccessful(err, "set_pipe_queues");
  /* Plain "switch,hws": the capture path forwards to an RSS queue, and this is the mode
   * doca_flow_nop is known to work with on 3.4. No shared-resource reservation is needed here —
   * the mirror resource the 2.x build allocated does not exist in this release. */
  err = doca_flow_cfg_set_mode_args(cfg, "switch,hws");
  crash_if_unsuccessful(err, "set_mode_args");
  err = doca_flow_cfg_set_nr_counters(cfg, 4);
  crash_if_unsuccessful(err, "set_nr_counters");
  err = doca_flow_cfg_set_cb_entry_process(cfg, entry_process_cb);
  crash_if_unsuccessful(err, "set_cb_entry_process");
  err = doca_flow_init(cfg);
  crash_if_unsuccessful(err, "doca_flow_init");
  doca_flow_cfg_destroy(cfg);
}

/*
 * DPDK port id of the PF uplink — the eSwitch "proxy" port, which DOCA Flow requires to be the
 * FIRST port started in switch mode. Deliberately not doca_dpdk_get_first_port_id(): on some
 * releases that returns the SF representor, and starting the representor first makes
 * doca_flow_port_start fail with "proxy port 0 not found". See doca_flow_nop.c.
 */
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
  doca_error_t err;
  struct doca_flow_port_cfg *cfg;
  err = doca_flow_port_cfg_create(&cfg);
  crash_if_unsuccessful(err, "port_cfg_create");
  err = doca_flow_port_cfg_set_dev(cfg, dev);
  crash_if_unsuccessful(err, "port_cfg_set_dev");
  err = doca_flow_port_cfg_set_port_id(cfg, pid);
  crash_if_unsuccessful(err, "port_cfg_set_port_id");
  err = doca_flow_port_cfg_set_actions_mem_size(cfg, ACTIONS_MEM_SIZE);
  crash_if_unsuccessful(err, "port_cfg_set_actions_mem_size");
  struct doca_flow_port *port;
  err = doca_flow_port_start(cfg, &port);
  crash_if_unsuccessful(err, "port_start");
  doca_flow_port_cfg_destroy(cfg);
  return port;
}

/*
 * Open the doca_dev_rep for PF0's SF network representor. DOCA 3.4 requires a doca_dev_rep (not
 * just a DPDK port id) to start a representor as a DOCA Flow port. We pick the first entry that
 * reports an SF index — that skips the host-PF/VF representors and lands on our sf0.
 */
static struct doca_dev_rep *open_sf_representor(struct doca_dev *pf_dev) {
  struct doca_devinfo_rep **rep_list;
  uint32_t nb_reps;
  struct doca_dev_rep *rep = NULL;
  doca_error_t err;

  err = doca_devinfo_rep_create_list(pf_dev, DOCA_DEVINFO_REP_FILTER_NET, &rep_list, &nb_reps);
  crash_if_unsuccessful(err, "doca_devinfo_rep_create_list");

  for (uint32_t i = 0; i < nb_reps; i++) {
    uint32_t sf_index;
    if (doca_devinfo_rep_get_sf_index(rep_list[i], &sf_index) == DOCA_SUCCESS) {
      err = doca_dev_rep_open(rep_list[i], &rep);
      crash_if_unsuccessful(err, "doca_dev_rep_open (sf_index=%u)", sf_index);
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

static struct doca_flow_port *rep_port_start(uint16_t pid, struct doca_dev_rep *dev_rep) {
  struct doca_flow_port_cfg *cfg;
  doca_error_t err = doca_flow_port_cfg_create(&cfg);
  crash_if_unsuccessful(err, "rep port_cfg_create");
  err = doca_flow_port_cfg_set_dev_rep(cfg, dev_rep);
  crash_if_unsuccessful(err, "rep set_dev_rep");
  err = doca_flow_port_cfg_set_port_id(cfg, pid);
  crash_if_unsuccessful(err, "rep set_port_id");
  struct doca_flow_port *port;
  err = doca_flow_port_start(cfg, &port);
  crash_if_unsuccessful(err, "rep port_start");
  doca_flow_port_cfg_destroy(cfg);
  return port;
}

/* RSS pipe: matched IPv4 -> CPU RX queue 0 (the flooding pipe's capture entry targets this). */
static struct doca_flow_pipe *create_rss_pipe(struct doca_flow_port *port) {
  static uint16_t rssq[1] = {0};
  struct doca_flow_match match = {0};
  struct doca_flow_fwd fwd = {0};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  struct doca_flow_pipe_entry *e;
  struct entry_batch_status st = {0};
  doca_error_t err;
  match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
  /* 3.4 nests the RSS parameters in fwd.rss (2.x had them flat as rss_queues/num_of_queues/...). */
  fwd.type = DOCA_FLOW_FWD_RSS;
  fwd.rss.queues_array = rssq;
  fwd.rss.nr_queues = 1;
  fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
  err = doca_flow_pipe_cfg_create(&cfg, port);
  crash_if_unsuccessful(err, "rss cfg_create");
  err = doca_flow_pipe_cfg_set_name(cfg, "MIRROR_RSS");
  crash_if_unsuccessful(err, "rss set_name");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  crash_if_unsuccessful(err, "rss set_type");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  crash_if_unsuccessful(err, "rss set_domain");
  err = doca_flow_pipe_cfg_set_is_root(cfg, false);
  crash_if_unsuccessful(err, "rss set_is_root");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
  crash_if_unsuccessful(err, "rss set_nr_entries");
  err = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
  crash_if_unsuccessful(err, "rss set_match");
  err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
  crash_if_unsuccessful(err, "rss create");
  doca_flow_pipe_cfg_destroy(cfg);
  err = doca_flow_pipe_basic_add_entry(0, pipe, &match, 0, NULL, NULL, NULL, DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, &st, &e);
  crash_if_unsuccessful(err, "rss add_entry");
  err = doca_flow_entries_process(port, 0, 10000, 1);
  crash_if_unsuccessful(err, "rss process");
  crash_if_unsuccessful((st.failure || st.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS, "rss install");
  DOCA_LOG_INFO("RSS pipe ready -> CPU queue 0");
  return pipe;
}

/* Plain forward pipe (also the flooding pipe's production target): all IPv4 -> port 1, no mark. */
static struct doca_flow_pipe *create_passthrough_pipe(struct doca_flow_port *port) {
  struct doca_flow_match m = {0}, mm = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = 1};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  struct doca_flow_pipe_entry *e;
  struct entry_batch_status st = {0};
  doca_error_t err;
  m.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  m.outer.ip4.dscp_ecn = 0xFF;
  err = doca_flow_pipe_cfg_create(&cfg, port);
  crash_if_unsuccessful(err, "pass cfg");
  err = doca_flow_pipe_cfg_set_name(cfg, "PASSTHROUGH");
  crash_if_unsuccessful(err, "pass name");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  crash_if_unsuccessful(err, "pass type");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  crash_if_unsuccessful(err, "pass dom");
  err = doca_flow_pipe_cfg_set_is_root(cfg, false);
  crash_if_unsuccessful(err, "pass root");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
  crash_if_unsuccessful(err, "pass nr");
  err = doca_flow_pipe_cfg_set_match(cfg, &m, &mm);
  crash_if_unsuccessful(err, "pass match");
  err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
  crash_if_unsuccessful(err, "pass create");
  doca_flow_pipe_cfg_destroy(cfg);
  m.outer.ip4.dscp_ecn = 0x00;
  err = doca_flow_pipe_basic_add_entry(0, pipe, &m, 0, NULL, NULL, NULL, DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, &st, &e);
  crash_if_unsuccessful(err, "pass entry");
  err = doca_flow_entries_process(port, 0, 10000, 1);
  crash_if_unsuccessful(err, "pass process");
  crash_if_unsuccessful((st.failure || st.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS, "pass install");
  return pipe;
}

/*
 * FLOOD — the DOCA 3.4 stand-in for 2.x's shared mirror.
 *
 * A hash pipe with the flooding algorithm delivers every packet to ALL of its entries, so this is
 * how one packet becomes two: entry 0 continues to the SF (via PASSTHROUGH) and entry 1 goes to
 * the RSS pipe, i.e. to this program's RX queue, where the pcap writer picks it up.
 *
 * Entry 0 is the production path on purpose: with flooding, packet order is only guaranteed for
 * the first entry's destination. Both entries use FWD_PIPE because the pipe is created from one
 * fwd template — hence PASSTHROUGH standing in for a bare FWD_PORT.
 */
static struct doca_flow_pipe *create_flood_pipe(struct doca_flow_port *port, struct doca_flow_pipe *production_pipe,
                                                struct doca_flow_pipe *capture_pipe) {
  struct doca_flow_match m = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = NULL}; /* per-entry below */
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  struct entry_batch_status st = {0};
  struct doca_flow_pipe_entry *e;
  doca_error_t err;

  err = doca_flow_pipe_cfg_create(&cfg, port);
  crash_if_unsuccessful(err, "flood cfg");
  err = doca_flow_pipe_cfg_set_name(cfg, "FLOOD");
  crash_if_unsuccessful(err, "flood name");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_HASH);
  crash_if_unsuccessful(err, "flood type");
  err = doca_flow_pipe_cfg_set_hash_map_algorithm(cfg, DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING);
  crash_if_unsuccessful(err, "flood algorithm");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  crash_if_unsuccessful(err, "flood dom");
  err = doca_flow_pipe_cfg_set_is_root(cfg, false);
  crash_if_unsuccessful(err, "flood root");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, FLOOD_NB_ENTRIES);
  crash_if_unsuccessful(err, "flood nr");
  err = doca_flow_pipe_cfg_set_match(cfg, &m, NULL); /* a hash pipe selects by index, not by match */
  crash_if_unsuccessful(err, "flood match");
  err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe);
  crash_if_unsuccessful(err, "flood create");
  doca_flow_pipe_cfg_destroy(cfg);

  struct doca_flow_fwd ef;

  memset(&ef, 0, sizeof(ef));
  ef.type = DOCA_FLOW_FWD_PIPE;
  ef.next_pipe = production_pipe;
  err = doca_flow_pipe_hash_add_entry(0, pipe, FLOOD_ENTRY_PRODUCTION, 0, NULL, NULL, &ef, DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH, &st, &e);
  crash_if_unsuccessful(err, "flood entry 0 (production)");

  memset(&ef, 0, sizeof(ef));
  ef.type = DOCA_FLOW_FWD_PIPE;
  ef.next_pipe = capture_pipe;
  err = doca_flow_pipe_hash_add_entry(0, pipe, FLOOD_ENTRY_CAPTURE, 0, NULL, NULL, &ef, DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, &st, &e);
  crash_if_unsuccessful(err, "flood entry 1 (capture)");

  err = doca_flow_entries_process(port, 0, 10000, FLOOD_NB_ENTRIES);
  crash_if_unsuccessful(err, "flood process");
  crash_if_unsuccessful((st.failure || st.nb_processed != FLOOD_NB_ENTRIES) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS,
                        "flood install: %u/%u processed", st.nb_processed, FLOOD_NB_ENTRIES);
  DOCA_LOG_INFO("Flooding pipe ready: entry 0 -> mlx5_2 (ordered), entry 1 -> RSS queue 0 (pcap copy)");
  return pipe;
}

/*
 * Capture pipe: all IPv4 -> the flooding pipe when capturing (so the packet reaches both the SF
 * and the pcap), or straight to port 1 when not. If mark==true it also SETs CE (ECN mark).
 * Non-IPv4 miss -> miss_pipe. Returns the entry (for the counter query).
 */
static struct doca_flow_pipe *create_capture_pipe(struct doca_flow_port *port, const char *name, bool mark,
                                                  struct doca_flow_pipe *flood_pipe, struct doca_flow_pipe *miss_pipe,
                                                  struct doca_flow_pipe_entry **out_entry) {
  struct doca_flow_match m = {0}, mm = {0};
  struct doca_flow_actions act = {0}, *act_arr[1] = {&act};
  struct doca_flow_monitor mon = {0};
  struct doca_flow_fwd fwd = {0};
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = miss_pipe};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  struct entry_batch_status st = {0};
  doca_error_t err;

  if (flood_pipe != NULL) {
    /* Fan out to {SF, pcap}. FWD_HASH_PIPE names both the pipe and the algorithm to run on it. */
    fwd.type = DOCA_FLOW_FWD_HASH_PIPE;
    fwd.hash_pipe.pipe = flood_pipe;
    fwd.hash_pipe.algorithm = DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING;
  } else {
    fwd.type = DOCA_FLOW_FWD_PORT;
    fwd.port_id = 1;
  }

  m.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  m.outer.ip4.dscp_ecn = 0xFF;
  mm.outer.ip4.dscp_ecn = 0x00;
  mon.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
  if (mark) {
    act.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    act.outer.ip4.dscp_ecn = 0xFF; /* per-entry value */
  }

  err = doca_flow_pipe_cfg_create(&cfg, port);
  crash_if_unsuccessful(err, "%s cfg", name);
  err = doca_flow_pipe_cfg_set_name(cfg, name);
  crash_if_unsuccessful(err, "%s name", name);
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  crash_if_unsuccessful(err, "%s type", name);
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  crash_if_unsuccessful(err, "%s dom", name);
  err = doca_flow_pipe_cfg_set_is_root(cfg, false);
  crash_if_unsuccessful(err, "%s root", name);
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
  crash_if_unsuccessful(err, "%s nr", name);
  err = doca_flow_pipe_cfg_set_match(cfg, &m, &mm);
  crash_if_unsuccessful(err, "%s match", name);
  if (mark) {
    err = doca_flow_pipe_cfg_set_actions(cfg, act_arr, NULL, NULL, 1);
    crash_if_unsuccessful(err, "%s actions", name);
  }
  err = doca_flow_pipe_cfg_set_monitor(cfg, &mon);
  crash_if_unsuccessful(err, "%s monitor", name);
  err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
  crash_if_unsuccessful(err, "%s create", name);
  doca_flow_pipe_cfg_destroy(cfg);

  /* 3.4 takes the action template index as an argument to add_entry; 2.x carried it in the
   * doca_flow_actions struct, which no longer has an action_idx field. */
  struct doca_flow_actions eact = {0};
  if (mark) {
    eact.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    eact.outer.ip4.dscp_ecn = 0x03; /* CE */
  }
  m.outer.ip4.dscp_ecn = 0x00;
  err = doca_flow_pipe_basic_add_entry(0, pipe, &m, 0, mark ? &eact : NULL, &mon, NULL, DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, &st, out_entry);
  crash_if_unsuccessful(err, "%s add_entry", name);
  err = doca_flow_entries_process(port, 0, 10000, 1);
  crash_if_unsuccessful(err, "%s process", name);
  crash_if_unsuccessful((st.failure || st.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS, "%s install", name);
  DOCA_LOG_INFO("%s pipe ready (%s, %s)", name, mark ? "CE-mark" : "no-mark", flood_pipe ? "flood->SF+pcap" : "direct->SF");
  return pipe;
}

/* RANDOM_SAMPLE: ~fraction (mask) of wire IPv4 -> hit_pipe, rest -> miss_pipe. */
static struct doca_flow_pipe *create_random_sample_pipe(struct doca_flow_port *port, struct doca_flow_pipe *hit,
                                                        struct doca_flow_pipe *miss, uint16_t mask) {
  struct doca_flow_match m = {0}, mm = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = hit};
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = miss};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  struct doca_flow_pipe_entry *e;
  struct entry_batch_status st = {0};
  doca_error_t err;
  m.parser_meta.random = 0;
  mm.parser_meta.random = mask;
  err = doca_flow_pipe_cfg_create(&cfg, port);
  crash_if_unsuccessful(err, "sample cfg");
  err = doca_flow_pipe_cfg_set_name(cfg, "RANDOM_SAMPLE");
  crash_if_unsuccessful(err, "sample name");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  crash_if_unsuccessful(err, "sample type");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  crash_if_unsuccessful(err, "sample dom");
  err = doca_flow_pipe_cfg_set_is_root(cfg, false);
  crash_if_unsuccessful(err, "sample root");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
  crash_if_unsuccessful(err, "sample nr");
  err = doca_flow_pipe_cfg_set_match(cfg, &m, &mm);
  crash_if_unsuccessful(err, "sample match");
  err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
  crash_if_unsuccessful(err, "sample create");
  doca_flow_pipe_cfg_destroy(cfg);
  err = doca_flow_pipe_basic_add_entry(0, pipe, &m, 0, NULL, NULL, NULL, DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, &st, &e);
  crash_if_unsuccessful(err, "sample entry");
  err = doca_flow_entries_process(port, 0, 10000, 1);
  crash_if_unsuccessful(err, "sample process");
  crash_if_unsuccessful((st.failure || st.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS, "sample install");
  DOCA_LOG_INFO("Random-sample pipe ready: mask 0x%04x", mask);
  return pipe;
}

static void create_port_demux_pipe(struct doca_flow_port *port, struct doca_flow_pipe *wire_target) {
  struct doca_flow_match m = {0}, mm = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_DROP};
  struct doca_flow_pipe_cfg *cfg;
  struct doca_flow_pipe *pipe;
  doca_error_t err;
  /* 3.4 matches the source port through parser_meta.port_id (2.x used port_meta). */
  m.parser_meta.port_id = UINT16_MAX;
  mm.parser_meta.port_id = UINT16_MAX;
  err = doca_flow_pipe_cfg_create(&cfg, port);
  crash_if_unsuccessful(err, "demux cfg");
  err = doca_flow_pipe_cfg_set_name(cfg, "PORT_DEMUX");
  crash_if_unsuccessful(err, "demux name");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
  crash_if_unsuccessful(err, "demux type");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  crash_if_unsuccessful(err, "demux dom");
  err = doca_flow_pipe_cfg_set_is_root(cfg, true);
  crash_if_unsuccessful(err, "demux root");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 2);
  crash_if_unsuccessful(err, "demux nr");
  err = doca_flow_pipe_cfg_set_match(cfg, &m, &mm);
  crash_if_unsuccessful(err, "demux match");
  err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);
  crash_if_unsuccessful(err, "demux create");
  doca_flow_pipe_cfg_destroy(cfg);
  struct doca_flow_match em = {0};
  struct doca_flow_fwd ef;
  struct entry_batch_status st = {0};
  struct doca_flow_pipe_entry *e;
  em.parser_meta.port_id = 0;
  memset(&ef, 0, sizeof(ef));
  ef.type = DOCA_FLOW_FWD_PIPE;
  ef.next_pipe = wire_target;
  err = doca_flow_pipe_basic_add_entry(0, pipe, &em, 0, NULL, NULL, &ef, DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH, &st, &e);
  crash_if_unsuccessful(err, "demux wire");
  em.parser_meta.port_id = 1;
  memset(&ef, 0, sizeof(ef));
  ef.type = DOCA_FLOW_FWD_PORT;
  ef.port_id = 0;
  err = doca_flow_pipe_basic_add_entry(0, pipe, &em, 0, NULL, NULL, &ef, DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, &st, &e);
  crash_if_unsuccessful(err, "demux sf");
  err = doca_flow_entries_process(port, 0, 10000, 2);
  crash_if_unsuccessful(err, "demux process");
  crash_if_unsuccessful((st.failure || st.nb_processed != 2) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS, "demux install");
  DOCA_LOG_INFO("Port demux ready");
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
  doca_error_t err;
  err = doca_argp_param_create(&p);
  crash_if_unsuccessful(err, "param pcap");
  doca_argp_param_set_long_name(p, "pcap");
  doca_argp_param_set_description(p, "Output pcap file. Omit to run in pure ECN-mark mode (no capture, full goodput).");
  doca_argp_param_set_callback(p, pcap_cb);
  doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
  err = doca_argp_register_param(p);
  crash_if_unsuccessful(err, "register pcap");
  err = doca_argp_param_create(&p);
  crash_if_unsuccessful(err, "param percent");
  doca_argp_param_set_long_name(p, "percent");
  doca_argp_param_set_description(
      p,
      "Percent of packets to CE-mark [0,100] (rounded down to a power-of-2 fraction; default 100). All packets are captured regardless.");
  doca_argp_param_set_callback(p, percent_cb);
  doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
  err = doca_argp_register_param(p);
  crash_if_unsuccessful(err, "register percent");
  err = doca_argp_param_create(&p);
  crash_if_unsuccessful(err, "param sample");
  doca_argp_param_set_long_name(p, "sample");
  doca_argp_param_set_description(
      p, "Write only ~1-in-N captured packets to the pcap (default 1 = every packet). Marking/forwarding are unaffected.");
  doca_argp_param_set_callback(p, sample_cb);
  doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
  err = doca_argp_register_param(p);
  crash_if_unsuccessful(err, "register sample");
}

int main(int argc, char **argv) {
  doca_error_t err = doca_log_backend_create_standard();
  crash_if_unsuccessful(err, "log_backend");
  struct doca_log_backend *sdk;
  err = doca_log_backend_create_with_file_sdk(stderr, &sdk);
  crash_if_unsuccessful(err, "sdk log");
  err = doca_log_backend_set_sdk_level(sdk, DOCA_LOG_LEVEL_WARNING);
  crash_if_unsuccessful(err, "sdk level");

  struct app_config cfg = {.pcap_path = NULL, .random_percent = 100.0, .sample_n = 1};
  err = doca_argp_init("doca_flow_ecn_pcap", &cfg);
  crash_if_unsuccessful(err, "argp_init");
  doca_argp_set_dpdk_program(initialize_dpdk);
  register_params();
  err = doca_argp_start(argc, argv);
  crash_if_unsuccessful(err, "argp_start");

  bool capture = (cfg.pcap_path != NULL); /* --pcap given => also flood a copy to the pcap */
  pcap_t *pd = NULL;
  pcap_dumper_t *dumper = NULL;
  if (capture) {
    pd = pcap_open_dead(DLT_EN10MB, SNAPLEN);
    if (!pd) {
      DOCA_LOG_CRIT("pcap_open_dead");
      return EXIT_FAILURE;
    }
    dumper = pcap_dump_open(pd, cfg.pcap_path);
    if (!dumper) {
      DOCA_LOG_CRIT("pcap_dump_open('%s'): %s", cfg.pcap_path, pcap_geterr(pd));
      return EXIT_FAILURE;
    }
  }

  struct doca_dev *dev = open_and_probe_dev(0, "dv_flow_en=2,fdb_def_rule_en=1,representor=sf0");
  configure_and_start_dpdk_port(dev);
  initialize_doca_flow();
  struct doca_flow_port *port = port_start(dev);
  struct doca_dev_rep *sf_rep_dev = open_sf_representor(dev);
  struct doca_flow_port *sf_rep = rep_port_start(1, sf_rep_dev);
  uint16_t pf0 = find_pf_port_id();

  /* PASSTHROUGH doubles as the non-IPv4 miss target and, when capturing, as the flooding pipe's
   * ordered production entry. */
  struct doca_flow_pipe *passthrough = create_passthrough_pipe(port);

  struct doca_flow_pipe *flood = NULL;
  if (capture) {
    struct doca_flow_pipe *rss = create_rss_pipe(port);
    flood = create_flood_pipe(port, passthrough, rss);
  }

  /* PASS_CAPTURE (no mark) and MARK_CAPTURE (CE-mark); both fan out to the pcap when capturing. */
  struct doca_flow_pipe_entry *pass_e = NULL, *ce_e = NULL;
  struct doca_flow_pipe *pass_cap = create_capture_pipe(port, "PASS_CAPTURE", false, flood, passthrough, &pass_e);
  struct doca_flow_pipe *mark_cap = NULL;
  if (cfg.random_percent > 0.0) mark_cap = create_capture_pipe(port, "MARK_CAPTURE", true, flood, passthrough, &ce_e);

  /* wire-ingress entry point per --percent */
  uint16_t mask = 0;
  struct doca_flow_pipe *wire_target;
  if (cfg.random_percent >= 100.0)
    wire_target = mark_cap; /* mark+capture all */
  else if (cfg.random_percent <= 0.0)
    wire_target = pass_cap; /* capture all, mark none */
  else {
    mask = get_random_mask(cfg.random_percent);
    wire_target = create_random_sample_pipe(port, mark_cap, pass_cap, mask);
  }

  create_port_demux_pipe(port, wire_target);

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  const char *dst = capture ? cfg.pcap_path : "(none — pure ECN-mark mode)";
  if (cfg.random_percent >= 100.0)
    DOCA_LOG_INFO("Marking ALL IPv4 | capture: %s — Ctrl-C to stop", dst);
  else if (cfg.random_percent <= 0.0)
    DOCA_LOG_INFO("Marking NONE     | capture: %s — Ctrl-C to stop", dst);
  else
    DOCA_LOG_INFO("Marking ~%.4g%%   | capture: %s — Ctrl-C to stop", 100.0 / (mask + 1), dst);
  if (capture) {
    if (cfg.sample_n > 1) DOCA_LOG_INFO("Capturing ~1-in-%u packets to the pcap", cfg.sample_n);
    signal(SIGUSR1, toggle_signal_handler);
    if (enable_key_toggle())
      DOCA_LOG_INFO("pcap writing starts PAUSED — press SPACE (or 'c'/'p'), or `kill -USR1 %d`, to start/stop writing to '%s'",
                    (int)getpid(), cfg.pcap_path);
    else
      DOCA_LOG_INFO("pcap writing starts PAUSED — no tty, so SPACE cannot be read: `kill -USR1 %d` to start/stop writing to '%s'",
                    (int)getpid(), cfg.pcap_path);
  }

  struct rte_mbuf *bufs[RX_BURST];
  uint64_t written = 0, mirrored = 0, sample_ctr = 0, truncated = 0;
  time_t last = time(NULL);
  while (g_running) {
    uint16_t nb = 0;
    if (capture) {
      nb = rte_eth_rx_burst(pf0, 0, bufs, RX_BURST);
      for (uint16_t i = 0; i < nb; i++) {
        mirrored++;
        bool take = (++sample_ctr % cfg.sample_n == 0); /* ~1-in-N (N==1 => every packet) */
        if (g_capture_writing && take) {
          struct pcap_pkthdr h;
          struct timeval tv;
          gettimeofday(&tv, NULL);
          h.ts = tv;
          /* Only ever write what this segment really holds. data_len is the first segment; if the
           * frame were ever chained across mbufs, pkt_len would exceed it and copying pkt_len
           * bytes would run off the end of the buffer. */
          h.caplen = rte_pktmbuf_data_len(bufs[i]);
          h.len = rte_pktmbuf_pkt_len(bufs[i]);
          if (h.caplen > h.len) h.caplen = h.len;
          if (h.len > h.caplen && truncated++ == 0)
            DOCA_LOG_WARN("captured frame is segmented (%u of %u bytes) — pcap entries will be truncated", h.caplen, h.len);
          pcap_dump((u_char *)dumper, &h, rte_pktmbuf_mtod(bufs[i], const u_char *));
          /* Flush every record, the way `tcpdump -U` does. pcap_dump goes through stdio's 4 KB
           * buffer, so without this a partial record sits on disk between flushes — and a frame
           * larger than that buffer leaves one there essentially always. Anything following the
           * file (check_ecn_bits_from_pcap.sh, tcpdump -r) then hits a truncated record and stops
           * after the first packet. Flushing per record keeps the file readable while it grows. */
          pcap_dump_flush(dumper);
          written++;
        }
        rte_pktmbuf_free(bufs[i]);
      }
    }
    if (nb == 0) usleep(200);
    poll_capture_toggle();
    time_t now = time(NULL);
    if (now != last) {
      last = now;
      if (capture) pcap_dump_flush(dumper);
      uint64_t ce = query_pkts(ce_e), pass = query_pkts(pass_e), tot = ce + pass;
      if (capture)
        DOCA_LOG_INFO("CE marked: %lu, passthrough: %lu (%.4g%% marked) | flooded: %lu -> pcap: %lu%s", ce, pass,
                      tot ? 100.0 * (double)ce / (double)tot : 0.0, mirrored, written, g_capture_writing ? "" : " [PAUSED]");
      else
        DOCA_LOG_INFO("CE marked: %lu, passthrough: %lu (%.4g%% marked)", ce, pass, tot ? 100.0 * (double)ce / (double)tot : 0.0);
    }
  }

  restore_key_toggle();
  if (capture) {
    pcap_dump_flush(dumper);
    pcap_dump_close(dumper);
    pcap_close(pd);
    DOCA_LOG_INFO("Wrote %lu packets to %s", written, cfg.pcap_path);
  }
  doca_flow_port_stop(sf_rep);
  doca_flow_port_stop(port);
  doca_flow_destroy();
  doca_argp_destroy();
  return EXIT_SUCCESS;
}
