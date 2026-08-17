/*
 * Copyright (c) 2023-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>
#include <doca_pcc_dev_services.h>
#include <doca_version.h>
#include "pcc_common_dev.h"
#include "rtt_template.h"
#include "rtt_template_ctxt.h"
#include "pcc_rate_report.h"

#define DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK (1 << DOCA_PCC_DEV_EVNT_ROCE_ACK)
#define SAMPLER_THREAD_RANK (0)
#define COUNTERS_SAMPLE_WINDOW_IN_MICROSEC (10)
#define EVENT_SUMMARY_FLOW_BUCKETS (256)
#define PCC_TRACE_MAX_WORKERS (1024)

FORCE_INLINE uint32_t pcc_event_flow_qpn(doca_pcc_dev_event_t *event)
{
#if DOCA_VERSION_MAJOR > 2 || (DOCA_VERSION_MAJOR == 2 && DOCA_VERSION_MINOR >= 8)
	return doca_pcc_dev_get_flow_qpn(event);
#else
	uint32_t value = __builtin_bswap32(*(uint32_t *)((uint8_t *)&event->ev_spec_attr.roce_tx + 8));

	return value & 0x00FFFFFFu;
#endif
}
#define PCC_TRACE_FLUSH_INTERVAL_US (1000000)
/**< Counters IDs to configure and read from */
uint32_t counter_ids[DOCA_PCC_DEV_MAX_NUM_PORTS] = {0};
/**< Table of TX bytes counters to sample to */
uint32_t current_sampled_tx_bytes[DOCA_PCC_DEV_MAX_NUM_PORTS] = {0};
/**< Table of TX bytes counters that was last sampled */
uint32_t previous_sampled_tx_bytes[DOCA_PCC_DEV_MAX_NUM_PORTS] = {0};
/**< Last timestamp of sampled counters */
uint32_t last_sample_ts;
/**< Ports active bandwidth. Units of MB/s */
uint32_t ports_bw[DOCA_PCC_DEV_MAX_NUM_PORTS];
/**< Number of available and initiated logical ports */
uint32_t ports_num = 0;
/**< Percentage of the current active ports utilized bandwidth. Saved in FXP 16 format */
uint32_t g_utilized_bw[DOCA_PCC_DEV_MAX_NUM_PORTS];

#if DOCA_VERSION_MAJOR < 3
static volatile uint32_t rate_mailbox_qpn[PCC_RATE_MAILBOX_MAX_FLOWS];
static volatile uint32_t rate_mailbox_value[PCC_RATE_MAILBOX_MAX_FLOWS];
static volatile uint32_t rate_mailbox_dropped;

static void rate_mailbox_store(uint32_t qpn, uint32_t rate)
{
	uint32_t start = qpn % PCC_RATE_MAILBOX_MAX_FLOWS;

	for (uint32_t probe = 0; probe < PCC_RATE_MAILBOX_MAX_FLOWS; probe++) {
		uint32_t slot = (start + probe) % PCC_RATE_MAILBOX_MAX_FLOWS;
		uint32_t owner = rate_mailbox_qpn[slot];

		if (owner == 0 || owner == qpn) {
			rate_mailbox_qpn[slot] = qpn;
			rate_mailbox_value[slot] = rate;
			return;
		}
	}
	rate_mailbox_dropped++;
}

doca_pcc_dev_error_t doca_pcc_dev_user_mailbox_handle(void *request, uint32_t request_size,
                                                        uint32_t max_response_size, void *response,
                                                        uint32_t *response_size)
{
	struct pcc_rate_mailbox_response *out = response;
	const struct pcc_rate_mailbox_request *in = request;

	if (request_size != sizeof(*in) || in->version != PCC_RATE_MAILBOX_VERSION ||
	    max_response_size < sizeof(*out))
		return DOCA_PCC_DEV_STATUS_FAIL;
	out->version = PCC_RATE_MAILBOX_VERSION;
	out->count = 0;
	out->dropped = rate_mailbox_dropped;
	for (uint32_t i = 0; i < PCC_RATE_MAILBOX_MAX_FLOWS; i++) {
		uint32_t qpn = rate_mailbox_qpn[i];

		if (qpn == 0)
			continue;
		out->flow[out->count].qpn = qpn;
		out->flow[out->count].rate = rate_mailbox_value[i];
		out->count++;
	}
	*response_size = sizeof(*out);
	return DOCA_PCC_DEV_STATUS_OK;
}
#endif /* DOCA_VERSION_MAJOR < 3 */
/**< Flag to indicate that the counters have been initiated */
uint32_t counters_started = 0;

