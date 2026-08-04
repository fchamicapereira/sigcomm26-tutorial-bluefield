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

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>

#include "utils.h"
#include "telem_template_ctxt.h"
#include "telem_template_algo_params.h"
#include "telem_template.h"

/* Algorithm parameters are defined in telem_template_algo_params.h */

/* Define the constants */
#define MAX_UINT (0xffffffff)
#define DEC_FACTOR ((1 << 16) - param[TELEM_TEMPLATE_UPDATE_FACTOR])	      /* Rate decrease factor */
#define CNP_DEC_FACTOR ((1 << 16) - 2 * param[TELEM_TEMPLATE_UPDATE_FACTOR])  /* CNP rate decrease factor */
#define NACK_DEC_FACTOR ((1 << 16) - 5 * param[TELEM_TEMPLATE_UPDATE_FACTOR]) /* NACK rate decrease factor */
#define ABORT_TIME (300000)	    /* The time to abort rtt_req - in nanosec */
#define TX_RATE_TH (52428)	    /* Threshold to update current rate according to TX rate. 0.8 in 16b fixed point */
#define HIGH_UTIL_THRESHOLD (55704) /* 0.85 in 16b fixed point */
#define HIGH_UTIL_DEC_FACTOR \
	((1 << 16) - 2 * param[TELEM_TEMPLATE_UPDATE_FACTOR]) /* Rate decrease factor in high port utilization mode */
#define HIGH_UTIL_CNP_DEC_FACTOR \
	((1 << 16) - 4 * param[TELEM_TEMPLATE_UPDATE_FACTOR]) /* CNP rate decrease factor in high port utilization \
							       * mode \
							       */
#define HIGH_UTIL_NACK_DEC_FACTOR \
	((1 << 16) - 10 * param[TELEM_TEMPLATE_UPDATE_FACTOR]) /* NACK rate decrease factor in high port utilization \
								mode */

typedef enum {
	TELEM_TEMPLATE_UPDATE_FACTOR = 0, /* configurable parameter of update factor */
	TELEM_TEMPLATE_AI = 1,		  /* configurable parameter of AI */
	TELEM_TEMPLATE_BASE_QLEN = 2,	  /* configurable parameter of base qlen */
	TELEM_TEMPLATE_NEW_FLOW_RATE = 3, /* configurable parameter of new flow rate */
	TELEM_TEMPLATE_MIN_RATE = 4,	  /* configurable parameter of min rate */
	TELEM_TEMPLATE_MAX_DELAY = 5,	  /* configurable parameter of max delay */
	TELEM_TEMPLATE_HAI = 6,		  /* configurable parameter of HAI*/
	TELEM_TEMPLATE_PORT_BW_G = 7,	  /* configurable parameter of port BW (in Gbps)*/
	TELEM_TEMPLATE_PARAM_NUM	  /* Maximal number of configurable parameters */
} telem_template_params_t;

enum {
	TELEM_TEMPLATE_COUNTER_TX_EVENT = 0,  /* tx event for telemetry template user algorithm */
	TELEM_TEMPLATE_COUNTER_RTT_EVENT = 1, /* rtt event for telemetry template user algorithm */
	TELEM_TEMPLATE_COUNTER_NUM	      /* Maximal number of counters */
} telem_template_counter_t;

const volatile char telem_template_desc[] = "telemetry template v0.1";
static const volatile char telem_template_param_update_factor_desc[] = "UPDATE_FACTOR, update factor";
static const volatile char telem_template_param_ai_desc[] = "AI, ai";
static const volatile char telem_template_param_base_rtt_desc[] = "BASE_RTT, base rtt";
static const volatile char telem_template_param_new_flow_rate_desc[] = "NEW_FLOW_RATE, new flow rate";
static const volatile char telem_template_param_min_rate_desc[] = "MIN_RATE, min rate";
static const volatile char telem_template_param_max_delay_desc[] = "MAX_DELAY, max delay";
static const volatile char telem_template_counter_tx_desc[] = "COUNTER_TX_EVENT, number of tx events handled";
static const volatile char telem_template_counter_rtt_desc[] = "COUNTER_RTT_EVENT, number of rtt events handled";

