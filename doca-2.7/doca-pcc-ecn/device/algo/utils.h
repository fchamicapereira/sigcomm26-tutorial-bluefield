/*
 * Small device-side helper macros (branch hints, MIN/MAX) used by rtt_template.c.
 * Not DOCA-specific; kept local instead of pulling in DOCA's applications/common tree.
 */

#ifndef DOCA_PCC_ECN_DEVICE_UTILS_H_
#define DOCA_PCC_ECN_DEVICE_UTILS_H_

#ifndef MIN
#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#endif

#ifndef MAX
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))
#endif

#ifndef likely
#define likely(x) __builtin_expect((x), 1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect((x), 0)
#endif

#endif /* DOCA_PCC_ECN_DEVICE_UTILS_H_ */