#ifdef DOCA_PCC_SAMPLE_TX_BYTES
/*
 * Dedicate one thread to sample tx bytes counters on a defined time frame,
 * calculate current bandwidth and compare with maximum port bandwidth.
 * This call is enabled by user option to sample TX bytes counter
 */
FORCE_INLINE void thread0_calc_ports_utilization(void)
{
	uint32_t tx_bytes_delta[DOCA_PCC_DEV_MAX_NUM_PORTS], current_bw[DOCA_PCC_DEV_MAX_NUM_PORTS], ts_delta,
		current_ts;

	if ((doca_pcc_dev_thread_rank() == SAMPLER_THREAD_RANK) && counters_started) {
		current_ts = doca_pcc_dev_get_timer_lo();
		ts_delta = diff_with_wrap32(current_ts, last_sample_ts);
		if (ts_delta >= COUNTERS_SAMPLE_WINDOW_IN_MICROSEC) {
			doca_pcc_dev_nic_counters_sample();
			for (uint32_t i = 0; i < ports_num; i++) {
				tx_bytes_delta[i] =
					diff_with_wrap32(current_sampled_tx_bytes[i], previous_sampled_tx_bytes[i]);
				previous_sampled_tx_bytes[i] = current_sampled_tx_bytes[i];
				current_bw[i] =
					(doca_pcc_dev_fxp_mult(tx_bytes_delta[i], doca_pcc_dev_fxp_recip(ts_delta)) >>
					 16);
				g_utilized_bw[i] = (1 << 16);
				if (current_bw[i] < ports_bw[i])
					g_utilized_bw[i] = doca_pcc_dev_fxp_mult(current_bw[i],
										 doca_pcc_dev_fxp_recip(ports_bw[i]));
			}
			last_sample_ts = current_ts;
		}
	}
}

/**
 * @brief Count the number of available logical ports from queried mask
 *
 * @param[in] ports_mask - ports_mask
 *
 * @return - number of available logical ports initiated in mask
 */
FORCE_INLINE uint32_t count_ports(uint32_t ports_mask)
{
	// find maximum port id enabled. Assume enabled ports are continuous
	return doca_pcc_dev_fls(ports_mask);
}

/**
 * @brief Initiate counter IDs global array on port for TX bytes counter type
 */
FORCE_INLINE void init_counter_ids(void)
{
	for (uint32_t i = 0; i < DOCA_PCC_DEV_MAX_NUM_PORTS; i++)
		counter_ids[i] = DOCA_PCC_DEV_GET_PORT_COUNTER_ID(i, DOCA_PCC_DEV_NIC_COUNTER_TYPE_TX_BYTES, 0);
}

/*
 * Initialize TX counters sampling
 */
FORCE_INLINE void tx_counters_sampling_init(uint32_t portid)
{
	/* number of ports to initiate counters for */
	ports_num = count_ports(doca_pcc_dev_get_logical_ports());
	/* Configure counters to read */
	doca_pcc_dev_nic_counters_config(counter_ids, ports_num, current_sampled_tx_bytes);
	/* save port speed in MBps units */
	ports_bw[portid] = (doca_pcc_dev_mult(doca_pcc_dev_get_port_speed(portid), 1000) >> 3);
	/* Sample counters and save in global table */
	doca_pcc_dev_nic_counters_sample();
	last_sample_ts = doca_pcc_dev_get_timer_lo();
	/* Save sampled TX bytes */
	for (uint32_t i = 0; i < ports_num; i++)
		previous_sampled_tx_bytes[i] = current_sampled_tx_bytes[i];
	counters_started = 1;
}

/*
 * Called on link or port info state change.
 * This callback is used to configure port counters to query TX bytes on
 *
 * @return - void
 */
void doca_pcc_dev_user_port_info_changed(uint32_t portid)
{
	tx_counters_sampling_init(portid);
}
#endif