/**
 * @brief Get the BW in GBps
 *
 * @param [in]: param
 * @return: BW in GBps
 */
static inline uint32_t getBW_GBps(uint32_t *param)
{
	return param[TELEM_TEMPLATE_PORT_BW_G] >> 3;
}

void telem_template_init(uint32_t algo_idx)
{
	struct doca_pcc_dev_algo_meta_data algo_def = {0};

	algo_def.algo_id = 0xBFFF;
	algo_def.algo_major_version = 0x00;
	algo_def.algo_minor_version = 0x01;
	algo_def.algo_desc_size = sizeof(telem_template_desc);
	algo_def.algo_desc_addr = (uint64_t)telem_template_desc;

	uint32_t total_param_num = TELEM_TEMPLATE_PARAM_NUM;
	uint32_t total_counter_num = TELEM_TEMPLATE_COUNTER_NUM;
	uint32_t param_num = 0;
	uint32_t counter_num = 0;

	doca_pcc_dev_algo_init_metadata(algo_idx, &algo_def, total_param_num, total_counter_num);

	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     UPDATE_FACTOR,
				     UPDATE_FACTOR_MAX,
				     1,
				     1,
				     sizeof(telem_template_param_update_factor_desc),
				     (uint64_t)telem_template_param_update_factor_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     AI,
				     AI_MAX,
				     1,
				     1,
				     sizeof(telem_template_param_ai_desc),
				     (uint64_t)telem_template_param_ai_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     BASE_RTT,
				     UINT32_MAX,
				     1,
				     1,
				     sizeof(telem_template_param_base_rtt_desc),
				     (uint64_t)telem_template_param_base_rtt_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     NEW_FLOW_RATE,
				     RATE_MAX,
				     1,
				     1,
				     sizeof(telem_template_param_new_flow_rate_desc),
				     (uint64_t)telem_template_param_new_flow_rate_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     MIN_RATE,
				     RATE_MAX,
				     1,
				     1,
				     sizeof(telem_template_param_min_rate_desc),
				     (uint64_t)telem_template_param_min_rate_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     MAX_DELAY,
				     UINT32_MAX,
				     1,
				     1,
				     sizeof(telem_template_param_max_delay_desc),
				     (uint64_t)telem_template_param_max_delay_desc);

	doca_pcc_dev_algo_init_counter(algo_idx,
				       counter_num++,
				       UINT32_MAX,
				       2,
				       sizeof(telem_template_counter_tx_desc),
				       (uint64_t)telem_template_counter_tx_desc);
	doca_pcc_dev_algo_init_counter(algo_idx,
				       counter_num++,
				       UINT32_MAX,
				       2,
				       sizeof(telem_template_counter_rtt_desc),
				       (uint64_t)telem_template_counter_rtt_desc);
}

/*
 * Entry point to core function of algorithm (reference code)
 * This function adjusts rate based on CC events.
 * It calculates the new rate parameters based on flow context data, rtt info and current rate.
 *
 * @norm_tx_rate [in]: The value of normalized switch TX rate
 * @qlen_256B [in]: Current switch queue occupancy in 256B units
 * @cur_rate [in]: Current rate value
 * @param [in]: A pointer to an array of parameters that are used to control algo behavior
 * @return: The new calculated rate value
 */
