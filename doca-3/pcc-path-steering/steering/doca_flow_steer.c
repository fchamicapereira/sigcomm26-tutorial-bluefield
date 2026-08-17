/*
 * Standalone runner for the steering module (steer.c). Parses CLI, initializes
 * EAL via DOCA argp, starts the pipeline, and polls once per second. The same
 * steer_* API is used embedded in doca_pcc (see ../pcc), where the PCC trace
 * handler drives steer_update_pcc_rate() instead of this static CLI.
 */

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>
#include "pcc_doca_compat.h"
#include "steer.h"

#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

DOCA_LOG_REGISTER(FLOW_STEER_MAIN);

static volatile sig_atomic_t g_running = 1;

static void on_signal(int s)
{
	if (s == SIGINT || s == SIGTERM)
		g_running = 0;
}

#define CRASH(err, msg)                                                                    \
	do {                                                                               \
		doca_error_t _e = (err);                                                   \
		if (_e != DOCA_SUCCESS) {                                                  \
			DOCA_LOG_CRIT("%s: %s", (msg), doca_error_get_descr(_e));          \
			exit(EXIT_FAILURE);                                                \
		}                                                                          \
	} while (0)

#if DOCA_USES_LEGACY_FLOW_BACKEND
static doca_error_t legacy_rep_cb(void *param, void *config, uint32_t path)
{
	struct steer_opts *o = config;
	char pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE] = {0};
	uint32_t sf_num;
	doca_error_t err = steer_parse_rep_spec(param, pci_addr, &sf_num);

	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("representor must use pci/<BDF>,pf<N>sf<N> syntax");
		return err;
	}
	if (o->device_pci_addr[0] != '\0' && strcmp(o->device_pci_addr, pci_addr) != 0) {
		DOCA_LOG_ERR("path0 and path1 representors must belong to the same PF device");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (o->device_pci_addr[0] == '\0')
		memcpy(o->device_pci_addr, pci_addr, sizeof(o->device_pci_addr));
	if (path == 0)
		o->sf_num = sf_num;
	else {
		o->path1_sf_num = sf_num;
		o->path1_sf_num_set = true;
	}
	return DOCA_SUCCESS;
}

static doca_error_t legacy_path0_rep_cb(void *param, void *config)
{
	return legacy_rep_cb(param, config, 0);
}

static doca_error_t legacy_path1_rep_cb(void *param, void *config)
{
	return legacy_rep_cb(param, config, 1);
}
#endif