/*
 * Main entry point to user CC algorithm (Reference code)
 * This function starts the algorithm code of a single event
 * It receives the flow context data, the event info and outputs the new rate parameters
 * The function can support multiple algorithms and can call the per algorithm handler based on
 * the algo type. If a single algorithm is required this code can be simplified
 * The function can not be renamed as it is called by the handler infrastructure
 *
 * @algo_ctxt [in]: A pointer to a flow context data retrieved by libpcc.
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @attr [in]: A pointer to additional parameters (algo type).
 * @results [out]: A pointer to result struct to update rate in HW.
 */
void doca_pcc_dev_user_algo(doca_pcc_dev_algo_ctxt_t *algo_ctxt,
			    doca_pcc_dev_event_t *event,
			    const doca_pcc_dev_attr_t *attr,
			    doca_pcc_dev_results_t *results)
{
	static uint32_t event_count[EVENT_SUMMARY_FLOW_BUCKETS] = {0};
	static uint32_t tx_count[EVENT_SUMMARY_FLOW_BUCKETS] = {0};
	static uint32_t rtt_count[EVENT_SUMMARY_FLOW_BUCKETS] = {0};
	static uint32_t cnp_count[EVENT_SUMMARY_FLOW_BUCKETS] = {0};
	static uint32_t cnp_window_count[EVENT_SUMMARY_FLOW_BUCKETS] = {0};
	static uint32_t last_print_ts[EVENT_SUMMARY_FLOW_BUCKETS] = {0};
	static uint32_t bucket_owner_qpn[EVENT_SUMMARY_FLOW_BUCKETS] = {0};
	static uint32_t collision_reported_qpn[EVENT_SUMMARY_FLOW_BUCKETS] = {0};
	uint32_t port_num = doca_pcc_dev_get_ev_attr(event).port_num;
	uint32_t ev_type = doca_pcc_dev_get_ev_attr(event).ev_type;
	uint32_t *param = doca_pcc_dev_get_algo_params(port_num, attr->algo_slot);
	uint32_t *counter = doca_pcc_dev_get_counters(port_num, attr->algo_slot);
	cc_ctxt_rtt_template_t *rtt_ctxt = (cc_ctxt_rtt_template_t *)algo_ctxt;
	uint32_t qpn = 0;
	uint32_t qpn_known = 0;
	uint32_t first_observed_flow = 0;

	/* BF3 exposes the QPN only on TX. libpcc supplies the algorithm context for
	 * this QP on every event, including CNP, so retain the identity as an
	 * explicit member of that per-QP context. */
	if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_TX) {
		qpn = pcc_event_flow_qpn(event);
		qpn_known = 1;
		first_observed_flow = rtt_ctxt->flow_qpn == 0;
		if (!first_observed_flow && rtt_ctxt->flow_qpn != qpn) {
			doca_pcc_dev_printf("PCC WARNING: algorithm context QPN changed old=0x%x new=0x%x port=%u slot=%u\n",
					    rtt_ctxt->flow_qpn, qpn, port_num, attr->algo_slot);
			/* Treat this as context recycling and force a host rate report for
			 * the newly observed owner. */
			rtt_ctxt->last_reported_rate = UINT32_MAX;
			first_observed_flow = 1;
			doca_pcc_dev_trace_flush();
		}
		rtt_ctxt->flow_qpn = qpn;
	} else if (rtt_ctxt->flow_qpn != 0) {
		qpn = rtt_ctxt->flow_qpn;
		qpn_known = 1;
	}
	uint32_t flow_bucket = qpn % EVENT_SUMMARY_FLOW_BUCKETS;

	uint32_t now = doca_pcc_dev_get_timer_lo();
	if (qpn_known) {
		/* These arrays are diagnostics indexed by qpn % table-size, not the
		 * algorithm's real per-flow context. Never silently merge two QPs. */
		if (bucket_owner_qpn[flow_bucket] == 0) {
			bucket_owner_qpn[flow_bucket] = qpn;
			last_print_ts[flow_bucket] = now;
		} else if (bucket_owner_qpn[flow_bucket] != qpn) {
			if (collision_reported_qpn[flow_bucket] != qpn) {
				doca_pcc_dev_printf("PCC ERROR: bucket collision bucket=%u owner_qpn=0x%x new_qpn=0x%x\n",
				                    flow_bucket, bucket_owner_qpn[flow_bucket], qpn);
				collision_reported_qpn[flow_bucket] = qpn;
			}
			goto skip_flow_summary;
		}

		event_count[flow_bucket]++;
		if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_TX)
			tx_count[flow_bucket]++;
		else if (ev_type == DOCA_PCC_DEV_EVNT_RTT)
			rtt_count[flow_bucket]++;
		else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_CNP) {
			cnp_count[flow_bucket]++;
			cnp_window_count[flow_bucket]++;
		}

		if (now - last_print_ts[flow_bucket] > 1000000) {
			uint32_t elapsed = now - last_print_ts[flow_bucket];
			uint32_t cnp_per_sec =
				(uint32_t)(((uint64_t)cnp_window_count[flow_bucket] * 1000000U) / elapsed);

			doca_pcc_dev_printf("PCC: bucket=%u total=%u tx=%u rtt=%u cnp=%u cnp_s=%u "
			                    "slot=%u port=%u qpn=0x%x rate=%u\n",
					    flow_bucket, event_count[flow_bucket], tx_count[flow_bucket],
					    rtt_count[flow_bucket], cnp_count[flow_bucket], cnp_per_sec,
					    attr->algo_slot, port_num, qpn, rtt_ctxt->cur_rate);
			cnp_window_count[flow_bucket] = 0;
			last_print_ts[flow_bucket] = now;
		}
	}

