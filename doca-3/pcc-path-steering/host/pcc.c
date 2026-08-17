/*
 * Copyright (c) 2022-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice, this list of
 *       conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
 *       to endorse or promote products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdlib.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "steer.h"

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_log.h>
#include <doca_pcc.h>

#include "pcc_core.h"

static const char *status_str[DOCA_PCC_PS_ERROR + 1] = {"Active", "Standby", "Deactivated", "Error"};
static volatile sig_atomic_t host_stop;
int log_level;
static volatile sig_atomic_t got_debug_sig;

/*
 * Signal sigusr1 handler
 *
 * @signum [in]: signal number
 */
static void siguser1_handler(int signum)
{
	if (signum == SIGUSR1) {
		got_debug_sig = true;
	}
}

/*
 * Signal sigint handler
 *
 * @dummy [in]: Dummy parameter because this handler must accept parameter of type int
 */
static void sigint_handler(int dummy)
{
	(void)dummy;
	host_stop = true;
}

/*
 * Start the embedded DOCA Flow path-steering datapath in this process, in the
 * EGRESS role: this program runs on the sender (the PCC RP), so it sets the
 * DSCP path marker according to the PCC-computed random share at SF-egress. The receiver-side
 * ingress role (CE-mark + restore) runs as a separate doca_flow_steer instance
 * on the receiver's PF. EAL is initialized with a minimal argv; the device and
 * SF representor are opened by DOCA argp (-a/-r) and passed in via cfg.
 */
static doca_error_t start_embedded_steering(char *prog_name, const struct pcc_config *cfg,
                                                   struct doca_dev *pcc_dev)
{
	struct steer_opts sopts;
	doca_error_t result;

	(void)prog_name; /* used only on the DOCA 2.x EAL-init path below */
#if DOCA_HAS_DEVICE_REPRESENTORS
	(void)pcc_dev;
#endif
	steer_default_opts(&sopts);
	sopts.role = STEER_ROLE_EGRESS;
	sopts.sf_num = cfg->steer_sf_num;
	sopts.force_path = cfg->steer_force_path_set ? cfg->steer_force_path : -1;
	for (int path = 0; path < STEER_NB_PATHS; path++) {
		sopts.path_ip[path] = cfg->steer_path_ip[path];
		sopts.path_ip_set[path] = cfg->steer_path_ip_set[path];
	}
#if DOCA_HAS_DEVICE_REPRESENTORS
	sopts.dev = cfg->steer_dev;
	sopts.dev_rep = cfg->steer_dev_rep;
	sopts.devargs = cfg->steer_devargs;
	if (sopts.dev == NULL || sopts.dev_rep == NULL) {
		PRINT_ERROR("Error: embedded steering needs a sender SF representor; pass -r pci/<bdf>,<sf>\n");
		return DOCA_ERROR_INVALID_VALUE;
	}
	/* EAL was already initialized in main() (before argp opened the -r device). */
#else
	sopts.dev = pcc_dev;
	memcpy(sopts.device_pci_addr, cfg->steer_pci_addr, sizeof(sopts.device_pci_addr));
	char *eal_argv[1] = {prog_name};

	result = steer_eal_init(1, eal_argv, steer_eal_prefix_for_role(sopts.role));
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: steer EAL init failed: %s\n", doca_error_get_descr(result));
		return result;
	}
#endif
	result = steer_start(&sopts);
	if (result != DOCA_SUCCESS)
		PRINT_ERROR("Error: steer_start failed: %s\n", doca_error_get_descr(result));
	return result;
}

/*
 * Application main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct pcc_config cfg = {0};
	struct pcc_resources resources = {0};
	doca_pcc_process_state_t process_status;
	doca_error_t result, tmp_result;
	int exit_status = EXIT_FAILURE;
	bool pcc_started = false;
	bool steering_started = false;
#if DOCA_HAS_PCC_DEBUG_API
	bool enable_debug = false;
#endif
	struct doca_log_backend *sdk_log;

	/* Set the default configuration values (Example values) */
	cfg.wait_time = -1;
	cfg.app = pcc_rp_rtt_template_app;
	memcpy(cfg.threads_list, default_pcc_rp_threads_list, sizeof(default_pcc_rp_threads_list));
	cfg.threads_num = PCC_RP_THREADS_NUM_DEFAULT_VALUE;
	cfg.remote_sw_handler = false;
	strcpy(cfg.coredump_file, PCC_COREDUMP_FILE_DEFAULT_PATH);
	log_level = LOG_LEVEL_INFO;

	/* Add SIGINT signal handler for graceful exit */
	if (signal(SIGINT, sigint_handler) == SIG_ERR) {
		PRINT_ERROR("Error: SIGINT error\n");
		return DOCA_ERROR_OPERATING_SYSTEM;
	}
	/* Add SIGUSR1 signal handler for printing debug info */
	if (signal(SIGUSR1, siguser1_handler) == SIG_ERR) {
		PRINT_ERROR("Error: SIGUSR1 error\n");
		return DOCA_ERROR_OPERATING_SYSTEM;
	}

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_ERROR);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/*
	 * If embedded steering is requested (-r/--steer-rep), EAL must be up BEFORE
	 * DOCA argp opens that device/representor, otherwise the later
	 * doca_dpdk_port_probe_with_representors() fails with a "cmd_fd mismatch /
	 * Probe again" error. Pre-scan argv and init EAL here; start_embedded_steering()
	 * then only builds the pipeline. (Embedded steering is always the egress role.)
	 */
