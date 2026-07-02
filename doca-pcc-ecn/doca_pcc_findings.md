# DOCA PCC on BlueField-3 — Findings & Runbook

*Programmable Congestion Control (DOCA PCC) brought up and characterized on a 2× BlueField-3 testbed.
Companion to `README.md` (apply/build/run) and `doca_pcc_guide.md` (how it works).*

---

## 1. Objective

Build and run the NVIDIA **DOCA PCC** (Programmable Congestion Control) `rtt_template` reaction-point
example on real BlueField-3 hardware, drive RoCE traffic through it, observe the CC engine per event,
and turn it into an **ECN/CNP-driven** controller.

## 2. Environment

| Item | Value |
|---|---|
| DPU | 2× NVIDIA BlueField-3 B3220 (PSID `MT_0000000884`), FW **32.43.2402** |
| SW stack (on Arm/DPU OS) | DOCA **2.9.1008**, MLNX_OFED 24.10, DPACC 1.9.0, MFT **4.30.1** |
| Build env | DOCA devel container `nvcr.io/nvidia/doca/doca:2.9.1-devel` (arm64, ~2 GB) |
| RP BF3 Arm (DPU OS) | `ubuntu@<bf3-rp-arm>` — 2 ports: **p0→sw-portA, p1→sw-portB** |
| NP BF3 Arm (DPU OS) | `ubuntu@<bf3-np-arm>` — 1 cabled port: **p1→sw-portC** |
| Host side (BF3 PCIe-passthrough VM) | `<user>@<host-vm>`; MFT installed here for `mlxconfig` |
| Bare-metal hypervisor | `<hypervisor>` — where the chip reset (`mlxfwreset -l 3`) is run |
| Data-plane switch | SONiC `admin@<switch>` (ECN/WRED on the lossless RoCE queue) |

**Device mapping (Arm, switchdev/DPU mode):**
- Uplink PFs: `mlx5_0` = p0, `mlx5_1` = p1 — **this is where PCC runs** (point `doca_pcc -d mlx5_1`).
- Data SFs: `mlx5_2` = `enp3s0f0s0` (p0), `mlx5_3` = `enp3s0f1s0` (p1) — **carry the RoCE QPs**
  (perftest uses these). PCC is *global per NIC*: the PF context governs the SF flows. PCC is not
  offered on the SFs themselves.

## 3. Build flow (devel container)

The system tree `/opt/mellanox/doca/applications/pcc` is **read-only/pristine**. Work on a copy:

```bash
cp -a /opt/mellanox/doca/applications ~/doca_devel/applications
sudo docker run --rm -v /home/ubuntu/doca_devel:/doca_devel \
  -v /dev/hugepages:/dev/hugepages --privileged --net=host \
  nvcr.io/nvidia/doca/doca:2.9.1-devel \
  bash -lc 'cd /doca_devel/applications && \
            meson setup /doca_devel/build -Denable_all_applications=false -Denable_pcc=true && \
            ninja -C /doca_devel/build'
# => binary: ~/doca_devel/build/pcc/doca_pcc
```
(Only the devel container has meson/ninja/dpacc. On an air-gapped Arm, `docker save | ssh … docker load`.)

## 4. Firmware prerequisite: `USER_PROGRAMMABLE_CC`

DOCA PCC requires NV-config **`USER_PROGRAMMABLE_CC=1`** (default 0). With it off, the device rejects
PCC: *"PCC CONFIG object is not supported on this device."*

```bash
mlxconfig -y -d 0000:03:00.0 set USER_PROGRAMMABLE_CC=1   # NV-config only; no firmware flashing
```

- **`REAL_TIME_CLOCK_ENABLE` is NOT needed** (verified — see §9). The RTT loop uses the free-running
  clock. `USER_PROGRAMMABLE_CC=1` is the only knob required.
- **Applying it = a chip reset, and that only works from the bare-metal hypervisor:**
  `mlxfwreset -d /dev/mst/mt41692_pciconf0 -l 3 -y reset` as root on `<hypervisor>`. It **fails** from
  the Arm (*"Synchronization by driver is not supported"*, DPU mode) and from the passthrough VM
  (*"not supported on virtual machines"*). If the hypervisor reports "sync not supported", the BF3 PF
  is bound to `vfio-pci` (VM running) — stop the VM / rebind `mlx5_core`, or cold-reboot.

## 5. Operational lessons (important)

- **Run `doca_pcc` natively on the Arm, in the FOREGROUND.** Background-launched daemons over ssh are
  unreliable; a foreground `sudo timeout <sec> stdbuf -oL doca_pcc …` stays Active for the whole run.
- **`stdbuf -oL`** is required — otherwise `doca_pcc` block-buffers stdout and "Standby"/event logs
  look stuck. With it, the log is live and shows `Standby → Active`.
- **Stop only with graceful SIGINT** (`pkill -INT -x doca_pcc`). `docker rm -f`/SIGKILL leaves a
  **ghost DPA context** that holds the NIC's Active slot, so new instances stay `Standby` (not cleared
  by a warm reboot — only a chip reset).
