/*
 * Copyright (c) 2022-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <ctype.h>
#include <arpa/inet.h>

#include "steer.h"

#include <doca_argp.h>
#include <doca_version.h>

#include "pcc_core.h"
#include "../device/pcc_rate_report.h"

/*
 * Formats of the trace message to be printed from the device
 */
static char *trace_message_formats[] = {
	"format 0 - user init: port num = %#lx, algo index = %#lx, algo slot = %#lx, algo enable = %#lx, disable event bitmask = %#lx\n",
	"format 1 - user algo: algo slot = %#lx, result rate = %#lx, result rtt req = %#lx, port num = %#lx, timestamp = %#lx\n",
	"unused format 2\n",
	"unused format 3\n",
	"unused format 4\n",
	"unused format 5\n",
	"RATE_REPORT: qpn=%#lx rate=%#lx ev_type=%#lx rtt=%#lx ts=%#lx\n",
	NULL};

/*
 * Per-flow rate tracking for rate reports from device
 */
#define MAX_TRACKED_FLOWS 256

struct flow_rate_entry {
	uint32_t qpn;
	uint64_t rate_sum;
	uint32_t count;
	uint32_t last_rate;
};

static struct flow_rate_entry flow_rate_table[MAX_TRACKED_FLOWS];
static uint32_t flow_rate_table_size = 0;
#if DOCA_HAS_PCC_TRACE_REPORTS
static uint32_t last_rate_print_ts = 0;
#endif
static uint64_t rate_reports_total = 0;
static uint64_t rate_reports_since_print = 0;

static struct flow_rate_entry *find_or_create_flow(uint32_t qpn)
{
	for (uint32_t i = 0; i < flow_rate_table_size; i++) {
		if (flow_rate_table[i].qpn == qpn)
			return &flow_rate_table[i];
	}
	if (flow_rate_table_size < MAX_TRACKED_FLOWS) {
		struct flow_rate_entry *entry = &flow_rate_table[flow_rate_table_size++];
		entry->qpn = qpn;
		entry->rate_sum = 0;
		entry->count = 0;
		entry->last_rate = 0;
		return entry;
	}
	return NULL;
}

#if DOCA_HAS_PCC_TRACE_REPORTS
/* doca_pcc_bin_report is intentionally opaque and changed layout while keeping
 * its 64-byte size. DOCA 3.1 uses the legacy FlexIO report (args at byte 16),
 * while DOCA 3.4 inserts an internal timestamp (args at byte 24). */
#if DOCA_HAS_TIMESTAMPED_PCC_TRACE_LAYOUT
struct pcc_trace_report {
	uint32_t msg_number;
	uint32_t seq_number;
	uint64_t metadata;
	uint64_t internal_timestamp;
	uint64_t args[5];
};
#else
struct pcc_trace_report {
	uint32_t msg_number;
	uint32_t seq_number;
	uint64_t thread_and_timestamp;
	uint64_t args[6];
};
#endif

_Static_assert(sizeof(struct pcc_trace_report) == 64, "unexpected PCC trace report size");

/*
 * Trace handler callback - receives binary trace reports from the DPA.
 * Filters for rate report format and updates per-flow average rate.
 */
static int rate_report_trace_handler(void *ctx, struct doca_pcc_bin_report *reps, int reps_size)
{
	struct pcc_trace_report *reports = (struct pcc_trace_report *)reps;
	uint32_t latest_report_ts = 0;
	bool received_rate_report = false;

	(void)ctx;
	if (reps_size <= 0)
		return 0;

	for (int i = 0; i < reps_size; i++) {
		uint32_t qpn;
		uint32_t rate;
		struct flow_rate_entry *entry;

		if (reports[i].msg_number != PCC_RATE_REPORT_FORMAT_ID)
			continue;
		qpn = (uint32_t)reports[i].args[0];
		rate = (uint32_t)reports[i].args[1];
		entry = find_or_create_flow(qpn);
		if (entry != NULL) {
			entry->rate_sum += rate;
			entry->count++;
			entry->last_rate = rate;
		}
		steer_update_pcc_rate(qpn, rate);
		latest_report_ts = (uint32_t)reports[i].args[4];
		received_rate_report = true;
		rate_reports_total++;
		rate_reports_since_print++;
	}

	/* Keep the existing host-side once-per-second per-QPN report. */
	if (received_rate_report &&
	    (last_rate_print_ts == 0 || (uint32_t)(latest_report_ts - last_rate_print_ts) >= 1000000)) {
		printf("--- Per-flow rate averages (received=%llu total=%llu) ---\n",
		       (unsigned long long)rate_reports_since_print,
		       (unsigned long long)rate_reports_total);
		for (uint32_t i = 0; i < flow_rate_table_size; i++) {
			const struct flow_rate_entry *entry = &flow_rate_table[i];
			uint32_t avg = (uint32_t)(entry->rate_sum / entry->count);

			printf("  QPN 0x%x: avg_rate=%u last_rate=%u updates=%u\n",
			       entry->qpn, avg, entry->last_rate, entry->count);
		}
		printf("---\n");
		fflush(stdout);
		last_rate_print_ts = latest_report_ts;
		rate_reports_since_print = 0;
	}
	return 0;
}

