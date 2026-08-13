#ifndef DOCA_FLOW_COMPAT_H_
#define DOCA_FLOW_COMPAT_H_

#include <doca_flow.h>
#include <doca_version.h>

// DOCA 2.9 renamed doca_flow_query -> doca_flow_resource_query (struct wrapped in a `counter`
// union member), doca_flow_query_entry -> doca_flow_resource_query_entry, and
// doca_flow_shared_resource_cfg -> doca_flow_shared_resource_set_cfg. Shim the old (pre-2.9)
// names to the new call sites this directory's .c files use, so the same source builds against
// either DOCA release. Force-included via -include in meson.build, not #include'd directly.
#if DOCA_VERSION_MAJOR == 2 && DOCA_VERSION_MINOR < 9

struct doca_flow_resource_query {
  struct doca_flow_query counter;
};

static inline doca_error_t doca_flow_resource_query_entry(struct doca_flow_pipe_entry *entry,
                                                          struct doca_flow_resource_query *query) {
  return doca_flow_query_entry(entry, &query->counter);
}

#define doca_flow_shared_resource_set_cfg doca_flow_shared_resource_cfg

#endif  // DOCA < 2.9
// Shared-mirror "original packet destination" (doca_flow_resource_mirror_cfg.fwd).
//
// The header calls the field optional, but the two releases disagree about what leaving it zero
// means, and each REJECTS the other's answer:
//
//   DOCA 2.7  zero is DOCA_FLOW_FWD_NONE, taken literally: the original packet is dropped, the
//             mirror delivers nothing, and the PF0 data path goes dark with no error logged.
//             Measured on testbed B (2.7, FW 32.41.1000): unset = RoCE never connects, 0 packets
//             captured; set = 92.27 Gb/s (line rate, matching the no-program baseline) with
//             2.35M CE-marked packets in the pcap.
//   DOCA 2.9  setting it makes doca_flow_shared_resources_bind fail outright —
//             "Failed to create HWS mirror action", FTE syndrome 0x6d3a25 — so the program will
//             not start at all. Leaving it zero is correct here; 2.9 falls back to the entry fwd.
//
// It tracks the library, not the firmware: DOCA 2.9 userspace in a container on testbed B's own
// 2.7-era firmware works with the field unset.
#if DOCA_VERSION_MAJOR == 2 && DOCA_VERSION_MINOR < 9
#define DOCA_TUT_MIRROR_SET_ORIG_FWD(mc, dest_port) \
  do {                                              \
    (mc).fwd.type = DOCA_FLOW_FWD_PORT;             \
    (mc).fwd.port_id = (dest_port);                 \
  } while (0)
#else
#define DOCA_TUT_MIRROR_SET_ORIG_FWD(mc, dest_port) \
  do {                                              \
    (void)(dest_port);                              \
  } while (0)
#endif

// DOCA 3.2 reworked how an entry names its action template, and renamed the entry flags.
//
// Three changes, all mechanical, and none of them visible in doca_flow_ecn_pcap.c because they are
// shimmed here:
//
//   doca_flow_pipe_add_entry   ->  doca_flow_pipe_basic_add_entry, with a new uint8_t action_idx
//                                  argument after `match`. Before 3.2 the index rode inside
//                                  doca_flow_actions.action_idx instead — the same field doca-2
//                                  sets by hand.
//   doca_flow_pipe_hash_add_entry  gained the same action_idx argument, after `entry_index`, and
//                                  its `flags` widened from enum doca_flow_flags_type to uint32_t.
//   DOCA_FLOW_NO_WAIT          ->  DOCA_FLOW_ENTRY_FLAGS_NO_WAIT
//   DOCA_FLOW_WAIT_FOR_BATCH   ->  DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH
//
// The flag VALUES differ as well as the names — pre-3.2 they are 0 and 1, from 3.2 they are 1 and
// 2 — so these must be mapped by name, never by number. Each name resolves to whatever its own
// release calls correct.
//
// The boundary is placed at 3.2 because that is where NVIDIA documents the change. Verified by
// building on 3.1 (shim path) and 3.4 (native path); **3.2 and 3.3 are untested — there is no
// machine on either in the fleet.** If one ever appears and fails to build, this one number is the
// thing to adjust.
#if DOCA_VERSION_MAJOR == 3 && DOCA_VERSION_MINOR < 2

#define DOCA_FLOW_ENTRY_FLAGS_NO_WAIT DOCA_FLOW_NO_WAIT
#define DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH DOCA_FLOW_WAIT_FOR_BATCH

// Folds action_idx back into the actions struct. `actions` is const, so it is copied rather than
// written through; add_entry consumes the template during the call, exactly as doca-2 relies on
// when it passes a stack-local `eact`.
static inline doca_error_t doca_flow_pipe_basic_add_entry(
    uint16_t pipe_queue, struct doca_flow_pipe *pipe, const struct doca_flow_match *match,
    uint8_t action_idx, const struct doca_flow_actions *actions,
    const struct doca_flow_monitor *monitor, const struct doca_flow_fwd *fwd, uint32_t flags,
    void *usr_ctx, struct doca_flow_pipe_entry **entry) {
  struct doca_flow_actions local;

  if (actions != NULL) {
    local = *actions;
    local.action_idx = action_idx;
  }
  return doca_flow_pipe_add_entry(pipe_queue, pipe, match, actions != NULL ? &local : NULL, monitor,
                                  fwd, flags, usr_ctx, entry);
}

// Same treatment for the hash pipe. The name collides with the real 3.1 function, so the wrapper
// is defined first under its own name and the macro is introduced afterwards — inside the wrapper
// the identifier still refers to the library function.
static inline doca_error_t doca_tut_pipe_hash_add_entry(
    uint16_t pipe_queue, struct doca_flow_pipe *pipe, uint32_t entry_index, uint8_t action_idx,
    const struct doca_flow_actions *actions, const struct doca_flow_monitor *monitor,
    const struct doca_flow_fwd *fwd, uint32_t flags, void *usr_ctx,
    struct doca_flow_pipe_entry **entry) {
  struct doca_flow_actions local;

  if (actions != NULL) {
    local = *actions;
    local.action_idx = action_idx;
  }
  return doca_flow_pipe_hash_add_entry(pipe_queue, pipe, entry_index,
                                       actions != NULL ? &local : NULL, monitor, fwd,
                                       (enum doca_flow_flags_type)flags, usr_ctx, entry);
}

#define doca_flow_pipe_hash_add_entry doca_tut_pipe_hash_add_entry

#endif  // DOCA 3.x < 3.2

#endif  // DOCA_FLOW_COMPAT_H_