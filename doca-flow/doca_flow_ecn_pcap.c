/*
 * doca_flow_ecn_pcap — ECN-mark AND capture-to-pcap in ONE PF0 program.
 *
 * Combines doca_flow_ecn (set CE on ~--percent of wire packets) with the mirror->RSS->pcap capture
 * from doca_flow_mirror, so you don't have to fight over PF0 with two separate programs.
 *
 * Topology (PF0 switch/FDB):
 *   PORT_DEMUX (root, by source port):
 *     p0 wire ingress  -> [RANDOM_SAMPLE] -> MARK_CAPTURE (set CE + fwd mlx5_2 + mirror->pcap)
 *                                          \-> PASS_CAPTURE (fwd mlx5_2 + mirror->pcap, no mark)
 *     mlx5_2 SF egress -> p0 wire (return path for ACKs/CNPs)
 * Every wire IPv4 packet is mirrored to CPU RX queue 0 and written to <pcap>; ~--percent of them
 * are CE-marked (the marked copies show CE in the pcap). Non-IPv4 -> PASSTHROUGH (fwd only).
 *
 * NOTE: uses the plain "switch,hws" mode (RSS capture needs it) — NOT the isolated/disable_switch_rss
 * mode the standalone doca_flow_ecn uses.
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
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>

DOCA_LOG_REGISTER(FLOW_ECN_PCAP);

#define NB_QUEUES 1
#define MIRROR_ID 1
#define RX_BURST 64
#define SNAPLEN 262144
#define RANDOM_FIELD_WIDTH 16

struct app_config {
  const char *pcap_path;  /* --pcap: NULL => pure ECN-mark mode (no capture) */
  double random_percent;  /* --percent, [0,100], default 100 */
  uint32_t sample_n;      /* --sample N: write ~1-in-N captured packets to the pcap (default 1) */
};

static volatile bool g_running = true;
static volatile bool g_capture_writing = false;  /* starts OFF; SPACE toggles pcap writing at runtime */
static struct termios g_saved_termios;
static bool g_termios_saved = false;

static void signal_handler(int s) { if (s == SIGINT || s == SIGTERM) g_running = false; }

/* Put STDIN into unbuffered, non-blocking mode so a single keypress (SPACE) toggles capture. */
static void enable_key_toggle(void) {
  struct termios t;
  if (tcgetattr(STDIN_FILENO, &t) != 0) return;  /* not a tty (piped/nohup) — no toggle, no hang */
  g_saved_termios = t; g_termios_saved = true;
  t.c_lflag &= ~(ICANON | ECHO);
  t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &t);
  int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (fl != -1) fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
}
static void restore_key_toggle(void) {
  if (g_termios_saved) tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
}
/* Drain any pending keypresses; SPACE / 'c' / 'p' flips whether packets are written to the pcap. */
static void poll_key_toggle(void) {
  if (!g_termios_saved) return;
  char c;
  while (read(STDIN_FILENO, &c, 1) == 1) {
    if (c == ' ' || c == 'c' || c == 'p') {
      g_capture_writing = !g_capture_writing;
      DOCA_LOG_INFO("[toggle] pcap writing %s (HW mirror stays active)", g_capture_writing ? "ENABLED" : "PAUSED");
    }
  }
}

static __attribute__((format(printf, 2, 3))) void crash_if_unsuccessful(doca_error_t err, const char *fmt, ...) {
  if (err == DOCA_SUCCESS) return;
  char msg[512]; va_list a; va_start(a, fmt); vsnprintf(msg, sizeof(msg), fmt, a); va_end(a);
  DOCA_LOG_CRIT("%s: %s", msg, doca_error_get_descr(err));
  exit(EXIT_FAILURE);
}

/* percent -> nearest power-of-two random mask (same technique as doca_flow_ecn). */
static uint16_t get_random_mask(double percentage) {
  double next = 50.0; uint8_t i;
  for (i = 1; i <= RANDOM_FIELD_WIDTH; ++i) { if (percentage >= next) break; next /= 2; }
  return (uint16_t)((1u << i) - 1);
}