- **Device:** PCC on the uplink **PF** (`mlx5_1`); RoCE traffic on the **SF** (`mlx5_3`).

## 6. Data path + queue steering (this matters a lot)

- **Traffic must be steered onto the lossless, ECN-marked queue.** perftest with `--tclass=104`
  sets DSCP 26 → TC3 → **Q3** (PFC-lossless + WRED/ECN). Default traffic rides DSCP 0 → **Q0**, which
  is lossy and unmarked: under incast it loses packets → go-back-N → **throughput collapse**, and no
  ECN marks means the CNP controller has nothing to react to.
- Sanity check on the wire (not an internal eswitch shortcut): NIC `*_phy` counters and the switch
  port counters both increment on the physical ports. (Note: the IB `port_xmit_data` counter does
  *not* track SF-on-uplink traffic — use mlx5 `*_phy` or switch counters.)

## 7. PCC is engaged — congestion experiment

2→1 incast (two senders on the RP host → one NP host = the 2:1 bottleneck), RP algo on the sender,
NP on the receiver, all on Q3 (`--tclass=104`):

- **With a running RP handler:** the two flows are **fair and high** (≈43/43 Gb/s ≈ 86–88 Gb/s agg).
- **With the RP handler stopped:** the flows **collapse** and are unfair.

→ The programmable CC is unambiguously in the datapath.

## 8. Instrumentation (custom DPA tweak)

Edited the **copy** (`~/doca_devel/applications/...`; `/opt` untouched; `.bak` kept):
- `rp_rtt_template_dev_main.c` → `doca_pcc_dev_user_algo()`: per-event-**type** counters + periodic
  `PCC_STATS` printf (`doca_pcc_dev_printf` / `doca_pcc_dev_trace_flush`).
- `algo/rtt_template.c` → the pure-ECN controller + `PURE_ECN`/`PCC_*_TRACE` markers.

The shareable `pureecn_dcqcn.patch` here is the clean, minimal version of the controller change.

## 9. Key finding — the custom algo only runs with `-R` (ECE); RTT is NOT firmware-gated

The QP→algo-slot binding is negotiated via RoCE **ECE**, which perftest performs only with **`-R`
(rdma_cm)**. Without `-R`, QPs silently use the built-in `doca_pcc_dev_default_internal_algo`
(a non-zero slot) and the slot-0 custom code never runs — which earlier looked like "0 RTT events /
CC has no effect."

**With `-R` (and `USER_PROGRAMMABLE_CC=1`, `REAL_TIME_CLOCK_ENABLE=0`):**
- The custom slot-0 algo runs; **RTT events fire abundantly** (~950k RTT events in a 15 s incast,
  with real latencies, e.g. ~4.8 µs idle → ~40 µs congested), and CNP/NACK events fire too.
- This **supersedes an earlier (incorrect) conclusion** that "RTT is firmware-gated / needs
  `REAL_TIME_CLOCK_ENABLE`." It was never a firmware gate — it was the missing `-R`/ECE binding plus
  traffic riding the wrong (unmarked) queue (§6).

## 10. From RTT-based to ECN-based

The stock `rtt_template` sets the rate from RTT/queueing delay (`algorithm_core()`). We made it a
**pure-ECN (DCQCN-style)** controller: rate driven only by **CNP** (multiplicative decrease) and
**TX** (additive increase), with RTT measured & logged but decoupled. See `doca_pcc_guide.md` for the
code walkthrough and `README.md` for the tuning table (sweet spot ×0.90 → ~88.5 Gb/s fair).

A subtlety worth demonstrating: with a healthy RTT controller the queue stays shallow, so the switch
rarely ECN-marks and CNPs are sparse. Disabling the RTT reaction lets the queue build past the WRED
threshold, so CNPs fire steadily and the ECN loop takes over — the point of the exercise.

## 11. Restore checklist (back to factory)

- [ ] Restore the algo file from `.bak` (or `patch -R`) and rebuild in the container.
- [ ] `USER_PROGRAMMABLE_CC` `1 → 0` on both BF3 (chip reset via `mlxfwreset -l 3` on the hypervisor).
- [ ] Remove any test IPs added to the SFs; stop `doca_pcc` (`pkill -INT -x doca_pcc`) on both.
- [ ] (Optional) uninstall MFT/dkms from the host VM if it was added just for `mlxconfig`.

## 12. Quick reproduce

```bash
# Receiver (NP):
sudo timeout 40 stdbuf -oL ~/doca_devel/build/pcc/doca_pcc -d mlx5_1 -np-nt -l 50 > np.log 2>&1
# Sender (RP), foreground:
sudo timeout 40 stdbuf -oL ~/doca_devel/build/pcc/doca_pcc -d mlx5_1 -l 50 > rp.log 2>&1 &
# Traffic (MUST use -R and --tclass=104):
ib_write_bw -d mlx5_3 -R -F --report_gbits --tclass=104 -p 18515              # server (NP host)
ib_write_bw -d mlx5_2 -R -F --report_gbits --tclass=104 -D 20 -p 18515 <ip>   # client (RP host)
grep PURE_ECN rp.log ; sudo pkill -INT -x doca_pcc
```