skip_flow_summary:
	;

#ifdef DOCA_PCC_SAMPLE_TX_BYTES
	thread0_calc_ports_utilization();
#endif

	/* Both rate state and its QPN identity live in the PCC-provided per-QP
	 * algorithm context. */
#if DOCA_VERSION_MAJOR >= 3
	uint32_t prev_rate = rtt_ctxt->cur_rate;
#endif

	switch (attr->algo_slot) {
	case 0: {
		static uint32_t rtt_count = 0;
		if (rtt_count < 5)
			doca_pcc_dev_printf("rtt_template: slot=%u ev=%u rtt_count=%u\n",
					    attr->algo_slot, ev_type, rtt_count);
		rtt_count++;
		rtt_template_algo(event, param, counter, algo_ctxt, results);
		break;
	}
	default: {
		/* @note The default internal algo is only supported for algo slot DOCA_PCC_DEV_ALGO_SLOT_INTERNAL and
		 * is initiated on DOCA_PCC_DEV_ALGO_INDEX_INTERNAL. */
		doca_pcc_dev_default_internal_algo(algo_ctxt, event, attr, results);
		break;
	}
	};
	/* The algorithm context retains the congestion-derived rate for the next
	 * event and for host steering. Hardware enforcement is disabled below. */
	uint32_t steering_rate = results->rate;

#if DOCA_VERSION_MAJOR < 3
	if (qpn_known)
		rate_mailbox_store(qpn, steering_rate);
#endif

	/* TX establishes the QPN in this per-QP context. Every later event for the
	 * same context, including CNP, can therefore publish its rate immediately. */
	if (qpn_known) {
		if (rtt_ctxt->last_reported_rate == steering_rate)
			goto skip_rate_report;

		/*
		 * Trace buffers are worker-local. Flush the startup report immediately so
		 * each QP's initial rate reaches the host even when its worker receives
		 * no later PCC event. Subsequent changes use the per-worker cadence below.
		 */
#if DOCA_VERSION_MAJOR >= 3
		uint32_t is_startup_report = first_observed_flow || prev_rate == 0;
		doca_pcc_dev_trace_5(PCC_RATE_REPORT_FORMAT_ID, qpn, steering_rate,
				     ev_type, rtt_ctxt->rtt, now);
		if (is_startup_report)
			doca_pcc_dev_trace_flush();
#endif
		/* DOCA 2.x has already published this update through rate_mailbox_store(). */
		rtt_ctxt->last_reported_rate = steering_rate;
	}

skip_rate_report:

	/*
	 * Do not share this state across PCC workers: each worker has its own trace
	 * buffer. A shared timestamp can suppress another worker's required flush.
	 */
	{
		static uint32_t last_flush_ts[PCC_TRACE_MAX_WORKERS] = {0};
		unsigned int worker = doca_pcc_dev_thread_rank();

		if (worker < PCC_TRACE_MAX_WORKERS &&
		    now - last_flush_ts[worker] > PCC_TRACE_FLUSH_INTERVAL_US) {
			doca_pcc_dev_trace_flush();
			last_flush_ts[worker] = now;
		}
	}

	/* PCC is used only as a congestion sensor for the host-side path-share
	 * controller. Never apply its calculated rate through the ordinary per-QP
	 * hardware rate limiter. */
	results->rate = DOCA_PCC_DEV_MAX_RATE;
}