struct entry_batch_status { bool failure; uint32_t nb_processed; };
static void entry_process_cb(struct doca_flow_pipe_entry *e, uint16_t q, enum doca_flow_entry_status st,
                             enum doca_flow_entry_op op, void *ctx) {
  (void)e; (void)q; (void)op;
  struct entry_batch_status *s = ctx;
  if (!s) return;
  if (st != DOCA_FLOW_ENTRY_STATUS_SUCCESS) s->failure = true;
  s->nb_processed++;
}

static doca_error_t initialize_dpdk(int argc, char **argv) {
  static char allow_flag[] = "-a"; static char dummy_pci[] = "pci:00:00.0";
  char *nv[64];
  if (argc >= 62) { DOCA_LOG_ERR("Too many EAL arguments"); return DOCA_ERROR_INVALID_VALUE; }
  for (int i = 0; i < argc; i++) nv[i] = argv[i];
  nv[argc] = allow_flag; nv[argc + 1] = dummy_pci;
  if (rte_eal_init(argc + 2, nv) < 0) { DOCA_LOG_ERR("EAL initialization failed"); return DOCA_ERROR_DRIVER; }
  return DOCA_SUCCESS;
}

static struct doca_dev *open_and_probe_dev(uint32_t index, const char *probe_args) {
  struct doca_devinfo **list; uint32_t n; struct doca_dev *dev; doca_error_t err;
  err = doca_devinfo_create_list(&list, &n); crash_if_unsuccessful(err, "doca_devinfo_create_list");
  if (index >= n) { DOCA_LOG_CRIT("Device index %u out of range (%u)", index, n); exit(EXIT_FAILURE); }
  err = doca_dev_open(list[index], &dev); crash_if_unsuccessful(err, "doca_dev_open");
  doca_devinfo_destroy_list(list);
  err = doca_dpdk_port_probe(dev, probe_args); crash_if_unsuccessful(err, "doca_dpdk_port_probe");
  return dev;
}

static void configure_and_start_dpdk_port(struct doca_dev *dev) {
  uint16_t first; doca_error_t err = doca_dpdk_get_first_port_id(dev, &first);
  crash_if_unsuccessful(err, "doca_dpdk_get_first_port_id");
  struct rte_mempool *mp = rte_pktmbuf_pool_create("mbuf_pool", 8192, 256, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                                   rte_eth_dev_socket_id(first));
  if (!mp) { DOCA_LOG_CRIT("rte_pktmbuf_pool_create failed"); exit(EXIT_FAILURE); }
  uint16_t pid;
  RTE_ETH_FOREACH_DEV(pid) {
    struct rte_eth_dev_info di = {0};
    if (rte_eth_dev_info_get(pid, &di) < 0) { DOCA_LOG_CRIT("dev_info port %u", pid); exit(EXIT_FAILURE); }
    struct rte_eth_conf ec = {0};
    if (rte_eth_dev_configure(pid, NB_QUEUES, NB_QUEUES, &ec) < 0) { DOCA_LOG_CRIT("configure %u", pid); exit(EXIT_FAILURE); }
    struct rte_eth_txconf tx = di.default_txconf;
    for (int q = 0; q < NB_QUEUES; q++) {
      if (rte_eth_rx_queue_setup(pid, q, 1024, rte_eth_dev_socket_id(pid), NULL, mp) < 0) { DOCA_LOG_CRIT("rxq %u", pid); exit(EXIT_FAILURE); }
      if (rte_eth_tx_queue_setup(pid, q, 512, rte_eth_dev_socket_id(pid), &tx) < 0) { DOCA_LOG_CRIT("txq %u", pid); exit(EXIT_FAILURE); }
    }
    if (rte_eth_dev_start(pid) < 0) { DOCA_LOG_CRIT("start %u", pid); exit(EXIT_FAILURE); }
  }
}

