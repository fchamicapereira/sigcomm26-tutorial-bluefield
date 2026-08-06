# doca-pcc-ecn — a pure-ECN (DCQCN-style) congestion controller for DOCA PCC

`pureecn_dcqcn.patch` turns NVIDIA's stock `rtt_template` **DOCA PCC** example into an
**ECN/CNP-driven** reaction-point controller that runs on the BlueField-3 **DPA**: the send rate is
driven **only** by CNP (multiplicative decrease) and TX (additive increase); the RTT-based update is
disabled. ~40 lines against one file.

## In this directory
| file | what |
|---|---|
| `pureecn_dcqcn.patch` | The patch — clean 4-hunk unified diff vs the stock DOCA PCC `rtt_template.c`. |
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

## Apply + build
```bash
cp -a /opt/mellanox/doca/applications ~/doca_devel/applications   # writable copy (/opt is read-only)
cd ~/doca_devel/applications
patch -p1 < /path/to/pureecn_dcqcn.patch                          # 1 file, 4 hunks

sudo docker run --rm -v /home/ubuntu/doca_devel:/doca_devel -v /dev/hugepages:/dev/hugepages \
  --privileged --net=host nvcr.io/nvidia/doca/doca:2.9.1-devel \
  bash -lc 'cd /doca_devel/applications && rm -rf /doca_devel/build && \
            meson setup /doca_devel/build -Denable_all_applications=false -Denable_pcc=true && \
            ninja -C /doca_devel/build'
# => ~/doca_devel/build/pcc/doca_pcc
```

## Run  (⚠️ two non-obvious requirements — see notes)
```bash
# Receiver (NP):
sudo timeout 40 stdbuf -oL ~/doca_devel/build/pcc/doca_pcc -d mlx5_1 -np-nt -l 50 > np.log 2>&1

# Sender (RP) — run FOREGROUND, then drive traffic within the window:
sudo timeout 40 stdbuf -oL ~/doca_devel/build/pcc/doca_pcc -d mlx5_1 -l 50 > rp.log 2>&1 &

# Traffic MUST use -R and --tclass=104:
ib_write_bw -d mlx5_3 -R -F --report_gbits --tclass=104 -p 18515             # server (NP host)
ib_write_bw -d mlx5_2 -R -F --report_gbits --tclass=104 -D 20 -p 18515 <ip>  # client (RP host)

grep PURE_ECN rp.log            # rate walking down as CNPs arrive => the loop is live
sudo pkill -INT -x doca_pcc     # stop gracefully (never SIGKILL: leaves a ghost DPA context)
```

### Critical notes (each one silently breaks the demo if missed)
1. **`-R` (rdma_cm) is mandatory.** QP→algo binding is negotiated via RoCE **ECE**; without `-R` the QP
   uses the built-in default algo and your code (algo slot 0) never runs.
2. **`--tclass=104` (DSCP 26 → Q3) is mandatory.** Default traffic rides a lossy queue with no ECN
   marking → no CNPs to react to (and go-back-N collapse). Q3 is lossless (PFC) + ECN-marked.
3. **Run `doca_pcc` in the foreground** and drive traffic during that window. Point it at the uplink
   **PF** (`mlx5_1`); RoCE traffic uses the **SF** (`mlx5_3`). Stop only with `pkill -INT`.

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
```bash
patch -R -p1 < /path/to/pureecn_dcqcn.patch    # or: restore rtt_template.c from .bak, then rebuild
```