static doca_error_t role_cb(void *param, void *config)
{
	struct steer_opts *o = config;
	const char *s = (const char *)param;

	if (strcmp(s, "egress") == 0 || strcmp(s, "sender") == 0)
		o->role = STEER_ROLE_EGRESS;
	else if (strcmp(s, "ingress") == 0 || strcmp(s, "receiver") == 0)
		o->role = STEER_ROLE_INGRESS;
	else if (strcmp(s, "both") == 0)
		o->role = STEER_ROLE_BOTH;
	else {
		DOCA_LOG_ERR("--role must be egress|ingress|both (got '%s')", s);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

static doca_error_t p0pct_cb(void *param, void *config)
{
	struct steer_opts *o = config;
	double v = atof((const char *)param);

	if (v < 0.0 || v > 100.0)
		return DOCA_ERROR_INVALID_VALUE;
	o->path_percent[0] = v;
	return DOCA_SUCCESS;
}

static doca_error_t p1pct_cb(void *param, void *config)
{
	struct steer_opts *o = config;
	double v = atof((const char *)param);

	if (v < 0.0 || v > 100.0)
		return DOCA_ERROR_INVALID_VALUE;
	o->path_percent[1] = v;
	return DOCA_SUCCESS;
}

static doca_error_t parse_path_ip(void *param, void *config, unsigned int path)
{
	struct steer_opts *o = config;
	struct in_addr addr;

	if (inet_pton(AF_INET, (const char *)param, &addr) != 1) {
		DOCA_LOG_ERR("--path%u-ip requires a valid IPv4 address", path);
		return DOCA_ERROR_INVALID_VALUE;
	}
	o->path_ip[path] = addr.s_addr;
	o->path_ip_set[path] = true;
	return DOCA_SUCCESS;
}

static doca_error_t p0ip_cb(void *param, void *config)
{
	return parse_path_ip(param, config, 0);
}

static doca_error_t p1ip_cb(void *param, void *config)
{
	return parse_path_ip(param, config, 1);
}

#if DOCA_HAS_DEVICE_REPRESENTORS
static doca_error_t device_cb(void *param, void *config)
{
	struct steer_opts *o = config;
	struct doca_argp_device_ctx *dev_ctx = param;

	o->dev = dev_ctx->dev;
	if (dev_ctx->devargs)
		o->devargs = dev_ctx->devargs;
	return DOCA_SUCCESS;
}

static doca_error_t rep_cb(void *param, void *config)
{
	struct steer_opts *o = config;
	struct doca_argp_device_rep_ctx *rep_ctx = param;

	o->dev = rep_ctx->dev_ctx.dev;
	o->dev_rep = rep_ctx->dev_rep;
	o->dev_rep_count = 1 + (o->dev_rep_path1 != NULL);
	if (rep_ctx->dev_ctx.devargs)
		o->devargs = rep_ctx->dev_ctx.devargs;
	return DOCA_SUCCESS;
}

static doca_error_t path1_rep_cb(void *param, void *config)
{
	struct steer_opts *o = config;
	struct doca_argp_device_rep_ctx *rep_ctx = param;

	if (o->dev != NULL && o->dev != rep_ctx->dev_ctx.dev) {
		DOCA_LOG_ERR("path0 and path1 representors must belong to the same PF device");
		return DOCA_ERROR_INVALID_VALUE;
	}
	o->dev = rep_ctx->dev_ctx.dev;
	o->dev_rep_path1 = rep_ctx->dev_rep;
	o->dev_rep_count = 1 + (o->dev_rep != NULL);
	if (rep_ctx->dev_ctx.devargs)
		o->devargs = rep_ctx->dev_ctx.devargs;
	return DOCA_SUCCESS;
}

static void reg_dev(const char *shortn, const char *longn, const char *desc, doca_argp_param_cb_t cb, int type)
{
	struct doca_argp_param *pm;

	CRASH(doca_argp_param_create(&pm), "argp_param_create");
	doca_argp_param_set_short_name(pm, shortn);
	doca_argp_param_set_long_name(pm, longn);
	doca_argp_param_set_description(pm, desc);
	doca_argp_param_set_callback(pm, cb);
	doca_argp_param_set_type(pm, type);
	CRASH(doca_argp_register_param(pm), "argp_register_param");
}
#endif

static void reg_short(const char *short_name, const char *name, const char *desc, doca_argp_param_cb_t cb)
{
	struct doca_argp_param *pm;

	CRASH(doca_argp_param_create(&pm), "argp_param_create");
	if (short_name != NULL)
		doca_argp_param_set_short_name(pm, short_name);
	doca_argp_param_set_long_name(pm, name);
	doca_argp_param_set_description(pm, desc);
	doca_argp_param_set_callback(pm, cb);
	doca_argp_param_set_type(pm, DOCA_ARGP_TYPE_STRING);
	CRASH(doca_argp_register_param(pm), "doca_argp_register_param");
}

static void reg(const char *name, const char *desc, doca_argp_param_cb_t cb)
{
	reg_short(NULL, name, desc, cb);
}

static const char *g_eal_prefix = "pcc-steer";

int main(int argc, char **argv)
{
	struct doca_log_backend *sdk_log;

	CRASH(doca_log_backend_create_standard(), "doca_log_backend_create_standard");
	CRASH(doca_log_backend_create_with_file_sdk(stderr, &sdk_log), "doca_log_backend_create_with_file_sdk");
	doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);

	struct steer_opts opts;

	steer_default_opts(&opts);

	/*
	 * Pre-scan --role for the DPDK --file-prefix. EAL itself is brought up by
	 * doca_argp via doca_argp_set_dpdk_program(eal_cb) DURING doca_argp_start, so
	 * the -r device/representor is opened DPDK-aware and the later probe does not
	 * hit "cmd_fd mismatch / Probe again".
	 */
	int pre_role = STEER_ROLE_BOTH;

	for (int i = 1; i + 1 < argc; i++) {
		if (strcmp(argv[i], "--role") != 0)
			continue;
		const char *v = argv[i + 1];

		if (!strcmp(v, "egress") || !strcmp(v, "sender"))
			pre_role = STEER_ROLE_EGRESS;
		else if (!strcmp(v, "ingress") || !strcmp(v, "receiver"))
			pre_role = STEER_ROLE_INGRESS;
		else
			pre_role = STEER_ROLE_BOTH;
		break;
	}
	g_eal_prefix = steer_eal_prefix_for_role(pre_role);

	/* Init EAL independently (like doca_pcc) before argp opens the -r device.
	 * No doca_argp_set_dpdk_program, so no "--" split is needed on the CLI. */
	char *eal_argv[1] = {argv[0]};

	CRASH(steer_eal_init(1, eal_argv, g_eal_prefix), "steer_eal_init");

	CRASH(doca_argp_init("doca_flow_steer", &opts), "doca_argp_init");
	reg("path0-percent", "Path 0 intended all-traffic CE percent [0,100]; selected-class sampling is 2x, capped at 100. Default: 100", p0pct_cb);
	reg("path1-percent", "Path 1 intended all-traffic CE percent [0,100]; selected-class sampling is 2x, capped at 100. Default: 100", p1pct_cb);
	reg("path0-ip", "IPv4 address delivered to the first -r receiver SF", p0ip_cb);
	reg("path1-ip", "IPv4 address delivered to the second -r receiver SF", p1ip_cb);
	reg("role", "Which half to build: egress (sender) | ingress (receiver) | both. Default: both", role_cb);
#if DOCA_HAS_DEVICE_REPRESENTORS
	reg_dev("a", "device", "DOCA device, e.g. pci/0000:03:00.0,dv_flow_en=2", device_cb, DOCA_ARGP_TYPE_DEVICE);
	reg_dev("r", "path0-rep", "Path-0 SF representor, e.g. pci/0000:03:00.0,pf0sf0", rep_cb,
	        DOCA_ARGP_TYPE_DEVICE_REP);
	reg_dev("R", "path1-rep", "Path-1 SF representor, e.g. pci/0000:03:00.0,pf0sf4", path1_rep_cb,
	        DOCA_ARGP_TYPE_DEVICE_REP);
#else
	reg_short("r", "path0-rep", "Path-0 SF representor, e.g. pci/0000:03:00.0,pf0sf0",
	          legacy_path0_rep_cb);
	reg_short("R", "path1-rep", "Path-1 SF representor, e.g. pci/0000:03:00.0,pf0sf4",
	          legacy_path1_rep_cb);
#endif
	CRASH(doca_argp_start(argc, argv), "doca_argp_start");

#if DOCA_HAS_DEVICE_REPRESENTORS
	if (opts.dev == NULL || opts.dev_rep == NULL) {
		DOCA_LOG_CRIT("Specify the SF representor via -r (e.g. -r pci/0000:03:00.0,sf0,dv_flow_en=2)");
		return EXIT_FAILURE;
	}
	if (opts.role != STEER_ROLE_EGRESS &&
	    (opts.dev_rep_count != STEER_NB_PATHS || !opts.path_ip_set[0] || !opts.path_ip_set[1])) {
		DOCA_LOG_CRIT("ingress requires -r, -R/--path1-rep, --path0-ip and --path1-ip");
		return EXIT_FAILURE;
	}
#else
	if (opts.device_pci_addr[0] == '\0') {
		DOCA_LOG_CRIT("Specify the SF representor via -r (e.g. -r pci/0000:03:00.0,pf0sf0)");
		return EXIT_FAILURE;
	}
	if (opts.role != STEER_ROLE_EGRESS && !opts.path1_sf_num_set) {
		DOCA_LOG_CRIT("DOCA 2.x ingress requires -r and -R/--path1-rep");
		return EXIT_FAILURE;
	}
#endif

	CRASH(steer_start(&opts), "steer_start");

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	DOCA_LOG_INFO("doca_flow_steer running -- Ctrl-C to stop");

	while (g_running) {
		for (uint32_t i = 0; i < 100 && g_running; i++) {
			steer_poll_rx();
			usleep(10000);
		}
		if (g_running)
			steer_poll();
	}

	DOCA_LOG_INFO("stopping steering");
	steer_stop();
	DOCA_LOG_INFO("steering stopped; destroying DOCA argp resources");
	doca_argp_destroy();
	DOCA_LOG_INFO("DOCA argp resources destroyed");
	return EXIT_SUCCESS;
}