static void initialize_doca_flow(void) {
  struct doca_flow_cfg *cfg; doca_error_t err = doca_flow_cfg_create(&cfg);
  crash_if_unsuccessful(err, "doca_flow_cfg_create");
  err = doca_flow_cfg_set_pipe_queues(cfg, NB_QUEUES); crash_if_unsuccessful(err, "set_pipe_queues");
  err = doca_flow_cfg_set_mode_args(cfg, "switch,hws"); crash_if_unsuccessful(err, "set_mode_args");
  err = doca_flow_cfg_set_nr_counters(cfg, 4); crash_if_unsuccessful(err, "set_nr_counters");
  err = doca_flow_cfg_set_nr_shared_resource(cfg, MIRROR_ID + 1, DOCA_FLOW_SHARED_RESOURCE_MIRROR);
  crash_if_unsuccessful(err, "set_nr_shared_resource (mirror)");
  err = doca_flow_cfg_set_cb_entry_process(cfg, entry_process_cb); crash_if_unsuccessful(err, "set_cb_entry_process");
  err = doca_flow_init(cfg); crash_if_unsuccessful(err, "doca_flow_init");
  doca_flow_cfg_destroy(cfg);
}

static struct doca_flow_port *port_start(struct doca_dev *dev) {
  uint16_t pid; doca_error_t err = doca_dpdk_get_first_port_id(dev, &pid);
  crash_if_unsuccessful(err, "get_first_port_id");
  struct doca_flow_port_cfg *cfg; err = doca_flow_port_cfg_create(&cfg); crash_if_unsuccessful(err, "port_cfg_create");
  err = doca_flow_port_cfg_set_dev(cfg, dev); crash_if_unsuccessful(err, "port_cfg_set_dev");
  char s[8]; snprintf(s, sizeof(s), "%u", pid);
  err = doca_flow_port_cfg_set_devargs(cfg, s); crash_if_unsuccessful(err, "port_cfg_set_devargs");
  struct doca_flow_port *port; err = doca_flow_port_start(cfg, &port); crash_if_unsuccessful(err, "port_start");
  doca_flow_port_cfg_destroy(cfg); return port;
}

static struct doca_flow_port *rep_port_start(uint16_t pid) {
  struct doca_flow_port_cfg *cfg; char s[8]; snprintf(s, sizeof(s), "%u", pid);
  doca_error_t err = doca_flow_port_cfg_create(&cfg); crash_if_unsuccessful(err, "rep port_cfg_create");
  err = doca_flow_port_cfg_set_devargs(cfg, s); crash_if_unsuccessful(err, "rep set_devargs");
  struct doca_flow_port *port; err = doca_flow_port_start(cfg, &port); crash_if_unsuccessful(err, "rep port_start");
  doca_flow_port_cfg_destroy(cfg); return port;
}

/* RSS pipe: matched IPv4 -> CPU RX queue 0 (the mirror targets this pipe). */
static struct doca_flow_pipe *create_rss_pipe(struct doca_flow_port *port) {
  static uint16_t rssq[1] = {0};
  struct doca_flow_match match = {0}; struct doca_flow_fwd fwd = {0};
  struct doca_flow_pipe_cfg *cfg; struct doca_flow_pipe *pipe; struct doca_flow_pipe_entry *e;
  struct entry_batch_status st = {0}; doca_error_t err;
  match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
  fwd.type = DOCA_FLOW_FWD_RSS; fwd.rss_queues = rssq; fwd.num_of_queues = 1;
  fwd.rss_outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
  err = doca_flow_pipe_cfg_create(&cfg, port); crash_if_unsuccessful(err, "rss cfg_create");
  err = doca_flow_pipe_cfg_set_name(cfg, "MIRROR_RSS"); crash_if_unsuccessful(err, "rss set_name");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC); crash_if_unsuccessful(err, "rss set_type");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT); crash_if_unsuccessful(err, "rss set_domain");
  err = doca_flow_pipe_cfg_set_is_root(cfg, false); crash_if_unsuccessful(err, "rss set_is_root");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1); crash_if_unsuccessful(err, "rss set_nr_entries");
  err = doca_flow_pipe_cfg_set_match(cfg, &match, NULL); crash_if_unsuccessful(err, "rss set_match");
  err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe); crash_if_unsuccessful(err, "rss create");
  doca_flow_pipe_cfg_destroy(cfg);
  err = doca_flow_pipe_add_entry(0, pipe, &match, NULL, NULL, NULL, 0, &st, &e); crash_if_unsuccessful(err, "rss add_entry");
  err = doca_flow_entries_process(port, 0, 10000, 1); crash_if_unsuccessful(err, "rss process");
  crash_if_unsuccessful((st.failure || st.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS, "rss install");
  DOCA_LOG_INFO("RSS pipe ready -> CPU queue 0");
  return pipe;
}