/*
 * Main entry point to user algorithm initialization (reference code)
 * This function starts the user algorithm initialization code
 * The function will be called once per process load and should init all supported
 * algorithms and all ports
 *
 * @disable_event_bitmask [out]: user code can tell the infrastructure which event
 * types to ignore (mask out). Events of this type will be dropped and not passed to
 * any algo
 */
void doca_pcc_dev_user_init(uint32_t *disable_event_bitmask)
{
	uint32_t algo_idx = 0, algo_slot = 0, algo_en = 1;

	/* Initialize algorithm with algo_idx=0 */
	rtt_template_init(algo_idx);

	for (int port_num = 0; port_num < DOCA_PCC_DEV_MAX_NUM_PORTS; ++port_num) {
		/* Slot 0 will use algo_idx 0, default enabled */
		doca_pcc_dev_init_algo_slot(port_num, algo_slot, algo_idx, algo_en);
		doca_pcc_dev_trace_5(0, port_num, algo_idx, algo_slot, algo_en, DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK);
	}

#ifdef DOCA_PCC_SAMPLE_TX_BYTES
	/** Assuming this is called prior to doca_pcc_dev_user_port_info_changed() */
	init_counter_ids();
#endif

	/* disable events of below type */
	*disable_event_bitmask = DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK;
#ifdef DOCA_PCC_DEV_ACK_NACK_TX_EVENT_DISABLED_SUPPORTED
	if (DOCA_PCC_DEV_ACK_NACK_TX_EVENT_DISABLED_SUPPORTED == 1)
		*disable_event_bitmask |= (1 << DOCA_PCC_DEV_EVNT_ROCE_TX_FOR_ACK_NACK);
#endif

	doca_pcc_dev_printf("%s, disable_event_bitmask=0x%x\n", __func__, *disable_event_bitmask);
	doca_pcc_dev_printf("DEBUG: user_init complete, waiting for events\n");
	doca_pcc_dev_trace_flush();
}

/*
 * Called when the parameter change was set externally.
 * The implementation should:
 *     Check the given new_parameters values. If those are correct from the algorithm perspective,
 *     assign them to the given parameter array.

 * @port_num [in]: index of the port
 * @algo_slot [in]: Algo slot identifier as referred to in the PPCC command field "algo_slot"
 * if possible it should be equal to the algo_idx
 * @param_id_base [in]: id of the first parameter that was changed.
 * @param_num [in]: number of all parameters that were changed
 * @new_param_values [in]: pointer to an array which holds param_num number of new values for parameters
 * @params [in]: pointer to an array which holds beginning of the current parameters to be changed
 * @return -
 * DOCA_PCC_DEV_STATUS_OK: Parameters were set
 * DOCA_PCC_DEV_STATUS_FAIL: the values (one or more) are not legal. No parameters were changed
 */
doca_pcc_dev_error_t doca_pcc_dev_user_set_algo_params(uint32_t port_num,
						       uint32_t algo_slot,
						       uint32_t param_id_base,
						       uint32_t param_num,
						       const uint32_t *new_param_values,
						       uint32_t *params)
{
	/* Notify the user that a change happened to take action.
	 * I.E.: Pre calculate values to be used in the algo that are based on the parameter value.
	 * Support more complex checks. E.G.: Param is a bit mask - min and max do not help
	 * Param dependency checking.
	 */
	doca_pcc_dev_error_t ret = DOCA_PCC_DEV_STATUS_OK;

	switch (algo_slot) {
	case 0: {
		uint32_t algo_idx = doca_pcc_dev_get_algo_index(port_num, algo_slot);

		if (algo_idx == 0)
			ret = rtt_template_set_algo_params(param_id_base, param_num, new_param_values, params);
		else
			ret = DOCA_PCC_DEV_STATUS_FAIL;

		break;
	}
	default:
		break;
	}
	return ret;
}