#endif /* DOCA_HAS_PCC_TRACE_REPORTS */

/* Default PCC RP threads */
const uint32_t default_pcc_rp_threads_list[PCC_RP_THREADS_NUM_DEFAULT_VALUE] = {
	176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 192, 193, 194, 195, 196,
	197, 198, 199, 200, 201, 202, 203, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217,
	218, 219, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 240};
/*
 * Declare threads list flag
 */
static bool use_threads_list = false;

/*
 * Declare DPA resources flag
 */
static bool use_dpa_resources = false;

/*
 * Declare application key flag
 */
static bool use_dpa_application_key = false;

#if DOCA_HAS_DPA_RESOURCES_FILE
/**
 * @brief Get the size of a file
 *
 * @param[in] path - Path to the file
 * @param[out] file_size - Size of the file in bytes
 * @return DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_file_size(const char *path, size_t *file_size)
{
	FILE *file;
	long nb_file_bytes;

	file = fopen(path, "rb");
	if (file == NULL)
		return DOCA_ERROR_NOT_FOUND;

	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return DOCA_ERROR_IO_FAILED;
	}

	nb_file_bytes = ftell(file);
	fclose(file);

	if (nb_file_bytes == -1)
		return DOCA_ERROR_IO_FAILED;

	if (nb_file_bytes == 0)
		return DOCA_ERROR_INVALID_VALUE;

	*file_size = (size_t)nb_file_bytes;
	return DOCA_SUCCESS;
}

/**
 * @brief Read file content into a pre-allocated buffer
 *
 * @param[in] path - Path to the file
 * @param[out] buffer - Pre-allocated buffer to store file content
 * @param[out] bytes_read - Number of bytes read from the file
 * @return DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t read_file_into_buffer(const char *path, char *buffer, size_t *bytes_read)
{
	FILE *file;
	size_t read_byte_count;

	file = fopen(path, "rb");
	if (file == NULL)
		return DOCA_ERROR_NOT_FOUND;

	read_byte_count = fread(buffer, 1, *bytes_read, file);
	fclose(file);

	if (read_byte_count != *bytes_read)
		return DOCA_ERROR_IO_FAILED;

	*bytes_read = read_byte_count;
	return DOCA_SUCCESS;
}
#endif

/*
 * Check if the provided device name is a name of a valid IB device
 *
 * @device_name [in]: The wanted IB device name
 * @return: True if device_name is an IB device, false otherwise.
 */
static bool pcc_device_exists_check(const char *device_name)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs = 0;
	doca_error_t result;
	bool exists = false;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	uint32_t i = 0;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to load DOCA devices list: %s\n", doca_error_get_descr(result));
		return false;
	}

	/* Search device with same device name */
	for (i = 0; i < nb_devs; i++) {
		result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (result != DOCA_SUCCESS)
			continue;

		/* Check if we found the device with the wanted name */
		if (strncmp(device_name, ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE) == 0) {
			exists = true;
			break;
		}
	}

	doca_devinfo_destroy_list(dev_list);

	return exists;
}

/*
 * Open DOCA device that supports PCC
 *
 * @device_name [in]: Requested IB device name
 * @role [in]: Role of the PCC context
 * @doca_device [out]: An allocated DOCA device that supports PCC on success and NULL otherwise
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t open_pcc_device(const char *device_name, struct doca_dev **doca_device)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs = 0;
	doca_error_t result;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	uint32_t i = 0;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to load DOCA devices list: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Search device with same device name and PCC capabilities supported */
	for (i = 0; i < nb_devs; i++) {
		result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (result != DOCA_SUCCESS) {
			PRINT_ERROR("Error: could not get DOCA device name\n");
			continue;
		}

		/* Check if the device has the requested device name */
		if (strncmp(device_name, ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE) != 0)
			continue;

		result = doca_devinfo_get_is_pcc_supported(dev_list[i]);
		if (result != DOCA_SUCCESS) {
			doca_devinfo_destroy_list(dev_list);
			PRINT_ERROR("Error: DOCA device %s does not support PCC RP\n", device_name);
			return result;
		}

		result = doca_dev_open(dev_list[i], doca_device);
		if (result != DOCA_SUCCESS) {
			doca_devinfo_destroy_list(dev_list);
			PRINT_ERROR("Error: Failed to open DOCA device: %s\n", doca_error_get_descr(result));
			return result;
		}
		break;
	}

	doca_devinfo_destroy_list(dev_list);

	if (*doca_device == NULL) {
		PRINT_ERROR("Error: Couldn't get DOCA device %s\n", device_name);
		return DOCA_ERROR_NOT_FOUND;
	}

	return result;
}

