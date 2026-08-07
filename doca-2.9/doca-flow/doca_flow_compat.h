#ifndef DOCA_FLOW_COMPAT_H_
#define DOCA_FLOW_COMPAT_H_

#include <doca_version.h>
#include <doca_flow.h>

/* DOCA 2.9 renamed doca_flow_query -> doca_flow_resource_query (struct wrapped in a `counter`
 * union member), doca_flow_query_entry -> doca_flow_resource_query_entry, and
 * doca_flow_shared_resource_cfg -> doca_flow_shared_resource_set_cfg. Shim the old (pre-2.9)
 * names to the new call sites this directory's .c files use, so the same source builds against
 * either DOCA release. Force-included via -include in meson.build, not #include'd directly.
 */
#if DOCA_VERSION_MAJOR == 2 && DOCA_VERSION_MINOR < 9

struct doca_flow_resource_query {
	struct doca_flow_query counter;
};

static inline doca_error_t doca_flow_resource_query_entry(struct doca_flow_pipe_entry *entry,
							    struct doca_flow_resource_query *query)
{
	return doca_flow_query_entry(entry, &query->counter);
}

#define doca_flow_shared_resource_set_cfg doca_flow_shared_resource_cfg

#endif /* DOCA < 2.9 */

#endif /* DOCA_FLOW_COMPAT_H_ */