static inline uint32_t algorithm_core(uint32_t norm_tx_rate, uint32_t qlen_256B, uint32_t cur_rate, uint32_t *param)
{
	////###### Put your algorithm code in here#######/////

	/// Example ////
	if ((qlen_256B << 8) > param[TELEM_TEMPLATE_BASE_QLEN]) {
		cur_rate = doca_pcc_dev_fxp_mult(DEC_FACTOR, cur_rate);
	} else if (norm_tx_rate < TX_RATE_TH) {
		cur_rate += param[TELEM_TEMPLATE_HAI];
	} else {
		cur_rate += param[TELEM_TEMPLATE_AI];
	}

	if (cur_rate > DOCA_PCC_DEV_MAX_RATE)
		cur_rate = DOCA_PCC_DEV_MAX_RATE;

	if (cur_rate < param[TELEM_TEMPLATE_MIN_RATE])
		cur_rate = param[TELEM_TEMPLATE_MIN_RATE];

	return cur_rate;
}

/*
 * Entry point to telemetry template to handle roce tx event (reference code)
 * This function updates flags for rtt measurement or re-send rtt_req if needed.
 * It calculates the new rate parameters based on flow context data, event info and current rate.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @cur_rate [in]: Current rate value
 * @ccctx [in/out]: A pointer to a flow context data retrieved by libpcc.
 * @results [out]: A pointer to result struct to update rate in HW.
 */
static inline void telem_template_handle_roce_tx(doca_pcc_dev_event_t *event,
						 uint32_t cur_rate,
						 cc_ctxt_telem_template_t *ccctx,
						 doca_pcc_dev_results_t *results)
{
	uint8_t rtt_req = 0;
	uint32_t rtt_meas_psn = ccctx->rtt_meas_psn;
	uint32_t timestamp = doca_pcc_dev_get_timestamp(event);
	doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);

	if (unlikely((ev_attr.flags & DOCA_PCC_DEV_TX_FLAG_RTT_REQ_SENT) && (rtt_meas_psn == 0))) {
		ccctx->rtt_meas_psn = 1;
		ccctx->flags.rtt_req_to_rtt_sent = 0;
		ccctx->start_delay = timestamp;
	} else {
		/* Calculate rtt_till_now */
		uint32_t rtt_till_now = (timestamp - ccctx->start_delay);

		if (unlikely(ccctx->start_delay > timestamp))
			rtt_till_now += UINT32_MAX;
		/* Abort RTT request flow - for cases event or packet was dropped */
		if (rtt_meas_psn == 0) {
			rtt_till_now = 0;
			ccctx->flags.rtt_req_to_rtt_sent += 1;
		}
		if (unlikely((rtt_till_now > ((uint32_t)ABORT_TIME << ccctx->abort_cnt)) ||
			     (ccctx->flags.rtt_req_to_rtt_sent > 2))) {
			rtt_req = 1;
			if (rtt_till_now > ((uint32_t)ABORT_TIME << ccctx->abort_cnt))
				ccctx->abort_cnt += 1;
			ccctx->flags.rtt_req_to_rtt_sent = 1;
		}
	}

	/* Update results buffer and context */
	ccctx->cur_rate = cur_rate;
	results->rate = cur_rate;
	results->rtt_req = rtt_req;
}

/*
 * Entry point to telemetry template to handle roce rtt event (reference code)
 * This function calculates the rtt and calls core function to adjust rate.
 * It calculates the new rate parameters based on flow context data, event info and current rate.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @cur_rate [in]: Current rate value
 * @param [in]: A pointer to an array of parameters that are used to control algo behavior
 * @ccctx [in/out]: A pointer to a flow context data retrieved by libpcc.
 * @results [out]: A pointer to result struct to update rate in HW.
 */