static void setup_capture_mirror(struct doca_flow_port *port, struct doca_flow_pipe *rss_pipe) {
  struct doca_flow_mirror_target target = {0}; struct doca_flow_resource_mirror_cfg mc = {0};
  struct doca_flow_shared_resource_cfg cfg = {0}; uint32_t ids[1] = {MIRROR_ID}; doca_error_t err;
  target.fwd.type = DOCA_FLOW_FWD_PIPE; target.fwd.next_pipe = rss_pipe;
  mc.nr_targets = 1; mc.target = &target; cfg.mirror_cfg = mc;
  err = doca_flow_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_MIRROR, MIRROR_ID, &cfg);
  crash_if_unsuccessful(err, "mirror set_cfg");
  err = doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_MIRROR, ids, 1, port);
  crash_if_unsuccessful(err, "mirror bind");
  DOCA_LOG_INFO("Shared mirror %u -> RSS pipe -> queue 0", MIRROR_ID);
}

/* Plain forward pipe (non-IPv4 catch): all IPv4 -> port 1, no mark, no mirror. */
static struct doca_flow_pipe *create_passthrough_pipe(struct doca_flow_port *port) {
  struct doca_flow_match m = {0}, mm = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = 1};
  struct doca_flow_pipe_cfg *cfg; struct doca_flow_pipe *pipe; struct doca_flow_pipe_entry *e;
  struct entry_batch_status st = {0}; doca_error_t err;
  m.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4; m.outer.ip4.dscp_ecn = 0xFF;
  err = doca_flow_pipe_cfg_create(&cfg, port); crash_if_unsuccessful(err, "pass cfg");
  err = doca_flow_pipe_cfg_set_name(cfg, "PASSTHROUGH"); crash_if_unsuccessful(err, "pass name");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC); crash_if_unsuccessful(err, "pass type");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT); crash_if_unsuccessful(err, "pass dom");
  err = doca_flow_pipe_cfg_set_is_root(cfg, false); crash_if_unsuccessful(err, "pass root");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1); crash_if_unsuccessful(err, "pass nr");
  err = doca_flow_pipe_cfg_set_match(cfg, &m, &mm); crash_if_unsuccessful(err, "pass match");
  err = doca_flow_pipe_create(cfg, &fwd, NULL, &pipe); crash_if_unsuccessful(err, "pass create");
  doca_flow_pipe_cfg_destroy(cfg);
  m.outer.ip4.dscp_ecn = 0x00;
  err = doca_flow_pipe_add_entry(0, pipe, &m, NULL, NULL, NULL, 0, &st, &e); crash_if_unsuccessful(err, "pass entry");
  err = doca_flow_entries_process(port, 0, 10000, 1); crash_if_unsuccessful(err, "pass process");
  crash_if_unsuccessful((st.failure || st.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS, "pass install");
  return pipe;
}

/*
 * Capture pipe: all IPv4 -> port 1, counter (+ mirror->pcap when mirror==true). If mark==true it
 * also SETs CE (ECN mark). Non-IPv4 miss -> miss_pipe. Returns the entry (for the counter query).
 */