static doca_error_t create_dpa_resources(struct pcc_config *cfg)
{
#if DOCA_HAS_DPA_RESOURCES_FILE
	char *file_buffer;
	size_t bytes_read;
	struct doca_pcc_resources *doca_pcc_resources;
	doca_error_t status;
	const char *app_key = cfg->dpa_application_key;
	uint32_t num_eus;

	/* Get the file size first */
	status = get_file_size(cfg->dpa_resources_file, &bytes_read);
	if (status != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to get DPA resources file size: %s\n", doca_error_get_descr(status));
		return status;
	}

	/* Allocate buffer based on file size */
	file_buffer = (char *)malloc(bytes_read);
	if (file_buffer == NULL) {
		PRINT_ERROR("Error: Failed to allocate memory for DPA resources file\n");
		return DOCA_ERROR_NO_MEMORY;
	}

	/* Read the DPA resources file */
	status = read_file_into_buffer(cfg->dpa_resources_file, file_buffer, &bytes_read);
	if (status != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to open DPA resources file: %s\n", doca_error_get_descr(status));
		free(file_buffer);
		return status;
	}

	status = doca_pcc_resources_create(app_key, file_buffer, bytes_read, &doca_pcc_resources);
	if (status != DOCA_SUCCESS) {
		PRINT_ERROR("Failed to create DPA resources: %s", doca_error_get_descr(status));
		free(file_buffer);
		return status;
	}

	status = doca_pcc_resources_get_num_eus(doca_pcc_resources, &num_eus);
	if (status != DOCA_SUCCESS) {
		PRINT_ERROR("Failed to get number of execution units: %s", doca_error_get_descr(status));
		doca_pcc_resources_destroy(doca_pcc_resources);
		free(file_buffer);
		return status;
	}

	uint32_t eus[num_eus];
	status = doca_pcc_resources_get_eus(doca_pcc_resources, num_eus, eus);
	if (status != DOCA_SUCCESS) {
		PRINT_ERROR("Failed to get execution units: %s", doca_error_get_descr(status));
		doca_pcc_resources_destroy(doca_pcc_resources);
		free(file_buffer);
		return status;
	}

	/* Print information about the execution units */
	PRINT_DEBUG("Debug: Found %d execution units in DPA resources file\n", num_eus);

	for (uint32_t i = 0; i < num_eus; i++) {
		cfg->threads_list[i] = eus[i];
	}
	cfg->threads_num = num_eus;

	status = doca_pcc_resources_destroy(doca_pcc_resources);
	if (status != DOCA_SUCCESS) {
		PRINT_ERROR("Failed to destroy DPA resources: %s", doca_error_get_descr(status));
		free(file_buffer);
		return status;
	}
	free(file_buffer);

	return DOCA_SUCCESS;
#else
	(void)cfg;
	PRINT_ERROR("Error: DPA resources-file parsing requires DOCA 3.4 or newer; use --threads on this SDK\n");
	return DOCA_ERROR_NOT_SUPPORTED;
#endif
}