static inline void telem_template_handle_roce_rtt(doca_pcc_dev_event_t *event,
						  uint32_t cur_rate,
						  uint32_t *param,
						  cc_ctxt_telem_template_t *ccctx,
						  doca_pcc_dev_results_t *results)
{
	/*
	 * Check that this RTT event is the one we are waiting for.
	 * For cases we re-send RTT request by mistake due to abort flow for example.
	 */
	uint32_t rtt_meas_psn = ccctx->rtt_meas_psn;

	if (unlikely(((rtt_meas_psn == 0) && (ccctx->flags.rtt_req_to_rtt_sent == 0)))) {
		results->rate = cur_rate;
		results->rtt_req = 0;
		return;
	}

	/* We got RTT measurement */
	/* Reset variables */
	ccctx->rtt_meas_psn = 0;
	ccctx->abort_cnt = 0;

	unsigned char *rtt_raw_data = doca_pcc_dev_get_rtt_raw_data(event);
	doca_pcc_dev_switch_telem_extra_t telem_extra = *((doca_pcc_dev_switch_telem_extra_t *)(rtt_raw_data));
	uint32_t switch_tx_bytes = *((uint32_t *)(rtt_raw_data + 4));
	uint32_t switch_tx_timestamp = *((uint32_t *)(rtt_raw_data + 8));
	uint32_t qlen_256B = telem_extra.qlen;

	uint32_t last_switch_tx_timestamp = ccctx->last_switch_tx_timestamp;
	uint32_t last_tx_bytes = ccctx->last_tx_bytes;
	ccctx->last_tx_bytes = switch_tx_bytes;
	ccctx->last_switch_tx_timestamp = switch_tx_timestamp;

	/* Call to the core of the CC algorithm */

	uint32_t delta_timestamp_ns = switch_tx_timestamp - last_switch_tx_timestamp;
	// handle wraparound
	if (unlikely(switch_tx_timestamp < last_switch_tx_timestamp))
		delta_timestamp_ns += MAX_UINT;
	uint32_t delta_tx = switch_tx_bytes - last_tx_bytes;
	// handle wraparound
	if (unlikely(switch_tx_bytes < last_tx_bytes))
		delta_tx += MAX_UINT;
	uint32_t delta_tx_256bytes = delta_tx >> 8;
	uint32_t tx_rate_GBps = doca_pcc_dev_fxp_mult(delta_tx_256bytes, doca_pcc_dev_fxp_recip(delta_timestamp_ns))
				<< 8; // fxp16
	uint32_t norm_tx_rate = doca_pcc_dev_mult(tx_rate_GBps, doca_pcc_dev_fxp_recip(getBW_GBps(param))) >> 32;
	if (norm_tx_rate > (1 << 16))
		norm_tx_rate = (1 << 16);
	cur_rate = algorithm_core(norm_tx_rate, qlen_256B, cur_rate, param);

	ccctx->flags.rtt_req_to_rtt_sent = 1;
	ccctx->cur_rate = cur_rate;
	results->rate = cur_rate;
	results->rtt_req = 1;
}

/*
 * Entry point to telemetry template to handle roce cnp events (reference code)
 * This function puts the code for immediate reaction to CNPs.
 * It calculates the new rate parameters based on flow context data, event info and current rate.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @cur_rate [in]: Current rate value
 * @ccctx [in/out]: A pointer to a flow context data retrieved by libpcc.
 * @results [out]: A pointer to result struct to update rate in HW.
 */
static inline void telem_template_handle_roce_cnp(doca_pcc_dev_event_t *event,
						  uint32_t cur_rate,
						  cc_ctxt_telem_template_t *ccctx,
						  doca_pcc_dev_results_t *results)
{
	(void)(event);
	ccctx->flags.was_cnp = 1;

	/* ###### You can put the code for immediate reaction to CNPs ####### */
	/*
	 * e.g:
	 * cur_rate =
	 */
	results->rtt_req = 0;
	results->rate = cur_rate;
	ccctx->cur_rate = cur_rate;
}

/*
 * Entry point to telemetry template to handle roce nack events (reference code)
 * This function puts the code for immediate reaction to NACKs.
 * It calculates the new rate parameters based on flow context data, event info and current rate.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @cur_rate [in]: Current rate value
 * @ccctx [in]: A pointer to a flow context data retrieved by libpcc.
 * @results [out]: A pointer to result struct to update rate in HW.
 */