static struct doca_flow_pipe *create_capture_pipe(struct doca_flow_port *port, const char *name, bool mark,
                                                  bool mirror, struct doca_flow_pipe *miss_pipe,
                                                  struct doca_flow_pipe_entry **out_entry) {
  struct doca_flow_match m = {0}, mm = {0};
  struct doca_flow_actions act = {0}, *act_arr[1] = {&act};
  struct doca_flow_monitor mon = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = 1};
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = miss_pipe};
  struct doca_flow_pipe_cfg *cfg; struct doca_flow_pipe *pipe;
  struct entry_batch_status st = {0}; doca_error_t err;

  m.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4; m.outer.ip4.dscp_ecn = 0xFF; mm.outer.ip4.dscp_ecn = 0x00;
  mon.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
  if (mirror) mon.shared_mirror_id = MIRROR_ID;  /* mirror a copy to the pcap (0 == no mirror) */
  if (mark) { act.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4; act.outer.ip4.dscp_ecn = 0xFF; } /* per-entry value */

  err = doca_flow_pipe_cfg_create(&cfg, port); crash_if_unsuccessful(err, "%s cfg", name);
  err = doca_flow_pipe_cfg_set_name(cfg, name); crash_if_unsuccessful(err, "%s name", name);
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC); crash_if_unsuccessful(err, "%s type", name);
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT); crash_if_unsuccessful(err, "%s dom", name);
  err = doca_flow_pipe_cfg_set_is_root(cfg, false); crash_if_unsuccessful(err, "%s root", name);
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1); crash_if_unsuccessful(err, "%s nr", name);
  err = doca_flow_pipe_cfg_set_match(cfg, &m, &mm); crash_if_unsuccessful(err, "%s match", name);
  if (mark) { err = doca_flow_pipe_cfg_set_actions(cfg, act_arr, NULL, NULL, 1); crash_if_unsuccessful(err, "%s actions", name); }
  err = doca_flow_pipe_cfg_set_monitor(cfg, &mon); crash_if_unsuccessful(err, "%s monitor", name);
  err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe); crash_if_unsuccessful(err, "%s create", name);
  doca_flow_pipe_cfg_destroy(cfg);

  struct doca_flow_actions eact = {0};
  if (mark) { eact.action_idx = 0; eact.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4; eact.outer.ip4.dscp_ecn = 0x03; /* CE */ }
  m.outer.ip4.dscp_ecn = 0x00;
  err = doca_flow_pipe_add_entry(0, pipe, &m, mark ? &eact : NULL, &mon, NULL, 0, &st, out_entry);
  crash_if_unsuccessful(err, "%s add_entry", name);
  err = doca_flow_entries_process(port, 0, 10000, 1); crash_if_unsuccessful(err, "%s process", name);
  crash_if_unsuccessful((st.failure || st.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS, "%s install", name);
  DOCA_LOG_INFO("%s pipe ready (%s + capture)", name, mark ? "CE-mark" : "no-mark");
  return pipe;
}

/* RANDOM_SAMPLE: ~fraction (mask) of wire IPv4 -> hit_pipe, rest -> miss_pipe. */
static struct doca_flow_pipe *create_random_sample_pipe(struct doca_flow_port *port, struct doca_flow_pipe *hit,
                                                        struct doca_flow_pipe *miss, uint16_t mask) {
  struct doca_flow_match m = {0}, mm = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = hit};
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = miss};
  struct doca_flow_pipe_cfg *cfg; struct doca_flow_pipe *pipe; struct doca_flow_pipe_entry *e;
  struct entry_batch_status st = {0}; doca_error_t err;
  m.parser_meta.random = 0; mm.parser_meta.random = mask;
  err = doca_flow_pipe_cfg_create(&cfg, port); crash_if_unsuccessful(err, "sample cfg");
  err = doca_flow_pipe_cfg_set_name(cfg, "RANDOM_SAMPLE"); crash_if_unsuccessful(err, "sample name");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC); crash_if_unsuccessful(err, "sample type");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT); crash_if_unsuccessful(err, "sample dom");
  err = doca_flow_pipe_cfg_set_is_root(cfg, false); crash_if_unsuccessful(err, "sample root");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 1); crash_if_unsuccessful(err, "sample nr");
  err = doca_flow_pipe_cfg_set_match(cfg, &m, &mm); crash_if_unsuccessful(err, "sample match");
  err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe); crash_if_unsuccessful(err, "sample create");
  doca_flow_pipe_cfg_destroy(cfg);
  err = doca_flow_pipe_add_entry(0, pipe, &m, NULL, NULL, NULL, 0, &st, &e); crash_if_unsuccessful(err, "sample entry");
  err = doca_flow_entries_process(port, 0, 10000, 1); crash_if_unsuccessful(err, "sample process");
  crash_if_unsuccessful((st.failure || st.nb_processed != 1) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS, "sample install");
  DOCA_LOG_INFO("Random-sample pipe ready: mask 0x%04x", mask);
  return pipe;
}

