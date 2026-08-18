# doca-pcc-ecn — a pure-ECN (DCQCN-style) congestion controller for DOCA PCC

A standalone project — real source under `device/` and `host/`, built the same way `doca-flow/` is
(part of the same `doca-N` meson project, no runtime vendor copy-and-patch step) — that turns
NVIDIA's stock `rtt_template` **DOCA PCC** sample into an **ECN/CNP-driven** reaction-point
controller running on the BlueField-3 **DPA**: the send rate is driven **only** by CNP
(multiplicative decrease) and TX (additive increase); the RTT-based update is disabled.

This is Part 2 of the tutorial. The participant-facing walkthrough is
**[`guides/doca-pcc.md`](../../guides/doca-pcc.md)** (rendered to `guides/doca-pcc.pdf`, which is what
participants were handed). This file describes the directory for whoever maintains it.

The DOCA 3.x build of the same controller is [`doca-3/doca-pcc-ecn/`](../../doca-3/doca-pcc-ecn/).

## In this directory

| file | what |
|---|---|
| `device/algo/rtt_template.c` | The pure-ECN controller algorithm — derived from NVIDIA's BSD-3-Clause `rtt_template` sample; the CNP/TX/RTT handlers are what changed. **The answer key.** |
| `device/algo/rtt_template_exercise.c` | The **participant exercise**: a copy of the above with the two rate reactions hollowed out to `TODO 1` (CNP multiplicative decrease) and `TODO 2` (TX additive increase). Builds and runs as shipped, but never moves the rate. **Generated** — see below. |
| `device/algo/*.h` | Supporting headers (context struct, tunable params, entry-point declarations) — unmodified from NVIDIA's sample. |
| `device/rp_main.c` | DPA entry point (`doca_pcc_dev_user_algo`/`_init`/`_set_algo_params`) — trimmed from NVIDIA's sample: no NP role, no switch-telemetry, no mailbox, no TX-byte-counter sampling (all unused by this tutorial). |
| `host/pcc_ecn_rp.c` | The host loader — a from-scratch, ~250-line replacement for NVIDIA's ~800-line multi-mode sample host app, exposing just `-d`/`--device` and `-l`/`--log-level`. |
| `meson.build` / `dependencies/meson.build` | Build wiring, following the exact same pattern `doca-flow/` uses. Declares both executables below. |
| `build_device_code.sh` | Invokes `dpacc` to compile `device/` into the DPA image; our own, much-trimmed version of NVIDIA's `applications/pcc/build_device_code.sh`. Takes the algo source as its 6th argument, which is how the two variants are built from one script. |
| `doca_pcc_guide.md` | How it works: host↔DPA split, the event loop, and **how the rate is computed and injected into the NIC**. |
| `doca_pcc_findings.md` | The original bring-up runbook: firmware prerequisite, device map, operational gotchas, verified results. |
| `plot_ecn_sweep.py` | Regenerates the tuning figure (measured data inline, matplotlib). |
| `doca_pcc_ecn_sweep.pdf` / `.png` | The tuning figure: goodput vs per-CNP decrease factor + the CNP/NACK mechanism. |
| `tune_ecn.py` | Sweeps the controller knobs. **Currently broken** — see [Tuning](#tune--the-one-line-knob). |

> **The two companion docs are historical.** `doca_pcc_guide.md` and `doca_pcc_findings.md` were
> written during the original bring-up on a **2× BlueField-3 testbed with a SONiC switch doing
> WRED/ECN marking**, before the exercise was converged onto a single card. The mechanism they
> explain is still exactly right; the topology, device map and build commands in them are not. For
> current commands use this file and the top-level
> [README](../../README.md#the-pcc-controller-part-2).

## Requirements

- **One BlueField-3**, set up by
  [`admin/local_scripts/setup_roce_loopback.sh`](../../admin/local_scripts/setup_roce_loopback.sh)
  (one SF per PF in its own netns, `mlx5_2`/`mlx5_3`, hugepages). No second card and no external
  switch: the DOCA Flow program from Part 1 does the ECN marking that a switch's WRED did originally.
- **DOCA 2.x on the Arm** (developed against 2.9.x) plus `meson`, `ninja` and `dpacc`. Builds
  natively on the Arm; the devel container (`../Dockerfile`) is an alternative, not a requirement.
- Firmware NV-config **`USER_PROGRAMMABLE_CC=1`** *and* **`DPA_AUTHENTICATION=0`**, both as the
  **Current** (live) value — which needs a real power-cycle, not a reboot. `REAL_TIME_CLOCK_ENABLE`
  is **not** required. Full procedure and the failure modes for each:
  [Firmware NV-config](../../README.md#firmware-nv-config-pcc-prerequisite);
  `admin/fleet.py pcc-ready` reports the state across the fleet.

## Build

Same as every other program in this repo — from the `doca-2` directory:

```bash
cd doca-2
meson setup build
ninja -C build
# => build/doca-pcc-ecn/doca_pcc_ecn_rp           the finished controller
# => build/doca-pcc-ecn/doca_pcc_ecn_rp_template  the exercise (two TODOs unfilled)
```

Both executables share `host/pcc_ecn_rp.c` and differ only in which DPA algorithm is linked in;
`meson.build` gives each its own `dpacc` output directory so the two archives never collide.

`build_device_code.sh` checks what is actually installed (untargeted vs per-target
`libdoca_pcc_dev*.a`, whether `dpacc` takes `-mcpu`) rather than branching on a DOCA version string,
so the same source builds against a range of 2.x releases unchanged — but **build wherever you intend
to run**: a DPA image built against a newer DOCA's `dpacc`/`libflexio` can be rejected by older
firmware at *start* (`Application OS version (...) is greater than device OS version (...)`), even
though the build itself succeeds.

> **Regenerating the exercise.** Do not hand-edit `rtt_template_exercise.c`. It is derived from
> `rtt_template.c` by [`admin/make_pcc_exercise.py`](../../admin/make_pcc_exercise.py):
>
> ```bash
> admin/make_pcc_exercise.py doca-2            # rewrite the exercise from the controller
> admin/make_pcc_exercise.py doca-2 --check    # verify the two are in step
> ```
>
> Its substitutions must each match exactly once, so a drifted controller is a hard error rather than
> a silent no-op. `update_participants_repo_on_github.py` ships the exercise renamed to
> `rtt_template.c` — participants never see a file called "exercise", and never see the answer key.

## Run

The controller is the RP (sender side), so it is pointed at the **PF** uplink of the port the sender
sits behind — `mlx5_1` — while the RoCE traffic itself uses the **SFs** (`mlx5_2`/`mlx5_3`). Start the
Part 1 marker first, then the controller, then drive traffic:

```bash
# 1. ECN marker on PF0 — leave running:
sudo ./build/doca-flow/doca_flow_solution -- --percent 100

# 2. RP controller on PF1 — stays Active for the whole window:
sudo timeout 40 stdbuf -oL ./build/doca-pcc-ecn/doca_pcc_ecn_rp -d mlx5_1 -l 50 > rp.log 2>&1 &

# 3. Drive RoCE traffic across the loopback:
sudo ip netns exec ns0 ib_write_bw -d mlx5_2 -R -x 1 -F --report_gbits --run_infinitely -D 1           # server
sudo ip netns exec ns1 ib_write_bw -d mlx5_3 -R -x 1 -F 10.0.0.1 --report_gbits --run_infinitely -D 1  # client

grep PURE_ECN rp.log                  # rate walking down as CNPs arrive => the loop is live
sudo pkill -INT -x doca_pcc_ecn_rp    # stop gracefully (never SIGKILL: leaves a ghost DPA context)
```

Swap in `doca_pcc_ecn_rp_template` to see the exercise's starting behaviour: it loads and logs
identically, but the rate never moves.

### Critical notes (each one silently breaks the demo if missed)

1. **`-R` (rdma_cm) is mandatory.** QP→algo binding is negotiated via RoCE **ECE**; without `-R` the
   QP uses the built-in default algo and your code (algo slot 0) never runs. Worse, on a card with
   `USER_PROGRAMMABLE_CC=1` the default algo is not observed to reduce rate at all, so the symptom is
   "nothing reacts" rather than "something else reacts" — see the
   [gotcha](../../README.md#firmware-nv-config-pcc-prerequisite) in the top-level README.
2. **`--tclass=104` (DSCP 26 → Q3) is *not* needed here.** It was mandatory on the original 2×BF3
   testbed, where a switch marked CE only on the lossless queue. The Flow program marks CE
   unconditionally, regardless of traffic class. Reach for it only as a diagnostic if CNPs never
   arrive: the receiver's hardware CNP *generation* can be priority-scoped.
3. **Point the controller at the uplink PF, not the SF.** PCC is global per NIC — the PF context
   governs the SF's flows, and PCC is not offered on the SFs themselves.
4. **Stop only with `pkill -INT`.** A `SIGKILL` leaves a ghost DPA context behind that blocks the next
   start.

## Tune — the one-line knob

```c
#define ECN_CNP_DEC_FACTOR (((1 << 16) * 900) / 1000)  /* x0.90 per CNP; 800..995 = x0.80..x0.995 */
```

The other two knobs are in the TX handler: the additive-increase step `cur_rate += (AI >> 2)` and the
gate `(++g_tx_inc % 1000) == 0` that decides how often it applies.

> **`tune_ecn.py` no longer works as written.** It still edits
> `/home/ubuntu/doca_devel/applications/pcc/device/rp/rtt_template/algo/rtt_template.c` — the path the
> algorithm lived at back when it was a patch applied to a copy of the vendor tree. The file is now
> `device/algo/rtt_template.c` in this directory. Its three regex substitutions still match the
> current source and still describe the right three knobs, so it is useful as a record of what was
> swept; point it at the in-tree path before running it. Rebuild after each change.

Measured on a 2→1 incast into a 100G lossless sink (the original 2×BF3 testbed):

| per-CNP cut | aggregate goodput | CNP | NACK | RTT avg |
|---|---|---|---|---|
| ×0.995 | 44 Gb/s | 124k | 130k | 56 µs |
| ×0.99  | 68 Gb/s | 63k | 67k | 31 µs |
| ×0.98  | 80 Gb/s | 31k | 34k | — |
| ×0.95  | 87 Gb/s | 12k | 15k | 32 µs |
| **×0.90** | **88.5 Gb/s** | 5.6k | 7.8k | **17 µs** |
| ×0.80  | 86.7 Gb/s | 2.6k | 3.8k | 6 µs |

**Takeaway:** the rate register sat at ~55% at *every* setting, yet goodput ranged 44→88 Gb/s —
goodput is governed by **queue depth / retransmission (NACK) waste**, not the rate average. A sharper
per-CNP cut keeps the queue shallow → fewer NACKs → higher goodput *and* lower latency. (See
`doca_pcc_ecn_sweep.pdf`.)

## History

`device/algo/rtt_template.c` is a plain, git-tracked file (there is no patch to apply or revert) —
use normal git history, e.g. `git log -- doca-2/doca-pcc-ecn/device/algo/rtt_template.c` to see how it
diverged from NVIDIA's sample.
