/*
 * Embeddable PCC-informed RoCE path-steering module (BF3 eSwitch, DOCA Flow).
 *
 * This is the "switching" component: it builds and owns the DOCA Flow pipeline
 * (see README.md) and exposes a small API so it can run either standalone
 * (doca_flow_steer) or embedded in the doca_pcc host process. In the embedded
 * case doca_pcc's PCC trace handler calls steer_update_pcc_rate() with each
 * per-QPN rate; RDMA-CM maps sender QPNs to destination-IP path groups and the
 * module adjusts the per-packet random share assigned to those paths.
 *
 * Lifecycle (embedded):
 *   steer_eal_init(argc, argv, steer_eal_prefix_for_role(role)); // before other EAL users
 *   steer_start(&opts);           // build the pipeline
 *   ... per PCC trace: steer_update_pcc_rate(qpn, rate);
 *   ... periodically:  steer_poll();   // apply decision + log counters
 *   steer_stop();
 */

#ifndef STEER_H_
#define STEER_H_

#include <doca_error.h>
#include <doca_dev.h>
#include <doca_flow.h> /* doca_be32_t */
#include <stdbool.h>
#include <stdint.h>

#define STEER_NB_PATHS 2

/*
 * Which half of the pipeline this instance builds. On a single BF3 with a
 * p0<->p1 DAC loopback the sender and receiver are different SFs (usually on
 * different PFs), so the two directions run as two separate programs:
 *
 *   STEER_ROLE_EGRESS  sender SF egress -> random-share DSCP path assignment ->
 *                       wire; returning ACK/CNP traffic is delivered to the SF.
 *   STEER_ROLE_INGRESS wire ingress -> DSCP path decision -> destination-IP ECN
 *                       marker -> matching receiver SF; SF egress goes to wire.
 *   STEER_ROLE_BOTH    both halves in one eSwitch instance (single-endpoint /
 *                       whole-eSwitch case).
 */
enum steer_role {
	STEER_ROLE_INGRESS = 0,
	STEER_ROLE_EGRESS = 1,
	STEER_ROLE_BOTH = 2,
};

struct steer_opts {
	uint32_t sf_num;			      /* path-0/sender SF parsed from -r on DOCA 2.x */
	uint32_t path1_sf_num;			      /* second receiver SF (DOCA 2.x ingress) */
	bool path1_sf_num_set;
	int role;				      /* enum steer_role */
	double path_percent[STEER_NB_PATHS];	      /* per-path CE-mark percentage [0,100] */
	char device_pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* DOCA 2.x PF parsed from -r */
	/* Device handle is supplied directly by embedded PCC on 2.x and by argp on 3.x. */
	struct doca_dev *dev;			      /* PF device */
	struct doca_dev_rep *dev_rep;		      /* SF representor */
	struct doca_dev_rep *dev_rep_path1;	      /* second receiver SF representor (ingress role) */
	uint32_t dev_rep_count;			      /* number of representors supplied through -r */
	uint32_t path_ip[STEER_NB_PATHS];	      /* receiver IPv4 addresses, network byte order */
	bool path_ip_set[STEER_NB_PATHS];
	int force_path;                           /* diagnostic: -1 dynamic; 3.x bypasses classifier for 0/1 */
	const char *devargs;			      /* optional probe devargs (default dv_flow_en=2,fdb_def_rule_en=1) */
};

/* Parse pci/<BDF>,pf<PF>sf<SF> representor syntax used by -r/-R. */
doca_error_t steer_parse_rep_spec(const char *spec,
                                  char pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE], uint32_t *sf_num);

/* Fill opts with defaults (100% CE on both paths). */
void steer_default_opts(struct steer_opts *opts);

/*
 * Initialize DPDK EAL for the steering datapath. `file_prefix` becomes the DPDK
 * --file-prefix so this primary process does not collide with another DPDK
 * primary on the host (e.g. the peer role's steering instance); pass
 * steer_eal_prefix_for_role(role). Call once, before any other EAL user in the
 * process.
 */
doca_error_t steer_eal_init(int argc, char **argv, const char *file_prefix);

/* A stable, role-specific DPDK --file-prefix ("pcc-egress" / "pcc-ingress" / "pcc-steer"). */
const char *steer_eal_prefix_for_role(int role);

/* Build the eSwitch pipeline. EAL must already be initialized. */
doca_error_t steer_start(const struct steer_opts *opts);

/*
 * Feed one per-QPN PCC rate (FXP20). Safe to call from the PCC trace handler.
 * RDMA-CM grouping associates the sender QPN with its destination-IP path.
 */
void steer_update_pcc_rate(uint32_t qpn, uint32_t rate);

/* Drain cloned packets without running the one-second control/statistics work. */
void steer_poll_rx(void);

/* Calculate/apply the PCC path share and log counters. */
void steer_poll(void);

/* Tear down the pipeline and DOCA Flow. */
void steer_stop(void);

#endif /* STEER_H_ */