static inline void telem_template_handle_roce_nack(doca_pcc_dev_event_t *event,
						   uint32_t cur_rate,
						   cc_ctxt_telem_template_t *ccctx,
						   doca_pcc_dev_results_t *results)
{
	(void)(event);
	ccctx->flags.was_nack = 1;
	results->rate = cur_rate;
	ccctx->cur_rate = cur_rate;
}

/*
 * Entry point to telemetry template to handle new flow (reference code)
 * This function initializes the flow context.
 * It calculates the new rate parameters based on flow context data, event info and current rate.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @cur_rate [in]: Current rate value
 * @param [in]: A pointer to an array of parameters that are used to control algo behavior
 * @ccctx [in/out]: A pointer to a flow context data retrieved by libpcc.
 * @results [out]: A pointer to result struct to update rate in HW.
 */
static inline void telem_template_handle_new_flow(doca_pcc_dev_event_t *event,
						  uint32_t cur_rate,
						  uint32_t *param,
						  cc_ctxt_telem_template_t *ccctx,
						  doca_pcc_dev_results_t *results)
{
	(void)(cur_rate);
	ccctx->cur_rate = param[TELEM_TEMPLATE_NEW_FLOW_RATE];
	ccctx->start_delay = doca_pcc_dev_get_timestamp(event);
	ccctx->rtt_meas_psn = 0;
	ccctx->flags.rtt_req_to_rtt_sent = 1;
	ccctx->abort_cnt = 0;
	ccctx->flags.was_nack = 0;
	results->rate = param[TELEM_TEMPLATE_NEW_FLOW_RATE];
	results->rtt_req = 1;
}

void telem_template_algo(doca_pcc_dev_event_t *event,
			 uint32_t *param,
			 uint32_t *counter,
			 doca_pcc_dev_algo_ctxt_t *algo_ctxt,
			 doca_pcc_dev_results_t *results)
{
	cc_ctxt_telem_template_t *telem_template_ctx = (cc_ctxt_telem_template_t *)algo_ctxt;
	doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);
	uint32_t ev_type = ev_attr.ev_type;
	uint32_t cur_rate = telem_template_ctx->cur_rate;

	if (unlikely(cur_rate == 0)) {
		telem_template_handle_new_flow(event, cur_rate, param, telem_template_ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_TX) {
		telem_template_handle_roce_tx(event, cur_rate, telem_template_ctx, results);
		/* Example code to update counter */
		if (counter != NULL)
			counter[TELEM_TEMPLATE_COUNTER_TX_EVENT]++;
	} else if (ev_type == DOCA_PCC_DEV_EVNT_RTT) {
		telem_template_handle_roce_rtt(event, cur_rate, param, telem_template_ctx, results);
		/* Example code to update counter */
		if (counter != NULL)
			counter[TELEM_TEMPLATE_COUNTER_RTT_EVENT]++;
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_CNP) {
		telem_template_handle_roce_cnp(event, cur_rate, telem_template_ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_NACK) {
		telem_template_handle_roce_nack(event, cur_rate, telem_template_ctx, results);
	} else {
		results->rate = cur_rate;
		results->rtt_req = 0;
	}
}

doca_pcc_dev_error_t telem_template_set_algo_params(uint32_t param_id_base,
						    uint32_t param_num,
						    const uint32_t *new_param_values,
						    uint32_t *params)
{
	if ((param_num > TELEM_TEMPLATE_PARAM_NUM) || (param_id_base >= TELEM_TEMPLATE_PARAM_NUM))
		return DOCA_PCC_DEV_STATUS_FAIL;

	if ((new_param_values == NULL) || (params == NULL))
		return DOCA_PCC_DEV_STATUS_FAIL;

	return DOCA_PCC_DEV_STATUS_OK;
}
