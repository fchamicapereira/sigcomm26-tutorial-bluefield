/*
 * Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_pcc_np_dev.h>
#include "pcc_common_dev.h"

#define NUM_LOGICAL_PORTS (2)

/**< Counters IDs to configure and read from */
uint32_t counter_ids[DOCA_PCC_DEV_MAX_NUM_PORTS] = {0};
/**< Table of RX bytes counters to sample to */
uint32_t current_sampled_rx_bytes[DOCA_PCC_DEV_MAX_NUM_PORTS] = {0};
/**< Flag to indicate that the counters have been initiated */
uint32_t counters_started = 0;

/**
 * @brief Initiate counter IDs global array on port for RX bytes counter type
 */
FORCE_INLINE void init_counter_ids(void)
{
	for (uint32_t i = 0; i < DOCA_PCC_DEV_MAX_NUM_PORTS; i++)
		counter_ids[i] = DOCA_PCC_DEV_GET_PORT_COUNTER_ID(i, DOCA_PCC_DEV_NIC_COUNTER_TYPE_RX_BYTES, 0);
}

doca_pcc_dev_error_t doca_pcc_dev_np_user_packet_handler(struct doca_pcc_np_dev_request_packet *in,
							 struct doca_pcc_np_dev_response_packet *out)
{
	if (doca_pcc_dev_thread_rank() == 0 && counters_started == 0) {
		init_counter_ids();
		/* Configure counters to read */
		doca_pcc_dev_nic_counters_config(counter_ids, NUM_LOGICAL_PORTS, current_sampled_rx_bytes);
		counters_started = 1;
		__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	}

	uint32_t *send_ts_p = (uint32_t *)(out->data);
	uint32_t *rx_256_bytes_p = (uint32_t *)(out->data + 4);
	uint32_t *rx_ts_p = (uint32_t *)(out->data + 8);

	if (counters_started) {
		*send_ts_p = __builtin_bswap32(*((uint32_t *)(doca_pcc_np_dev_get_payload(in))));
		doca_pcc_dev_nic_counters_sample();
		*rx_256_bytes_p = current_sampled_rx_bytes[doca_pcc_np_dev_get_port_num(in)];
		*rx_ts_p = doca_pcc_dev_get_timer_lo();
		__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
	}

	return DOCA_PCC_DEV_STATUS_OK;
}