doca_error_t pcc_init(struct pcc_config *cfg, struct pcc_resources *resources)
{
	doca_error_t result, tmp_result;
	uint32_t min_num_threads, max_num_threads;

	/* Check if both threads list and DPA resources are specified */
	if (use_dpa_resources && use_threads_list) {
		PRINT_ERROR(
			"Error: Cannot specify both threads list and DPA resources. Use either threads list or DPA resources (with application key).\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* If DPA resources are specified, read the DPA resources file */
	if (use_dpa_resources) {
		if (!use_dpa_application_key) {
			PRINT_ERROR("Error: when using DPA resources file, DPA application key must be provided\n");
			return DOCA_ERROR_INVALID_VALUE;
		}
		result = create_dpa_resources(cfg);
		if (result != DOCA_SUCCESS) {
			PRINT_ERROR("Failed to create DPA resources: %s\n", doca_error_get_descr(result));
			return result;
		}
	}

	/* Open DOCA device that supports PCC */
	result = open_pcc_device(cfg->device_name, &(resources->doca_device));
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to open DOCA device that supports PCC\n");
		return result;
	}

	/* Create DOCA PCC context */
	bool use_default_threads = !use_threads_list && !use_dpa_resources;
	result = doca_pcc_create(resources->doca_device, &(resources->doca_pcc));
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create DOCA PCC context\n");
		goto close_doca_dev;
	}

	/* Define default RP threads if not explicitly configured. */
	if (use_default_threads) {
		memcpy(cfg->threads_list, default_pcc_rp_threads_list, sizeof(default_pcc_rp_threads_list));
		cfg->threads_num = PCC_RP_THREADS_NUM_DEFAULT_VALUE;
	}

	result = doca_pcc_get_min_num_threads(resources->doca_pcc, &min_num_threads);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Failed to get minimum DOCA PCC number of threads\n");
		goto destroy_pcc;
	}

	result = doca_pcc_get_max_num_threads(resources->doca_pcc, &max_num_threads);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Failed to get maximum DOCA PCC number of threads\n");
		goto destroy_pcc;
	}

	if (cfg->threads_num < min_num_threads || cfg->threads_num > max_num_threads) {
		PRINT_ERROR(
			"Invalid number of PCC threads: %u. The Minimum number of PCC threads is %d and the maximum number of PCC threads is %d\n",
			cfg->threads_num,
			min_num_threads,
			max_num_threads);
		result = DOCA_ERROR_INVALID_VALUE;
		goto destroy_pcc;
	}

	result = doca_pcc_set_app(resources->doca_pcc, cfg->app);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to set DOCA PCC app\n");
		goto destroy_pcc;
	}

	/* Set DOCA PCC thread affinity */
	result = doca_pcc_set_thread_affinity(resources->doca_pcc, cfg->threads_num, cfg->threads_list);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to set thread affinity for DOCA PCC\n");
		goto destroy_pcc;
	}

	/* This RP application uses CCMAD probes only. */
	result = doca_pcc_set_ccmad_probe_packet_format(resources->doca_pcc, 0);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to set CCMAD probe packet format for DOCA PCC\n");
		goto destroy_pcc;
	}
	result = doca_pcc_rp_set_ccmad_remote_sw_handler(resources->doca_pcc, 0, cfg->remote_sw_handler);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to set CCMAD remote SW handler for DOCA PCC\n");
		goto destroy_pcc;
	}

	/* Set DOCA PCC print buffer size */
	result = doca_pcc_set_print_buffer_size(resources->doca_pcc, PCC_PRINT_BUFFER_SIZE_DEFAULT_VALUE);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to set print buffer size for DOCA PCC\n");
		goto destroy_pcc;
	}

	/* Set DOCA PCC trace message formats */
	result = doca_pcc_set_trace_message(resources->doca_pcc, trace_message_formats);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to set trace message for DOCA PCC\n");
		goto destroy_pcc;
	}

#if DOCA_HAS_PCC_TRACE_REPORTS
	/* Register trace handler for per-flow rate reports */
	result = doca_pcc_register_trace_handler(resources->doca_pcc, rate_report_trace_handler, NULL);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register trace handler for rate reports\n");
		goto destroy_pcc;
	}
#endif

#if !DOCA_HAS_PCC_TRACE_REPORTS
	result = doca_pcc_set_mailbox(resources->doca_pcc,
	                              sizeof(struct pcc_rate_mailbox_request),
	                              sizeof(struct pcc_rate_mailbox_response));
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to configure PCC rate mailbox\n");
		goto destroy_pcc;
	}
#endif

	/* Set DOCA PCC coredump file pathname */
	result = doca_pcc_set_dev_coredump_file(resources->doca_pcc, cfg->coredump_file);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to set coredump file for DOCA PCC\n");
		goto destroy_pcc;
	}

	return result;

