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

#ifndef TELEM_TEMPLATE_H
#define TELEM_TEMPLATE_H

/*
 * Entry point to telem template (example) user algorithm (reference code)
 * This function starts the algorithm code of a single event for the telem template example algorithm
 * It calculates the new rate parameters based on flow context data and event info.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @param [in]: A pointer to an array of parameters that are used to control algo behavior (see PPCC access register)
 * @counter [in/out]: A pointer to an array of counters that are incremented by algo (see PPCC access register)
 * @algo_ctxt [in/out]: A pointer to a flow context data retrieved by libpcc.
 * @results [out]: A pointer to result struct to update rate in HW.
 */
void telem_template_algo(doca_pcc_dev_event_t *event,
			 uint32_t *param,
			 uint32_t *counter,
			 doca_pcc_dev_algo_ctxt_t *algo_ctxt,
			 doca_pcc_dev_results_t *results);

/*
 * Entry point to telem template (example) user algorithm initialization (reference code)
 * This function starts the user algorithm initialization code
 * The function will be called once per process load and should init all ports
 *
 * @algo_idx [in]: Algo identifier. To be passed on to initialization APIs
 */
void telem_template_init(uint32_t algo_idx);

/*
 * Entry point to telem template (example) user algorithm setting parameters (reference code)
 * This function starts the user algorithm setting parameters code
 * The function will be called to update algorithm parameters
 *
 * @param_id_base [in]: id of the first parameter that was changed.
 * @param_num [in]: number of all parameters that were changed
 * @new_param_values [in]: pointer to an array which holds param_num number of new values for parameters
 * @params [in]: pointer to an array which holds beginning of the current parameters to be changed
 * @return: DOCA_PCC_DEV_STATUS_FAIL if input parameters (one or more) are not legal.
 */
doca_pcc_dev_error_t telem_template_set_algo_params(uint32_t param_id_base,
						    uint32_t param_num,
						    const uint32_t *new_param_values,
						    uint32_t *params);

#endif /* TELEM_TEMPLATE_H */
