/*
 * DOCA PCC reaction-point entry point — dispatches PCC events to rtt_template_algo()
 * (algo/rtt_template.c, our pure-ECN controller). Single algo slot, no other roles.
 *
 * Derived from NVIDIA's DOCA PCC "rp_rtt_template_dev_main.c" sample (DOCA >= 2.9,
 * BSD-3-Clause, see algo/rtt_template.c for the full license text), trimmed to drop
 * features this tutorial doesn't use: TX-byte-counter sampling, the mailbox handler,
 * and switch-telemetry / NP roles. Those are all off by default in the stock sample
 * too (gated behind build options we never enable) — dropping them here just removes
 * dead code rather than changing behavior.
 */

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>

#include "algo/rtt_template.h"

#define DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK (1 << DOCA_PCC_DEV_EVNT_ROCE_ACK)

/*
 * Main entry point to user CC algorithm. Called once per event; dispatches by algo
 * slot to the corresponding algorithm handler. We only ever run one algorithm (slot 0).
 *
 * @algo_ctxt [in]: flow context data retrieved by libpcc.
 * @event [in]: event data structure passed to extractor functions.
 * @attr [in]: additional parameters (algo type).
 * @results [out]: result struct to update rate in HW.
 */
void doca_pcc_dev_user_algo(doca_pcc_dev_algo_ctxt_t *algo_ctxt, doca_pcc_dev_event_t *event,
                            const doca_pcc_dev_attr_t *attr, doca_pcc_dev_results_t *results) {
  uint32_t port_num = doca_pcc_dev_get_ev_attr(event).port_num;
  uint32_t *param = doca_pcc_dev_get_algo_params(port_num, attr->algo_slot);
  uint32_t *counter = doca_pcc_dev_get_counters(port_num, attr->algo_slot);

  switch (attr->algo_slot) {
    case 0:
      rtt_template_algo(event, param, counter, algo_ctxt, results);
      break;
    default:
      doca_pcc_dev_default_internal_algo(algo_ctxt, event, attr, results);
      break;
  }
}

/*
 * Called once per process load. Initializes the one algorithm we run (slot 0, all ports).
 *
 * @disable_event_bitmask [out]: event types the infra should drop before they reach any algo.
 */
void doca_pcc_dev_user_init(uint32_t *disable_event_bitmask) {
  uint32_t algo_idx = 0;

  rtt_template_init(algo_idx);

  for (int port_num = 0; port_num < DOCA_PCC_DEV_MAX_NUM_PORTS; ++port_num) {
    doca_pcc_dev_init_algo_slot(port_num, 0, algo_idx, 1);
    doca_pcc_dev_trace_5(0, port_num, 0, algo_idx, 1, DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK);
  }

  *disable_event_bitmask = DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK;
  doca_pcc_dev_printf("%s, disable_event_bitmask=0x%x\n", __func__, *disable_event_bitmask);
  doca_pcc_dev_trace_flush();
}

/*
 * Called when a parameter change is set externally (e.g. via doca_pcc_set_algo_params on host).
 *
 * @port_num [in]: index of the port.
 * @algo_slot [in]: algo slot identifier, as referred to in the PPCC command field "algo_slot".
 * @param_id_base [in]: id of the first parameter that changed.
 * @param_num [in]: number of parameters that changed.
 * @new_param_values [in]: new values for those parameters.
 * @params [in]: current parameter array to update.
 * @return: DOCA_PCC_DEV_STATUS_OK if applied, DOCA_PCC_DEV_STATUS_FAIL if rejected.
 */
doca_pcc_dev_error_t doca_pcc_dev_user_set_algo_params(uint32_t port_num, uint32_t algo_slot,
                                                       uint32_t param_id_base, uint32_t param_num,
                                                       const uint32_t *new_param_values,
                                                       uint32_t *params) {
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
