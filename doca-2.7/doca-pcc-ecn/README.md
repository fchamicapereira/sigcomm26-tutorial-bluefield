# doca-pcc-ecn — a pure-ECN (DCQCN-style) congestion controller for DOCA PCC

A standalone project — real source under `device/` and `host/`, built the same way `doca-flow/` is
(its own `meson.build`, no runtime vendor-copy step) — that turns NVIDIA's stock `rtt_template`
**DOCA PCC** sample into an **ECN/CNP-driven** reaction-point controller running on the
BlueField-3 **DPA**: the send rate is driven **only** by CNP (multiplicative decrease) and TX
(additive increase); the RTT-based update is disabled.

## In this directory
| file | what |
|---|---|
| `device/algo/rtt_template.c` | The pure-ECN controller algorithm — derived from NVIDIA's BSD-3-Clause `rtt_template` sample; the CNP/TX/RTT handlers are what changed. |
| `device/algo/*.h` | Supporting headers (context struct, tunable params, entry-point declarations) — unmodified from NVIDIA's sample. |
| `device/rp_main.c` | DPA entry point (`doca_pcc_dev_user_algo`/`_init`/`_set_algo_params`) — trimmed from NVIDIA's sample: no NP role, no switch-telemetry, no mailbox, no TX-byte-counter sampling (all unused by this tutorial). |
| `host/pcc_ecn_rp.c` | The host loader — a from-scratch, ~250-line replacement for NVIDIA's ~800-line multi-mode sample host app, exposing just `-d`/`--device` and `-l`/`--log-level`. |
| `meson.build` / `dependencies/meson.build` | Build wiring, following the exact same pattern `doca-flow/` uses. |
| `build_device_code.sh` | Invokes `dpacc` to compile `device/` into the DPA image; our own, much-trimmed version of NVIDIA's `applications/pcc/build_device_code.sh`. |
| `run.sh` | Launches the built controller on the RP (`sudo doca_pcc_ecn_rp -d mlx5_1 -l 50`). |
| `doca_pcc_guide.md` | How it works: host↔DPA split, the event loop, and **how the rate is computed and injected into the NIC**. |
| `doca_pcc_findings.md` | Bring-up runbook: firmware prerequisite, device map, operational gotchas, verified results. |
| `tune_ecn.py` | Sweeps the controller knobs (`tune_ecn.py <dec_permille> <ai_shift> <gate>`; rebuild per value). |
| `plot_ecn_sweep.py` | Regenerates the tuning figure (measured data inline, matplotlib). |
| `doca_pcc_ecn_sweep.pdf` / `.png` | The tuning figure: goodput vs per-CNP decrease factor + the CNP/NACK mechanism. |

## Requirements
- 2× BlueField-3 (one **RP** sender + one **NP** receiver), **DOCA 2.9.x** on the Arm/DPU OS, and the
  DOCA **devel container** `nvcr.io/nvidia/doca/doca:2.9.1-devel` (it has meson / ninja / dpacc).
- Firmware NV-config **`USER_PROGRAMMABLE_CC=1`** (apply via a chip reset — `mlxfwreset -l 3` on the
  bare-metal host). `REAL_TIME_CLOCK_ENABLE` is **not** required.
- A switch between them with **ECN/WRED enabled on the lossless RoCE queue** (DSCP 26 → TC3/Q3).

## Build

Same as every other program in this repo — from the repo root:

```bash
meson setup build
ninja -C build
# => build/doca-pcc-ecn/doca_pcc_ecn_rp
```

Works natively on the Arm *and* inside the tutorial devel container (see the top-level
[`Dockerfile`](../Dockerfile)). `build_device_code.sh` checks what's actually installed (untargeted
vs per-target `libdoca_pcc_dev*.a`, whether `dpacc` takes `-mcpu`) rather than branching on a DOCA
version string, so the same source builds against DOCA 2.7 (this tutorial's DPU) or 2.9 (the dev
container) unchanged — but **build wherever you intend to run**: a DPA image built against a newer
DOCA's `dpacc`/`libflexio` can be rejected by older firmware at *start* (`Application OS version
(...) is greater than device OS version (...)`), even though the build itself succeeds. See the
top-level README's [Building and running DOCA PCC (Part IV)](../README.md#building-and-running-doca-pcc-part-iv)
section for the full explanation.

[`run.sh`](run.sh) starts the result on the RP:

```bash
./run.sh            # => sudo doca_pcc_ecn_rp -d mlx5_1 -l 50
```

## Run (⚠️ notes below — this is from the original 2×BF3 bring-up; see the top-level README's
[Combined run](../README.md#building-and-running-doca-pcc-part-iv) for this tutorial's actual
single-DPU loopback commands, which don't need a separate NP role or `--tclass`)
```bash
# Receiver (NP) — not used in this tutorial's single-DPU setup (receiver HW CNP is enough):
sudo timeout 40 stdbuf -oL build/doca-pcc-ecn/doca_pcc_ecn_rp -d mlx5_1 -l 50 > np.log 2>&1

# Sender (RP) — run FOREGROUND, then drive traffic within the window:
sudo timeout 40 stdbuf -oL build/doca-pcc-ecn/doca_pcc_ecn_rp -d mlx5_1 -l 50 > rp.log 2>&1 &

# Traffic MUST use -R and --tclass=104:
ib_write_bw -d mlx5_3 -R -F --report_gbits --tclass=104 -p 18515             # server (NP host)
ib_write_bw -d mlx5_2 -R -F --report_gbits --tclass=104 -D 20 -p 18515 <ip>  # client (RP host)

grep PURE_ECN rp.log                # rate walking down as CNPs arrive => the loop is live
sudo pkill -INT -x doca_pcc_ecn_rp  # stop gracefully (never SIGKILL: leaves a ghost DPA context)
```

### Critical notes (each one silently breaks the demo if missed)
1. **`-R` (rdma_cm) is mandatory.** QP→algo binding is negotiated via RoCE **ECE**; without `-R` the QP
   uses the built-in default algo and your code (algo slot 0) never runs.
2. **`--tclass=104` (DSCP 26 → Q3) is mandatory** on a setup with switch-based WRED (the original
   2×BF3 testbed). Not needed in this tutorial's single-DPU loopback, where `doca_flow_ecn` marks CE
   unconditionally regardless of traffic class.
3. **Run the controller in the foreground** and drive traffic during that window. Point it at the
   uplink **PF** (`mlx5_1`); RoCE traffic uses the **SF** (`mlx5_3`). Stop only with `pkill -INT`.

## Tune — the one-line knob
```c
#define ECN_CNP_DEC_FACTOR (((1 << 16) * 900) / 1000)  /* x0.90 per CNP; 800..995 = x0.80..x0.995 */
```
Measured on a 2→1 incast into a 100G lossless sink:

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

## Revert
`device/algo/rtt_template.c` is a plain, git-tracked file now (no patch to apply/revert) — use
normal git history, e.g. `git log -- doca-pcc-ecn/device/algo/rtt_template.c` to see the change, or
`git checkout <rev> -- doca-pcc-ecn/device/algo/rtt_template.c` to restore an earlier version.