static void create_port_demux_pipe(struct doca_flow_port *port, struct doca_flow_pipe *wire_target) {
  struct doca_flow_match m = {0}, mm = {0};
  struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};
  struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_DROP};
  struct doca_flow_pipe_cfg *cfg; struct doca_flow_pipe *pipe; doca_error_t err;
  m.parser_meta.port_meta = UINT32_MAX; mm.parser_meta.port_meta = UINT32_MAX;
  err = doca_flow_pipe_cfg_create(&cfg, port); crash_if_unsuccessful(err, "demux cfg");
  err = doca_flow_pipe_cfg_set_name(cfg, "PORT_DEMUX"); crash_if_unsuccessful(err, "demux name");
  err = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC); crash_if_unsuccessful(err, "demux type");
  err = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_DEFAULT); crash_if_unsuccessful(err, "demux dom");
  err = doca_flow_pipe_cfg_set_is_root(cfg, true); crash_if_unsuccessful(err, "demux root");
  err = doca_flow_pipe_cfg_set_nr_entries(cfg, 2); crash_if_unsuccessful(err, "demux nr");
  err = doca_flow_pipe_cfg_set_match(cfg, &m, &mm); crash_if_unsuccessful(err, "demux match");
  err = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe); crash_if_unsuccessful(err, "demux create");
  doca_flow_pipe_cfg_destroy(cfg);
  struct doca_flow_match em = {0}; struct doca_flow_fwd ef; struct entry_batch_status st = {0}; struct doca_flow_pipe_entry *e;
  em.parser_meta.port_meta = 0; memset(&ef, 0, sizeof(ef)); ef.type = DOCA_FLOW_FWD_PIPE; ef.next_pipe = wire_target;
  err = doca_flow_pipe_add_entry(0, pipe, &em, NULL, NULL, &ef, DOCA_FLOW_WAIT_FOR_BATCH, &st, &e);
  crash_if_unsuccessful(err, "demux wire");
  em.parser_meta.port_meta = 1; memset(&ef, 0, sizeof(ef)); ef.type = DOCA_FLOW_FWD_PORT; ef.port_id = 0;
  err = doca_flow_pipe_add_entry(0, pipe, &em, NULL, NULL, &ef, 0, &st, &e); crash_if_unsuccessful(err, "demux sf");
  err = doca_flow_entries_process(port, 0, 10000, 2); crash_if_unsuccessful(err, "demux process");
  crash_if_unsuccessful((st.failure || st.nb_processed != 2) ? DOCA_ERROR_BAD_STATE : DOCA_SUCCESS, "demux install");
  DOCA_LOG_INFO("Port demux ready");
}

static uint64_t query_pkts(struct doca_flow_pipe_entry *e) {
  if (!e) return 0;
  struct doca_flow_resource_query q;
  return (doca_flow_resource_query_entry(e, &q) == DOCA_SUCCESS) ? q.counter.total_pkts : 0;
}