destroy_pcc:
	tmp_result = doca_pcc_destroy(resources->doca_pcc);
	if (tmp_result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to destroy DOCA PCC context: %s\n", doca_error_get_descr(result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
close_doca_dev:
	tmp_result = doca_dev_close(resources->doca_device);
	if (tmp_result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to close DOCA device: %s\n", doca_error_get_descr(result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

doca_error_t pcc_poll_rate_reports(struct pcc_resources *resources)
{
#if !DOCA_HAS_PCC_TRACE_REPORTS
	struct pcc_rate_mailbox_request *request;
	struct pcc_rate_mailbox_response *response;
	uint32_t response_size = 0;
	uint32_t cb_ret = 0;
	doca_error_t result;

	result = doca_pcc_mailbox_get_request_buffer(resources->doca_pcc, (void **)&request);
	if (result != DOCA_SUCCESS)
		return result;
	request->version = PCC_RATE_MAILBOX_VERSION;
	result = doca_pcc_mailbox_send(resources->doca_pcc, sizeof(*request), &response_size, &cb_ret);
	if (result != DOCA_SUCCESS)
		return result;
	if (cb_ret != 0 || response_size != sizeof(*response))
		return DOCA_ERROR_BAD_STATE;
	result = doca_pcc_mailbox_get_response_buffer(resources->doca_pcc, (void **)&response);
	if (result != DOCA_SUCCESS)
		return result;
	if (response->version != PCC_RATE_MAILBOX_VERSION ||
	    response->count > PCC_RATE_MAILBOX_MAX_FLOWS)
		return DOCA_ERROR_BAD_STATE;

	for (uint32_t i = 0; i < response->count; i++) {
		struct flow_rate_entry *entry = find_or_create_flow(response->flow[i].qpn);

		if (entry != NULL) {
			entry->rate_sum += response->flow[i].rate;
			entry->count++;
			entry->last_rate = response->flow[i].rate;
		}
		steer_update_pcc_rate(response->flow[i].qpn, response->flow[i].rate);
	}
	rate_reports_since_print += response->count;
	rate_reports_total += response->count;
	printf("--- Per-flow rate averages (received=%llu total=%llu) ---\n",
	       (unsigned long long)rate_reports_since_print,
	       (unsigned long long)rate_reports_total);
	for (uint32_t i = 0; i < flow_rate_table_size; i++) {
		const struct flow_rate_entry *entry = &flow_rate_table[i];
		uint32_t avg = (uint32_t)(entry->rate_sum / entry->count);

		printf("  QPN 0x%x: avg_rate=%u last_rate=%u updates=%u\n",
		       entry->qpn, avg, entry->last_rate, entry->count);
	}
	printf("---\n");
	fflush(stdout);
	rate_reports_since_print = 0;
#else
	(void)resources;
#endif
	return DOCA_SUCCESS;
}

doca_error_t pcc_destroy(struct pcc_resources *resources)
{
	doca_error_t result, tmp_result;

	result = doca_pcc_destroy(resources->doca_pcc);
	if (result != DOCA_SUCCESS)
		PRINT_ERROR("Error: Failed to destroy DOCA PCC context: %s\n", doca_error_get_descr(result));

	tmp_result = doca_dev_close(resources->doca_device);
	if (tmp_result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to close DOCA device: %s\n", doca_error_get_descr(result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

/*
 * ARGP Callback - Handle IB device name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t device_name_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	char *device_name = (char *)param;
	int len;

	len = strnlen(device_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		PRINT_ERROR("Error: Entered IB device name exceeding the maximum size of %d\n",
			    DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(pcc_cfg->device_name, device_name, len + 1);

	if (!pcc_device_exists_check(pcc_cfg->device_name)) {
		PRINT_ERROR("Error: Entered IB device name: %s doesn't exist\n", pcc_cfg->device_name);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCC threads list parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t threads_list_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	char *threads_list_string = (char *)param;
	static const char delim[2] = " ";
	char *curr_pcc_string;
	int curr_pcc_check, i, len;
	uint32_t curr_pcc;

	len = strnlen(threads_list_string, MAX_ARG_SIZE);
	if (len == MAX_ARG_SIZE) {
		PRINT_ERROR("Error: Entered PCC threads list exceeded buffer size: %d\n", MAX_USER_ARG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	use_threads_list = true;
	pcc_cfg->threads_num = 0;

	/* Check and fill out the PCC threads list */
	/* Get the first PCC thread number */
	curr_pcc_string = strtok(threads_list_string, delim);
	if (curr_pcc_string == NULL) {
		PRINT_ERROR("Error: Invalid PCC threads list: %s\n", threads_list_string);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Walk through rest of the PCC threads numbers */
	while (curr_pcc_string != NULL) {
		/* Check if it's a number by checking its digits */
		len = strnlen(threads_list_string, MAX_ARG_SIZE);
		for (i = 0; i < len; i++) {
			if (!isdigit(curr_pcc_string[i])) {
				PRINT_ERROR("Error: Invalid PCC thread number: %s\n", curr_pcc_string);
				return DOCA_ERROR_INVALID_VALUE;
			}
		}

		/* Convert to integer to check if it is non-negative */
		curr_pcc_check = (int)atoi(curr_pcc_string);
		if (curr_pcc_check < 0) {
			PRINT_ERROR("Error: Invalid PCC thread number %d. PCC threads numbers must be non-negative\n",
				    curr_pcc_check);
			return DOCA_ERROR_INVALID_VALUE;
		}

		curr_pcc = (uint32_t)atoi(curr_pcc_string);
		pcc_cfg->threads_list[pcc_cfg->threads_num++] = curr_pcc;
		curr_pcc_string = strtok(NULL, delim);
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCC wait time parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t wait_time_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	int wait_time = *((int *)param);

	/* Wait time must be either positive or infinity (meaning -1 )*/
	if (wait_time == 0) {
		PRINT_ERROR(
			"Error: Entered wait time can't be zero. Must be either positive or infinity (meaning negative value)\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	pcc_cfg->wait_time = wait_time;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCC remote SW handler parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ccmad_remote_sw_handler_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;

	pcc_cfg->remote_sw_handler = *((bool *)param);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCC device coredump file parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t coredump_file_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	const char *path = (char *)param;

	int path_len = strnlen(path, MAX_ARG_SIZE);
	if (path_len == MAX_ARG_SIZE) {
		PRINT_ERROR("Entered path exceeded buffer size: %d\n", MAX_USER_ARG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* The string will be '\0' terminated due to the strnlen check above */
	strncpy(pcc_cfg->coredump_file, path, path_len + 1);

	return DOCA_SUCCESS;
}

static doca_error_t steer_path_ip_callback(void *param, void *config, unsigned int path)
{
	struct pcc_config *cfg = config;
	struct in_addr addr;
	if (inet_pton(AF_INET, (const char *)param, &addr) != 1)
		return DOCA_ERROR_INVALID_VALUE;
	cfg->steer_path_ip[path] = addr.s_addr;
	cfg->steer_path_ip_set[path] = true;
	return DOCA_SUCCESS;
}
static doca_error_t steer_path0_ip_callback(void *param, void *config)
{
	return steer_path_ip_callback(param, config, 0);
}
static doca_error_t steer_path1_ip_callback(void *param, void *config)
{
	return steer_path_ip_callback(param, config, 1);
}

static doca_error_t steer_force_path_callback(void *param, void *config)
{
	struct pcc_config *cfg = config;
	const char *value = param;

	if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0) {
		PRINT_ERROR("Error: --force-path must be 0 or 1\n");
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->steer_force_path = value[0] - '0';
	cfg->steer_force_path_set = true;
	return DOCA_SUCCESS;
}

#if DOCA_HAS_DEVICE_REPRESENTORS
/*
 * ARGP Callback - PF device for embedded steering (DOCA 3.x, -a/--steer-dev).
 * Also enables steering. Usually the same PF the PCC RP runs on.
 */
static doca_error_t steer_device_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	struct doca_argp_device_ctx *dev_ctx = (struct doca_argp_device_ctx *)param;

	pcc_cfg->steer_enable = true;
	pcc_cfg->steer_dev = dev_ctx->dev;
	if (dev_ctx->devargs)
		pcc_cfg->steer_devargs = dev_ctx->devargs;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - sender SF representor for embedded steering (DOCA 3.x,
 * -r/--steer-rep, e.g. "pci/0000:03:00.1,pf1sf0"). Enables steering and provides
 * both the PF dev and the SF representor that steer_start() needs.
 */
static doca_error_t steer_rep_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	struct doca_argp_device_rep_ctx *rep_ctx = (struct doca_argp_device_rep_ctx *)param;

	pcc_cfg->steer_enable = true;
	pcc_cfg->steer_dev = rep_ctx->dev_ctx.dev;
	pcc_cfg->steer_dev_rep = rep_ctx->dev_rep;
	if (rep_ctx->dev_ctx.devargs)
		pcc_cfg->steer_devargs = rep_ctx->dev_ctx.devargs;
	return DOCA_SUCCESS;
}
#else
static doca_error_t steer_rep_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = config;
	doca_error_t err = steer_parse_rep_spec(param, pcc_cfg->steer_pci_addr,
	                                        &pcc_cfg->steer_sf_num);

	if (err != DOCA_SUCCESS) {
		PRINT_ERROR("Error: -r must use pci/<BDF>,pf<N>sf<N> syntax\n");
		return err;
	}
	pcc_cfg->steer_enable = true;
	return DOCA_SUCCESS;
}
#endif

/*
 * ARGP Callback - Handles DPA resources file path parameter
 *
 * @param[in] param Input parameter
 * @param[in,out] config Program configuration context
 * @return DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dpa_resources_file_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	const char *path = (char *)param;

	int path_len = strnlen(path, MAX_ARG_SIZE);
	if (path_len == MAX_ARG_SIZE) {
		PRINT_ERROR("Error: Entered path exceeded buffer size: %d\n", MAX_USER_ARG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(pcc_cfg->dpa_resources_file, path, path_len + 1);

	/* Check if the DPA resources file exists */
	if (path_len > 0) {
		FILE *file = fopen(path, "r");
		if (file == NULL) {
			PRINT_ERROR("Error: DPA resources file '%s' does not exist or cannot be accessed\n", path);
			return DOCA_ERROR_NOT_FOUND;
		}
		fclose(file);
		use_dpa_resources = true;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handles DPA application key parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dpa_application_key_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	const char *app_key = (char *)param;

	int dpa_app_key_len = strnlen(app_key, MAX_ARG_SIZE);
	if (dpa_app_key_len == MAX_ARG_SIZE) {
		PRINT_ERROR("Entered application key exceeded buffer size: %d\n", MAX_USER_ARG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(pcc_cfg->dpa_application_key, app_key, dpa_app_key_len + 1);
	use_dpa_application_key = true;

	return DOCA_SUCCESS;
}

doca_error_t register_pcc_params(void)
{
	struct doca_argp_param *device_param;
	struct doca_argp_param *threads_list_param;
	struct doca_argp_param *wait_time_param;
	struct doca_argp_param *remote_sw_handler_param;
	struct doca_argp_param *coredump_file_param;
	struct doca_argp_param *dpa_resources_file;
	struct doca_argp_param *dpa_application_key;

	/* Create and register DOCA device name parameter */
	doca_error_t result = doca_argp_param_create(&device_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(device_param, "d");
	doca_argp_param_set_long_name(device_param, "device");
	doca_argp_param_set_arguments(device_param, "<RDMA device names>");
	doca_argp_param_set_description(device_param, "RDMA device name that supports PCC (mandatory).");
	doca_argp_param_set_callback(device_param, device_name_callback);
	doca_argp_param_set_type(device_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(device_param);
	result = doca_argp_register_param(device_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register PCC threads list parameter */
	result = doca_argp_param_create(&threads_list_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(threads_list_param, "t");
	doca_argp_param_set_long_name(threads_list_param, "threads");
	doca_argp_param_set_arguments(threads_list_param, "<PCC threads list>");
	doca_argp_param_set_description(
		threads_list_param,
		"A list of the PCC threads numbers to be chosen for the DOCA PCC context to run on (optional). Must be provided as a string, such that the number are separated by a space.");
	doca_argp_param_set_callback(threads_list_param, threads_list_callback);
	doca_argp_param_set_type(threads_list_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(threads_list_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register PCC wait time parameter */
	result = doca_argp_param_create(&wait_time_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(wait_time_param, "w");
	doca_argp_param_set_long_name(wait_time_param, "wait-time");
	doca_argp_param_set_arguments(wait_time_param, "<PCC wait time>");
	doca_argp_param_set_description(
		wait_time_param,
		"The duration of the DOCA PCC wait (optional), can provide negative values which means infinity. If not provided then -1 will be chosen.");
	doca_argp_param_set_callback(wait_time_param, wait_time_callback);
	doca_argp_param_set_type(wait_time_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(wait_time_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register PCC remote SW handler */
	result = doca_argp_param_create(&remote_sw_handler_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(remote_sw_handler_param, "r-handler");
	doca_argp_param_set_long_name(remote_sw_handler_param, "remote-sw-handler");
	doca_argp_param_set_arguments(remote_sw_handler_param, "<CCMAD remote SW handler>");
	doca_argp_param_set_description(
		remote_sw_handler_param,
		"CCMAD remote SW handler flag (optional). If not provided then false will be chosen.");
	doca_argp_param_set_callback(remote_sw_handler_param, ccmad_remote_sw_handler_callback);
	doca_argp_param_set_type(remote_sw_handler_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(remote_sw_handler_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register PCC device coredump file parameter */
	result = doca_argp_param_create(&coredump_file_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(coredump_file_param, "f");
	doca_argp_param_set_long_name(coredump_file_param, "coredump-file");
	doca_argp_param_set_arguments(coredump_file_param, "<PCC coredump file>");
	doca_argp_param_set_description(
		coredump_file_param,
		"A pathname to the file to write coredump data in case of unrecoverable error on the device (optional). Must be provided as a string.");
	doca_argp_param_set_callback(coredump_file_param, coredump_file_callback);
	doca_argp_param_set_type(coredump_file_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(coredump_file_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	struct doca_argp_param *path0_ip_param, *path1_ip_param, *force_path_param;
	result = doca_argp_param_create(&path0_ip_param);
	if (result != DOCA_SUCCESS)
		return result;
	doca_argp_param_set_long_name(path0_ip_param, "path0-ip");
	doca_argp_param_set_arguments(path0_ip_param, "<IPv4>");
	doca_argp_param_set_description(path0_ip_param, "Path-0 receiver IP used for PCC flow grouping.");
	doca_argp_param_set_callback(path0_ip_param, steer_path0_ip_callback);
	doca_argp_param_set_type(path0_ip_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(path0_ip_param);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_argp_param_create(&path1_ip_param);
	if (result != DOCA_SUCCESS)
		return result;
	doca_argp_param_set_long_name(path1_ip_param, "path1-ip");
	doca_argp_param_set_arguments(path1_ip_param, "<IPv4>");
	doca_argp_param_set_description(path1_ip_param, "Path-1 receiver IP used for PCC flow grouping.");
	doca_argp_param_set_callback(path1_ip_param, steer_path1_ip_callback);
	doca_argp_param_set_type(path1_ip_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(path1_ip_param);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_argp_param_create(&force_path_param);
	if (result != DOCA_SUCCESS)
		return result;
	doca_argp_param_set_long_name(force_path_param, "force-path");
	doca_argp_param_set_arguments(force_path_param, "<0|1>");
	doca_argp_param_set_description(
		force_path_param,
		"Diagnostic: force path 0 or 1 (DOCA 3.x bypasses EGRESS_CLASSIFY entirely).");
	doca_argp_param_set_callback(force_path_param, steer_force_path_callback);
	doca_argp_param_set_type(force_path_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(force_path_param);
	if (result != DOCA_SUCCESS)
		return result;

	struct doca_argp_param *steer_rep_param;

	result = doca_argp_param_create(&steer_rep_param);
	if (result != DOCA_SUCCESS)
		return result;
	doca_argp_param_set_short_name(steer_rep_param, "r");
	doca_argp_param_set_long_name(steer_rep_param, "steer-rep");
	doca_argp_param_set_arguments(steer_rep_param, "<pci/bdf,pfNsfN>");
	doca_argp_param_set_description(steer_rep_param,
		"Enable embedded DOCA Flow egress steering on the sender SF representor.");
	doca_argp_param_set_callback(steer_rep_param, steer_rep_callback);
#if DOCA_HAS_DEVICE_REPRESENTORS
	doca_argp_param_set_type(steer_rep_param, DOCA_ARGP_TYPE_DEVICE_REP);
#else
	doca_argp_param_set_type(steer_rep_param, DOCA_ARGP_TYPE_STRING);
#endif
	result = doca_argp_register_param(steer_rep_param);
	if (result != DOCA_SUCCESS)
		return result;

#if DOCA_HAS_DEVICE_REPRESENTORS
	struct doca_argp_param *steer_dev_param;

	result = doca_argp_param_create(&steer_dev_param);
	if (result != DOCA_SUCCESS)
		return result;
	doca_argp_param_set_short_name(steer_dev_param, "a");
	doca_argp_param_set_long_name(steer_dev_param, "steer-dev");
	doca_argp_param_set_arguments(steer_dev_param, "<pci/bdf>");
	doca_argp_param_set_description(steer_dev_param,
		"Explicit PF device for embedded steering (optional, DOCA 3.x).");
	doca_argp_param_set_callback(steer_dev_param, steer_device_callback);
	doca_argp_param_set_type(steer_dev_param, DOCA_ARGP_TYPE_DEVICE);
	result = doca_argp_register_param(steer_dev_param);
	if (result != DOCA_SUCCESS)
		return result;
#endif

	/* Create and register DPA resources file parameter */
	result = doca_argp_param_create(&dpa_resources_file);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(dpa_resources_file, "dpa-resources");
	doca_argp_param_set_arguments(dpa_resources_file, "<DPA resources file>");
	doca_argp_param_set_description(
		dpa_resources_file,
		"Path to a DPA resources .yaml file (optional). Must be provided together with DPA application key.");
	doca_argp_param_set_callback(dpa_resources_file, dpa_resources_file_callback);
	doca_argp_param_set_type(dpa_resources_file, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(dpa_resources_file);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register DPA application name parameter */
	result = doca_argp_param_create(&dpa_application_key);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(dpa_application_key, "dpa-app-key");
	doca_argp_param_set_arguments(dpa_application_key, "<DPA application key>");
	doca_argp_param_set_description(
		dpa_application_key,
		"Application key in specified DPA resources .yaml file (optional). Must be provided together with DPA resources file.");
	doca_argp_param_set_callback(dpa_application_key, dpa_application_key_callback);
	doca_argp_param_set_type(dpa_application_key, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(dpa_application_key);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}
