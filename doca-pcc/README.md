# `doca_pcc` — standalone pure-ECN (DCQCN-style) PCC controller

A self-contained copy of NVIDIA's DOCA **PCC** (Programmable Congestion Control) reference
application with the tutorial's **pure-ECN** controller baked into the device algorithm — so it
builds like any other app in this repo (`meson setup build && ninja`), with **no patch step and no
copy of the DOCA `applications/` tree**.

This replaces the earlier [`doca-pcc-ecn/`](../doca-pcc-ecn) flow, which regenerated a writable copy
of `/opt/mellanox/doca/applications` and applied [`pureecn_dcqcn.patch`](../doca-pcc-ecn/pureecn_dcqcn.patch)
at build time. Here the change is already **in the source**
([`device/rp/rtt_template/algo/rtt_template.c`](device/rp/rtt_template/algo/rtt_template.c)).

---

## What "pure-ECN" changes

The stock RP `rtt_template` algorithm updates the send rate from RTT (and optionally NP telemetry).
The pure-ECN variant makes it behave like DCQCN — rate is driven **only** by congestion signals on
the wire:

- **CNP → multiplicative decrease**: on each CNP, `rate *= ECN_CNP_DEC_FACTOR` (default `x0.90`,
  fxp16), floored at `MIN_RATE`.
- **gated additive increase**: recover by `AI/4` periodically when CNPs stop.
- **RTT is measured and logged but does *not* drive the rate** (the `algorithm_core()` call is
  disabled; uncomment it to restore stock RTT/hybrid control).

Tune the aggressiveness via `ECN_CNP_DEC_FACTOR` at the top of `rtt_template.c` (`800..995` =
`x0.800 .. x0.995` per CNP; `x0.90` was the tuned sweet spot → ~88.5 Gb/s fair on the loopback).

## Layout

```
doca-pcc/
├── host/               pcc.c, pcc_core.c/.h        (host control plane — unmodified)
├── device/             DPA (device) algorithms compiled by dpacc
│   ├── rp/rtt_template/algo/rtt_template.c   <-- pure-ECN change lives here
│   ├── rp/switch_telemetry/ …
│   └── np/{nic,switch}_telemetry/ …
├── build_device_code.sh   dpacc wrapper (vendored; PCC includes point at THIS tree)
├── pcc_params.json
├── dependencies/meson.build   (doca-argp, doca-pcc, flexio)
└── meson.build            builds 4 DPA programs + links the doca_pcc host binary
```

Unlike the pure-host doca-flow apps, PCC has a **DPA (device) half**. `meson` runs
`build_device_code.sh` (→ `dpacc`, target `nv-dpa-bf3`) at configure time to compile four DPA
programs into host-stub static libraries, then links them into `doca_pcc`:

| DPA program | role |
|---|---|
| `pcc_rp_rtt_template_app` | **RP**, RTT template — **the pure-ECN controller (what the tutorial runs)** |
| `pcc_rp_switch_telemetry_app` | RP, in-band switch telemetry (IFA2) |
| `pcc_np_nic_telemetry_app` | NP, NIC telemetry (CCMAD) |
| `pcc_np_switch_telemetry_app` | NP, switch telemetry |

All four are kept so the host's `--app` options are unchanged; the tutorial only touches the first.

## Build

Requires DOCA installed under `/opt/mellanox/doca` (for `dpacc`, the DPA/`flexio` includes, and the
generic `applications/common/device` headers). Build inside the tutorial devel container or anywhere
DOCA + `meson`/`ninja` are available:

```bash
# from the repo root — builds doca_pcc alongside the doca-flow apps
meson setup build && ninja -C build
# -> build/doca-pcc/doca_pcc
```

`build_device_code.sh` needs `dpacc` on `PATH` (`export PATH=/opt/mellanox/doca/tools:$PATH`) and
`libflexio`/`doca-pcc` on `PKG_CONFIG_PATH`; the container image sets these up.

## Run

PCC binds to the uplink **PF** (`mlx5_0`) — RoCE traffic uses the SFs (see the repo
[README](../README.md) Part IV). It requires the firmware knob `USER_PROGRAMMABLE_CC=1` (applied via
`mlxfwreset` / hypervisor) to be set.

```bash
sudo ./build/doca-pcc/doca_pcc -d mlx5_0        # default --app = RP rtt_template (pure-ECN)
```

The controller prints `PURE_ECN cnp=<n> rate=<r>` periodically as the CNP loop engages. Drive
congesting RoCE traffic with `--tclass=104` (DSCP26→Q3) and `-R` (ECE→slot0) so CNPs are generated.

> **Note:** this program modifies **runtime** rate state only. Reverting the app to stock is just
> uncommenting the `algorithm_core()` call in `rtt_template.c`; returning the DPU to factory means
> setting the firmware `USER_PROGRAMMABLE_CC` back to `0`.
