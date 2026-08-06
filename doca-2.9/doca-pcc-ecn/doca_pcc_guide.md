# DOCA PCC on BlueField-3 — Complete Guide: How the Pure-ECN Controller Works

*From event to rate to hardware. Companion to `doca_pcc_findings.md` and the tuning figure
`doca_pcc_ecn_sweep.pdf`. Prepared for the SIGCOMM tutorial.*

---

## 1. The big picture

DOCA PCC (Programmable Congestion Control) lets you write the **congestion-control algorithm of a
RoCE NIC in C** and run it on the BlueField-3 **DPA** (Data-Path Accelerator — a set of small,
massively-threaded RISC cores inside the NIC). There are two halves:

```
        HOST / Arm (control plane)                    NIC hardware (data plane)
   ┌───────────────────────────────┐          ┌───────────────────────────────────────┐
   │  doca_pcc process             │  loads   │  DPA execution units run YOUR algo    │
   │  (build/pcc/doca_pcc)         │ ───────► │  doca_pcc_dev_user_algo() per EVENT   │
   │  - opens the PCC context      │          │  ▲ events: TX / RTT / CNP / NACK      │
   │  - loads the DPA program      │          │  │                                    │
   │  - prints logs, stays "Active"│          │  ▼ writes results->rate               │
   └───────────────────────────────┘          │  NIC per-QP RATE LIMITER  ◄── injected│
                                              └───────────────────────────────────────┘
```

- The **host `doca_pcc` binary is only a loader / babysitter.** It uploads the compiled DPA program,
  opens the PCC context, and keeps it "Active." It does **not** run the algorithm.
- The **algorithm runs entirely on the DPA**, invoked **once per congestion-relevant event** for
  each flow. That is the function `doca_pcc_dev_user_algo()`.
- Two roles: **RP (Reaction Point** = the sender that adjusts its rate) and **NP (Notification
  Point** = the receiver side / telemetry). Our controller is the **RP**.

Why our custom code only runs with perftest `-R`: each QP negotiates (via RoCE **ECE**) which
**algo slot** it uses. Slot 0 = our code; anything else = the built-in default algo. `-R`
(rdma_cm) is what performs that negotiation — without it, QPs silently use the built-in algo and our
handlers never fire.

---

## 2. Program structure (device side)

Two source files, compiled by **DPACC** (`dpacc -mcpu=nv-dpa-bf3`) into a DPA program the host loads:

| file | role |
|---|---|
| `device/rp/rtt_template/rp_rtt_template_dev_main.c` | entry point `doca_pcc_dev_user_algo()`; dispatch by algo-slot |
| `device/rp/rtt_template/algo/rtt_template.c` | the actual algorithm (per-event handlers) — **this is what we edited** |
| `device/rp/rtt_template/algo/rtt_template_algo_params.h` | tunable constants (AI, rates, thresholds) |

### 2.1 The entry point (per-event, per-flow)

```c
void doca_pcc_dev_user_algo(doca_pcc_dev_algo_ctxt_t *algo_ctxt,   // per-FLOW saved state
                            doca_pcc_dev_event_t     *event,        // opaque event handle
                            const doca_pcc_dev_attr_t *attr,        // algo_slot, ...
                            doca_pcc_dev_results_t   *results)      // OUT: rate + rtt_req
{
    uint32_t *param   = doca_pcc_dev_get_algo_params(port_num, attr->algo_slot); // tunables
    ...
    switch (attr->algo_slot) {
    case 0:  rtt_template_algo(event, param, counter, algo_ctxt, results); break; // OUR CODE
    default: doca_pcc_dev_default_internal_algo(algo_ctxt, event, attr, results); // built-in
    }
}
```

Key ideas:
- The DPA runtime calls this **for every event of every flow**, handing you that flow's **saved
  context** (`algo_ctxt`) — you read the last rate from it and write the new one back.
- `results` is the **output**: you fill `results->rate` (the new target rate) and `results->rtt_req`
  (whether to trigger an RTT probe). That's the *entire* contract with the hardware.

### 2.2 The per-flow context

`algo_ctxt` is cast to `cc_ctxt_rtt_template_t`. The field that matters for rate is **`cur_rate`** —
the flow's current target rate, persisted across events. Also holds RTT bookkeeping
(`rtt_meas_psn`, `start_delay`, `abort_cnt`) and `flags.was_cnp / was_nack`.