static doca_error_t pcap_cb(void *p, void *c) { struct app_config *cfg = c; cfg->pcap_path = strdup((const char *)p); return cfg->pcap_path ? DOCA_SUCCESS : DOCA_ERROR_NO_MEMORY; }
static doca_error_t percent_cb(void *p, void *c) {
  struct app_config *cfg = c; double v = atof((const char *)p);
  if (v < 0.0 || v > 100.0) { DOCA_LOG_ERR("--percent must be [0,100]"); return DOCA_ERROR_INVALID_VALUE; }
  cfg->random_percent = v; return DOCA_SUCCESS;
}
static doca_error_t sample_cb(void *p, void *c) {
  struct app_config *cfg = c; long v = atol((const char *)p);
  if (v < 1) { DOCA_LOG_ERR("--sample must be >= 1"); return DOCA_ERROR_INVALID_VALUE; }
  cfg->sample_n = (uint32_t)v; return DOCA_SUCCESS;
}
static void register_params(void) {
  struct doca_argp_param *p; doca_error_t err;
  err = doca_argp_param_create(&p); crash_if_unsuccessful(err, "param pcap");
  doca_argp_param_set_long_name(p, "pcap");
  doca_argp_param_set_description(p, "Output pcap file. Omit to run in pure ECN-mark mode (no capture, full goodput).");
  doca_argp_param_set_callback(p, pcap_cb); doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
  err = doca_argp_register_param(p); crash_if_unsuccessful(err, "register pcap");
  err = doca_argp_param_create(&p); crash_if_unsuccessful(err, "param percent");
  doca_argp_param_set_long_name(p, "percent");
  doca_argp_param_set_description(p, "Percent of packets to CE-mark [0,100] (rounded down to a power-of-2 fraction; default 100). All packets are captured regardless.");
  doca_argp_param_set_callback(p, percent_cb); doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
  err = doca_argp_register_param(p); crash_if_unsuccessful(err, "register percent");
  err = doca_argp_param_create(&p); crash_if_unsuccessful(err, "param sample");
  doca_argp_param_set_long_name(p, "sample");
  doca_argp_param_set_description(p, "Write only ~1-in-N captured packets to the pcap (default 1 = every packet). Marking/forwarding are unaffected.");
  doca_argp_param_set_callback(p, sample_cb); doca_argp_param_set_type(p, DOCA_ARGP_TYPE_STRING);
  err = doca_argp_register_param(p); crash_if_unsuccessful(err, "register sample");
}