#if DOCA_HAS_DEVICE_REPRESENTORS
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-r") != 0 && strcmp(argv[i], "--steer-rep") != 0)
			continue;
		char *eal_argv[1] = {argv[0]};

		result = steer_eal_init(1, eal_argv, steer_eal_prefix_for_role(STEER_ROLE_EGRESS));
		if (result != DOCA_SUCCESS) {
			PRINT_ERROR("Error: steer EAL init failed: %s\n", doca_error_get_descr(result));
			return EXIT_FAILURE;
		}
		break;
	}
#endif

	/* Initialize argparser */
	result = doca_argp_init("doca_pcc", &cfg);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to init ARGP resources: %s\n", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	/* Register DOCA PCC application params */
	result = register_pcc_params();
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register parameters: %s\n", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* Start argparser */
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to parse input: %s\n", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* Get the log level */
	result = doca_argp_get_log_level(&log_level);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to get log level: %s\n", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* Initialize DOCA PCC application resources */
	result = pcc_init(&cfg, &resources);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to initialize PCC resources: %s\n", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	PRINT_INFO("Info: Welcome to DOCA Programmable Congestion Control (PCC) application\n");
	PRINT_INFO("Info: Starting DOCA PCC\n");

	/* Start DOCA PCC */
	result = doca_pcc_start(resources.doca_pcc);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to start PCC\n");
		goto destroy_pcc;
	}
	pcc_started = true;

	if (cfg.steer_enable) {
		result = start_embedded_steering(argv[0], &cfg, resources.doca_device);
		if (result != DOCA_SUCCESS)
			goto destroy_pcc;
		steering_started = true;
		PRINT_INFO("Info: Embedded DOCA Flow egress steering active\n");
	}

	host_stop = false;
	PRINT_INFO("Info: Press ctrl + C to exit\n");
	while (!host_stop) {
#if DOCA_HAS_PCC_DEBUG_API
		if (got_debug_sig) {
			if (enable_debug == false) {
				enable_debug = true;
				result = doca_pcc_enable_debug(resources.doca_pcc, enable_debug);
				if (result != DOCA_SUCCESS) {
					PRINT_ERROR("Error: failed to enable debug\n");
				}
			} else {
				result = doca_pcc_dump_debug(resources.doca_pcc);
				if (result != DOCA_SUCCESS) {
					PRINT_ERROR("Error: failed to dump debug\n");
				}
			}
			got_debug_sig = 0;
		}
#else
		got_debug_sig = 0;
#endif
		result = doca_pcc_get_process_state(resources.doca_pcc, &process_status);
		if (result != DOCA_SUCCESS) {
			PRINT_ERROR("Error: Failed to query PCC\n");
			goto destroy_pcc;
		}

		PRINT_INFO("Info: PCC host status %s\n", status_str[process_status]);

		if (process_status == DOCA_PCC_PS_DEACTIVATED || process_status == DOCA_PCC_PS_ERROR)
			break;

		if (cfg.steer_enable) {
			/* Drive the embedded steering decision + counters ~1/s. */
			tmp_result = pcc_poll_rate_reports(&resources);
			if (tmp_result != DOCA_SUCCESS)
				PRINT_WARNING("Warning: failed to poll PCC rate reports: %s\n",
				              doca_error_get_descr(tmp_result));
			steer_poll();
			for (uint32_t i = 0; i < 100 && !host_stop; i++) {
				steer_poll_rx();
				usleep(10000);
			}
		} else {
			PRINT_INFO("Info: Waiting on DOCA PCC\n");
			result = doca_pcc_wait(resources.doca_pcc, cfg.wait_time);
			if (result != DOCA_SUCCESS) {
				PRINT_ERROR("Error: Failed to wait PCC\n");
				goto destroy_pcc;
			}
		}
	}

	PRINT_INFO("Info: Finished waiting on DOCA PCC\n");

	exit_status = EXIT_SUCCESS;

destroy_pcc:
	/* Quiesce PCC before destroying Flow objects referenced by its asynchronous
	 * callbacks, then release steering before closing the shared DOCA device. */
	if (pcc_started) {
		tmp_result = doca_pcc_stop(resources.doca_pcc);
		if (tmp_result != DOCA_SUCCESS) {
			PRINT_ERROR("Error: Failed to stop DOCA PCC before steering cleanup: %s\n",
			            doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
			exit_status = EXIT_FAILURE;
		}
		pcc_started = false;
	}
	if (steering_started) {
		steer_stop();
		steering_started = false;
	}
	tmp_result = pcc_destroy(&resources);
	if (tmp_result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to destroy DOCA PCC application resources: %s\n",
			    doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		exit_status = EXIT_FAILURE;
	}
argp_cleanup:
	tmp_result = doca_argp_destroy();
	if (tmp_result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to destroy ARGP: %s\n", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		exit_status = EXIT_FAILURE;
	}
	return exit_status;
}