---

## 3. How rate is represented

From `doca_pcc_dev.h`:

```c
typedef struct { uint32_t rate; uint32_t rtt_req; ... } doca_pcc_dev_results_t;

#define DOCA_PCC_DEV_LOG_MAX_RATE (20)                 // rate is FIXED-POINT-20
#define DOCA_PCC_DEV_MAX_RATE     (1U << 20)           // = 1,048,576 == 100% of line rate
#define DOCA_PCC_DEV_DEFAULT_RATE (MAX_RATE >> 8)      // = 4096  (~0.39% line) initial probe rate
```

**`rate` is a normalized fixed-point-20 fraction of line rate.**

| `results->rate` | meaning (on a 100G port) |
|---|---|
| `1 << 20` = 1048576 | 100% ≈ 100 Gb/s |
| `524288` | 50% ≈ 50 Gb/s |
| `104858` | 10% ≈ 10 Gb/s |
| `MIN_RATE = 1<<6` = 64 | ~0.006% (floor) |

Relevant constants (`rtt_template_algo_params.h`):

```c
#define AI            (((1 << 20) * 5) / 100)  //  52428  = +5% of line rate (additive-increase unit)
#define NEW_FLOW_RATE (1 << 20)                //  full line rate for a brand-new flow
#define MIN_RATE      (1 << (20 - 14))          //  64     = rate floor
#define RATE_MAX      (1 << 20)                //  cap
#define BASE_RTT      13000                     //  13 µs  (RTT target — used by the ORIGINAL algo)
#define MAX_DELAY     150000                    //  150 µs (delay ceiling — original algo)
```

Fixed-point multiply helper (`doca_pcc_dev_utils.h`):

```c
// a is fxp16, b is fxp20  ->  (a*b)>>16  keeps the fxp20 scale
#define doca_pcc_dev_fxp_mult(a, b)  ((uint32_t)((doca_pcc_dev_mult((a),(b)) >> 16) & 0xffffffff))
```

So a "×0.90" factor is expressed in **fxp16**: `((1<<16) * 900) / 1000 = 58982`, and
`fxp_mult(58982, cur_rate) = 0.90 * cur_rate`.

---

## 4. How we calculate the rate — the pure-ECN control loop

The dispatcher picks a handler by event type:

```c
void rtt_template_algo(event, param, counter, algo_ctxt, results) {
    uint32_t cur_rate = ctx->cur_rate;                 // 1) load this flow's current rate
    if (cur_rate == 0)                    handle_new_flow(...);   // first ever event
    else if (ev == ROCE_TX)              handle_roce_tx(...);    // a packet was sent  -> INCREASE
    else if (ev == RTT)                  handle_roce_rtt(...);   // RTT sample arrived -> LOG ONLY
    else if (ev == ROCE_CNP)             handle_roce_cnp(...);   // ECN congestion sig -> DECREASE
    else if (ev == ROCE_NACK)            handle_roce_nack(...);  // loss/retransmit
}
```

Event-type enum: `CNP=2, TX=3, ACK=4, NACK=5, RTT=6` (ACK is masked off by
`disable_event_bitmask=0x10`).

Our **pure-ECN** controller is a textbook **DCQCN-style loop**: rate goes **down** on ECN
congestion signals (CNP), **up** slowly otherwise (TX), and **RTT is observed but does not steer the
rate.**

### 4.1 New flow → start at full rate

```c
static void rtt_template_handle_new_flow(...) {
    ctx->cur_rate  = param[NEW_FLOW_RATE];   // = 1<<20 = 100% line rate
    results->rate  = param[NEW_FLOW_RATE];
    ...
}
```

### 4.2 Decrease — the ECN reaction (CNP)  ← **the heart of it**

A **CNP** (Congestion Notification Packet) arrives at the sender when the switch **ECN-marked** a
data packet (queue built past the WRED threshold) and the receiver echoed it back. Each CNP →
multiplicative decrease:

```c
static void rtt_template_handle_roce_cnp(..., uint32_t cur_rate, ctx, results) {
    ctx->flags.was_cnp = 1;
    /* === ECN/CNP controller: multiplicative decrease per CNP, floored at MIN_RATE === */
    cur_rate = doca_pcc_dev_fxp_mult(ECN_CNP_DEC_FACTOR, cur_rate);   // cur_rate *= 0.90 (tuned)
    if (cur_rate < MIN_RATE) cur_rate = MIN_RATE;
    ...
    results->rate  = cur_rate;    // <-- new target rate
    ctx->cur_rate  = cur_rate;    // <-- persist for next event
}
```

with (tuned optimum from the sweep):

```c
#define ECN_CNP_DEC_FACTOR (((1 << 16) * 900) / 1000)   // ×0.90 per CNP  (was ×0.99 at start)
```

So each CNP cuts the rate by 10%: `1048576 → 943718 → 849346 → …`, floored at `MIN_RATE`.

### 4.3 Increase — recovery (TX)

Every data-send event is a chance to nudge the rate back up. We gate it (every 1000th TX) so
recovery is gentle:

```c
static void rtt_template_handle_roce_tx(..., uint32_t cur_rate, ctx, results) {
    ... (RTT-probe bookkeeping: decide results->rtt_req) ...
    /* === ECN mode: gated additive increase (recover when no CNP) === */
    { static uint32_t g_tx_inc = 0;
      if ((++g_tx_inc % 1000) == 0) {
          cur_rate += (AI >> 2);                 // +13107 fxp20 ≈ +1.25% of line rate
          if (cur_rate > RATE_MAX) cur_rate = RATE_MAX;
      } }
    ctx->cur_rate = cur_rate;
    results->rate = cur_rate;                    // <-- new target rate
    results->rtt_req = rtt_req;                  // <-- also ask HW for an RTT sample if due
}
```

**Equilibrium** is where the up-nudges balance the CNP down-cuts — the classic DCQCN sawtooth.
On the 2→1 incast that lands the rate register around ~55% (per flow) with the aggregate near line
rate.

### 4.4 RTT — measured & logged, but decoupled (our key change)

Originally the RTT handler called `algorithm_core()`, which set the rate from RTT/queueing delay.
We **commented that out** so RTT no longer drives the rate:

```c
static void rtt_template_handle_roce_rtt(..., uint32_t cur_rate, param, ctx, results) {
    ... compute rtt = end_ts - start_ts ...
    /* === PURE-ECN mode: RTT is measured & logged but does NOT drive the rate ===
     * Original: cur_rate = algorithm_core(ctx, rtt, cur_rate, param, is_high_tx_util, norm_np_rx_rate);
     * Rate is now driven ONLY by CNP (decrease) and TX (increase). */
    (void)is_high_tx_util; (void)norm_np_rx_rate; (void)param; (void)algorithm_core;
    doca_pcc_dev_printf("PCC_RTT_TRACE ... rtt=%d rate=%u\n", ..., rtt, cur_rate);  // observe only
    ctx->cur_rate = cur_rate;      // unchanged
    results->rate = cur_rate;      // unchanged (no-op write)
    results->rtt_req = 1;          // keep requesting RTT samples so we can keep observing
}
```

`algorithm_core()` (now dormant) is the *original* rtt_template logic for reference — it decreased on
`rtt > BASE_RTT`, hard-cut on CNP/NACK (`CNP_DEC_FACTOR = 1 - 2·UPDATE_FACTOR = ×0.80`), and added
`AI` otherwise. We keep it in the file (referenced via `(void)algorithm_core;` to satisfy
`-Werror -Wunused-function`) so the hybrid is one line away.

---

## 5. How the rate gets injected into hardware

This is the part people find surprising: **the algorithm never programs the shaper directly.**

1. Per event, the DPA runtime calls `doca_pcc_dev_user_algo()` with a fresh `results` struct.
2. Your code writes **`results->rate`** (fxp20 fraction of line rate) and **`results->rtt_req`**.
3. When your function **returns**, the DOCA PCC infrastructure (DPA runtime + NIC PCC engine) reads
   `results->rate` and **programs that flow's hardware rate limiter** — a per-QP committed-rate
   (token-bucket) shaper in the NIC's transmit pipeline. `2^20` maps to the port's max rate, so the
   shaper is set to `rate / 2^20 × line_rate`.
4. From then on the **NIC hardware paces that QP's packets** at the new rate — in the data path, at
   line-speed, with no host or DPA involvement per packet. The next event (TX/CNP/RTT) will invoke
   your algo again to revise it.