int main(int argc, char **argv) {
  doca_error_t err = doca_log_backend_create_standard(); crash_if_unsuccessful(err, "log_backend");
  struct doca_log_backend *sdk; err = doca_log_backend_create_with_file_sdk(stderr, &sdk); crash_if_unsuccessful(err, "sdk log");
  err = doca_log_backend_set_sdk_level(sdk, DOCA_LOG_LEVEL_WARNING); crash_if_unsuccessful(err, "sdk level");

  struct app_config cfg = {.pcap_path = NULL, .random_percent = 100.0, .sample_n = 1};
  err = doca_argp_init("doca_flow_ecn_pcap", &cfg); crash_if_unsuccessful(err, "argp_init");
  doca_argp_set_dpdk_program(initialize_dpdk); register_params();
  err = doca_argp_start(argc, argv); crash_if_unsuccessful(err, "argp_start");

  bool capture = (cfg.pcap_path != NULL);  /* --pcap given => also mirror wire copies to a pcap */
  pcap_t *pd = NULL; pcap_dumper_t *dumper = NULL;
  if (capture) {
    pd = pcap_open_dead(DLT_EN10MB, SNAPLEN);
    if (!pd) { DOCA_LOG_CRIT("pcap_open_dead"); return EXIT_FAILURE; }
    dumper = pcap_dump_open(pd, cfg.pcap_path);
    if (!dumper) { DOCA_LOG_CRIT("pcap_dump_open('%s'): %s", cfg.pcap_path, pcap_geterr(pd)); return EXIT_FAILURE; }
  }

  struct doca_dev *dev = open_and_probe_dev(0, "dv_flow_en=2,fdb_def_rule_en=1,repr_matching_en=0,representor=sf0");
  configure_and_start_dpdk_port(dev);
  initialize_doca_flow();
  struct doca_flow_port *port = port_start(dev);
  struct doca_flow_port *sf_rep = rep_port_start(1);
  uint16_t pf0; err = doca_dpdk_get_first_port_id(dev, &pf0); crash_if_unsuccessful(err, "pf0 id");

  if (capture) { struct doca_flow_pipe *rss = create_rss_pipe(port); setup_capture_mirror(port, rss); }
  struct doca_flow_pipe *passthrough = create_passthrough_pipe(port);

  /* PASS_CAPTURE (no mark) and MARK_CAPTURE (CE-mark); both mirror to pcap only when capturing. */
  struct doca_flow_pipe_entry *pass_e = NULL, *ce_e = NULL;
  struct doca_flow_pipe *pass_cap = create_capture_pipe(port, "PASS_CAPTURE", false, capture, passthrough, &pass_e);
  struct doca_flow_pipe *mark_cap = NULL;
  if (cfg.random_percent > 0.0)
    mark_cap = create_capture_pipe(port, "MARK_CAPTURE", true, capture, passthrough, &ce_e);

  /* wire-ingress entry point per --percent */
  uint16_t mask = 0;
  struct doca_flow_pipe *wire_target;
  if (cfg.random_percent >= 100.0)      wire_target = mark_cap;                 /* mark+capture all */
  else if (cfg.random_percent <= 0.0)   wire_target = pass_cap;                 /* capture all, mark none */
  else { mask = get_random_mask(cfg.random_percent);
         wire_target = create_random_sample_pipe(port, mark_cap, pass_cap, mask); }

  create_port_demux_pipe(port, wire_target);

  signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler);
  const char *dst = capture ? cfg.pcap_path : "(none — pure ECN-mark mode)";
  if (cfg.random_percent >= 100.0)      DOCA_LOG_INFO("Marking ALL IPv4 | capture: %s — Ctrl-C to stop", dst);
  else if (cfg.random_percent <= 0.0)   DOCA_LOG_INFO("Marking NONE     | capture: %s — Ctrl-C to stop", dst);
  else                                  DOCA_LOG_INFO("Marking ~%.4g%%   | capture: %s — Ctrl-C to stop", 100.0 / (mask + 1), dst);
  if (capture) {
    if (cfg.sample_n > 1) DOCA_LOG_INFO("Capturing ~1-in-%u packets to the pcap", cfg.sample_n);
    enable_key_toggle();
    DOCA_LOG_INFO("pcap writing starts PAUSED — press SPACE (or 'c'/'p') to start/stop writing to '%s'", cfg.pcap_path);
  }

  struct rte_mbuf *bufs[RX_BURST];
  uint64_t written = 0, mirrored = 0, sample_ctr = 0; time_t last = time(NULL);
  while (g_running) {
    uint16_t nb = 0;
    if (capture) {
      nb = rte_eth_rx_burst(pf0, 0, bufs, RX_BURST);
      for (uint16_t i = 0; i < nb; i++) {
        mirrored++;
        bool take = (++sample_ctr % cfg.sample_n == 0);   /* ~1-in-N (N==1 => every packet) */
        if (g_capture_writing && take) {
          struct pcap_pkthdr h; struct timeval tv; gettimeofday(&tv, NULL);
          h.ts = tv; h.caplen = rte_pktmbuf_data_len(bufs[i]); h.len = rte_pktmbuf_pkt_len(bufs[i]);
          pcap_dump((u_char *)dumper, &h, rte_pktmbuf_mtod(bufs[i], const u_char *));
          written++;
        }
        rte_pktmbuf_free(bufs[i]);
      }
    }
    if (nb == 0) usleep(200);
    poll_key_toggle();
    time_t now = time(NULL);
    if (now != last) {
      last = now;
      if (capture) pcap_dump_flush(dumper);
      uint64_t ce = query_pkts(ce_e), pass = query_pkts(pass_e), tot = ce + pass;
      if (capture)
        DOCA_LOG_INFO("CE marked: %lu, passthrough: %lu (%.4g%% marked) | mirrored: %lu -> pcap: %lu%s",
                      ce, pass, tot ? 100.0 * (double)ce / (double)tot : 0.0, mirrored, written,
                      g_capture_writing ? "" : " [PAUSED]");
      else
        DOCA_LOG_INFO("CE marked: %lu, passthrough: %lu (%.4g%% marked)",
                      ce, pass, tot ? 100.0 * (double)ce / (double)tot : 0.0);
    }
  }

  restore_key_toggle();
  if (capture) {
    pcap_dump_flush(dumper); pcap_dump_close(dumper); pcap_close(pd);
    DOCA_LOG_INFO("Wrote %lu packets to %s", written, cfg.pcap_path);
  }
  doca_flow_port_stop(sf_rep); doca_flow_port_stop(port); doca_flow_destroy(); doca_argp_destroy();
  return EXIT_SUCCESS;
}