And the RTT-probe side:

5. If you set **`results->rtt_req = 1`**, the NIC **piggybacks an RTT measurement request** on an
   outgoing packet. The NP echoes it; when the timestamped response returns, the hardware raises a
   **RTT event** carrying send/receive timestamps — which is what `handle_roce_rtt()` reads via
   `doca_pcc_dev_get_timestamp()` / `..._get_rtt_req_send_timestamp()`.

So the whole loop is: **event → your C handler computes a new `results->rate` → firmware loads it
into the per-QP shaper → hardware paces packets → congestion (or its absence) generates the next
event.** You are writing the *set-point*; the NIC does the actual pacing.

Two consequences we measured:
- The **rate register ≠ goodput.** The shaper set-point averaged ~55% across every tuning, yet
  goodput ranged 44→88 Gb/s. Goodput was governed by **queue depth / retransmission (NACK) waste**,
  not the set-point — which is exactly why a *sharper* per-CNP cut (shallower queue, fewer NACKs)
  *raised* goodput.
- **RTT decoupling is real:** under pure-ECN the queue is deep (RTT ~17–56 µs) yet the rate ignores
  it — proving the rate is driven by CNP alone.

---

## 6. What we changed vs. stock (the revertable delta)

| change | file | why |
|---|---|---|
| per-event-type counters + `PCC_STATS/PCC_DISP/PCC_RTT/CNP/NACK_TRACE` printf | both | observe the event stream (`doca_pcc_dev_printf` + `..._trace_flush`) |
| `ECN_CNP_DEC_FACTOR` + multiplicative decrease in `handle_roce_cnp` | `rtt_template.c` | the ECN reaction (tuned ×0.90) |
| gated additive-increase in `handle_roce_tx` | `rtt_template.c` | recovery |
| commented `cur_rate = algorithm_core(...)` in `handle_roce_rtt` | `rtt_template.c` | **make it ECN-driven, not RTT-driven** |

Backups on the Arm (revert + rebuild to undo): `.bak` = fully stock · `.pre_ecn` = before the ECN
controller · `.pre_pureecn` = the RTT+ECN hybrid · plus `.ecn.patch` / `.pureecn.patch` diffs.
Tuner: `~/tune_ecn.py <dec_permille> <ai_shift> <gate>`.

---

## 7. Build / run / observe (quick reference)

```bash
# BUILD (in the DOCA devel container — only it has meson/ninja/dpacc):
sudo docker run --rm -v /home/ubuntu/doca_devel:/doca_devel -v /dev/hugepages:/dev/hugepages \
  --privileged --net=host nvcr.io/nvidia/doca/doca:2.9.1-devel \
  bash -lc 'cd /doca_devel/applications && rm -rf /doca_devel/build && \
            meson setup /doca_devel/build -Denable_all_applications=false -Denable_pcc=true && \
            ninja -C /doca_devel/build'

# RUN the RP algo — FOREGROUND (stays Active for the whole run; background-launch is unreliable):
sudo timeout 30 stdbuf -oL ~/doca_devel/build/pcc/doca_pcc -d mlx5_1 -l 50 > rp.log 2>&1
#   host4 NP:   doca_pcc -d mlx5_1 -np-nt -l 50
#   stop:       sudo pkill -INT -x doca_pcc   (never SIGKILL — leaves a ghost DPA context)

# DRIVE traffic — MUST use -R (ECE binds the QP to algo slot 0) and --tclass=104 (DSCP26 -> Q3,
#   the lossless+ECN-marked queue):
ib_write_bw -d mlx5_3 -R -F --report_gbits --tclass=104 -p 18515            # server (host4)
ib_write_bw -d mlx5_2 -R -F --report_gbits --tclass=104 -D 20 -p 18515 <ip> # client (host3)

# OBSERVE:
grep PCC_STATS rp.log     # per-type event counts (tx/rtt/cnp/nack)
grep PCC_CNP_TRACE rp.log # rate after each CNP cut
grep PCC_DISP rp.log      # ev-type + rate over time
```

Prerequisite firmware knob (once, via hypervisor `mlxfwreset -l 3`): `USER_PROGRAMMABLE_CC=1`
(RTC **not** needed — verified). Everything else (rate limiter, ECN marking) is standard RoCE.
