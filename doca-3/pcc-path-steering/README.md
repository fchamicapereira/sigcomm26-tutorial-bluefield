# PCC path steering

This PCC application reports per-flow rate information from the DPA to the host
through the PCC trace stream. The custom RTT-template / Pure-ECN algorithm keeps
calculating its congestion-derived per-QP rate, but that value is a steering
signal only: it is traced to the host and retained in algorithm context while
the event result returned to PCC hardware is always `DOCA_PCC_DEV_MAX_RATE`.
The ordinary PCC per-QP rate limiter therefore does not throttle traffic.

## Trace format

Format ID `6` is reserved in `device/pcc_rate_report.h`:

```c
#define PCC_RATE_REPORT_FORMAT_ID (6)
```

The DPA emits it with:

```c
doca_pcc_dev_trace_5(PCC_RATE_REPORT_FORMAT_ID,
                     qpn, steering_rate, ev_type, rtt, now);
```

The five trace arguments are:

| Argument | Value |
| --- | --- |
| 1 | QPN |
| 2 | calculated steering rate, in FXP20 format; not the enforced hardware rate |
| 3 | PCC event type |
| 4 | RTT value from the algorithm context |
| 5 | DPA low timer value, in microseconds |

For example, `1048576` (`1 << 20`) is the configured `NEW_FLOW_RATE` currently observed in the Pure-ECN test.

## Host decoding and aggregation

`host/pcc_core.c` registers `rate_report_trace_handler()` with `doca_pcc_register_trace_handler()`.

`struct doca_pcc_bin_report` is opaque in the public PCC API. It is 64 bytes in
both supported 3.x ABIs, but its argument offset differs:

- DOCA 3.1 uses the legacy FlexIO layout: message/sequence, combined
  thread/timestamp metadata, then six arguments beginning at byte 16.
- Runtime validation on DOCA 3.4 showed message/sequence, metadata, an internal
  timestamp, then five arguments beginning at byte 24.

The host selects the layout at compile time using `DOCA_VERSION_MAJOR/MINOR`.
The effective DOCA 3.4 layout is:

```c
struct pcc_trace_report {
    uint32_t msg_number;
    uint32_t seq_number;
    uint64_t metadata;
    uint64_t internal_timestamp;
    uint64_t args[5];  /* trace_5 argument 1 through 5 */
};
```

This differs from FlexIO's `struct msg_bin_report`, whose `args` array begins at byte 16. Casting PCC reports to that FlexIO layout shifts the arguments and produces invalid QPN/rate values.

The host filters format ID 6 records and tracks up to 256 QPNs. For every QPN it keeps:

- cumulative rate sum;
- number of received rate reports;
- most recently received rate.

On BF3, `doca_pcc_dev_get_flow_qpn()` is valid only for RoCE TX events. The
device program caches that sender-local QPN in the per-flow algorithm context;
CNP/ACK/NACK rate changes are attributed through the cached owner. Feedback
events received before an owner TX event are not emitted as per-QPN reports.
This avoids interpreting the feedback-event union as a TX QPN field.

When a rate report arrives and at least one DPA-timer second has elapsed since the prior summary, the host prints a cumulative summary:

```text
--- Per-flow rate averages (received=129767 total=1427306) ---
  QPN 0x63f: avg_rate=1048576 last_rate=1048576 updates=713651
  QPN 0x640: avg_rate=1048576 last_rate=1048576 updates=713650
---
```

`received` is the number of accepted format-6 reports since the previous summary; `total` is the number accepted since PCC startup. `updates` is the cumulative count for that QPN.

The host prints only when rate reports are received. Consequently, a single flow that sends one startup report and has no later rate changes prints its startup summary once; it does not produce periodic output without later trace events.

## Validated test behavior

With BF3 as the PCC sender and CX7 as the receiver, `ib_write_bw` traffic assigned to custom slot 0 produces stable consecutive traffic QPNs. RTT probing is disabled in the current Pure-ECN algorithm, so the observed traffic is TX-only rather than polluted by RTT-probe QPs.

The trace route has been validated end to end:

1. DPA emits format-6 records.
2. `doca_pcc_dev_trace_flush()` delivers records to the host callback.
3. The host decodes QPN and FXP20 rate correctly.
4. The host aggregates reports and prints per-QPN averages once per second.

## Current limitation: DPA report volume

The current startup-detection experiment uses reserved RTT-template context words to tag the QPN that owns a cloned context. PCC context cloning/sharing can cause the tag to alternate between active QPNs. In a multi-flow run this currently causes a format-6 report and a startup flush on many TX events, yielding high report rates (for example, about 129k reports in one host summary interval).

This behavior is useful for validating the trace transport and host aggregation, but it is **not** the intended production stream rate. The next DPA-side refinement should reduce reporting to:

1. one initial report per real flow; and
2. one report for each actual PCC rate change.

Do not flush on every TX or every rate report in the production path: `doca_pcc_dev_trace_flush()` is intended to flush partial trace buffers and frequent calls can reduce DPA performance.

## Build

Build both host and DPA code:

```bash
meson setup build
ninja -C build
```

The DPA archive is a Ninja custom target. Changes to the device C sources,
headers, attributes YAML, or `build_device_code.sh` automatically rerun
`dpacc` during an ordinary `ninja -C build`; no Meson wipe or reconfigure is
required.

## Embedded DOCA Flow path steering

For the BF3 experiment, the PCC executable runs the **DOCA Flow path-steering
module** (`steering/steer.c`) in the **same host process**, in the **egress
role**: because the PCC RP runs on the sender, `doca_pcc` sets the DSCP path marker
independently on each sender packet at SF egress. Enable it by passing the sender
SF representor with `-r` (DOCA 3.x), which also opens the PF device and SF rep the
steering datapath needs:

```bash
# sender (PCC RP + embedded egress steering), PF1 / pf1sf0:
sudo ./build/doca_pcc --device mlx5_1 -r pci/0000:03:00.1,pf1sf0 <PCC args...>

# receiver side runs the ingress half as a separate standalone instance, PF0:
sudo ./build/doca_flow_steer \
  -r pci/0000:03:00.0,pf0sf0 -R pci/0000:03:00.0,pf0sf4 \
  --path0-ip 10.0.0.1 --path1-ip 10.0.0.11 --role ingress
```

DOCA 2.9 uses the same `-r`/`-R` representor syntax. Its callbacks parse
the BDF and SF numbers and translate them to legacy mlx5 probe devargs; complete
commands are in [`steering/README.md`](steering/README.md).

There is no rate file, external IPC, or manual QPN-to-path configuration. On DOCA
3.x, each format-6 trace report calls `steer_update_pcc_rate(qpn, rate)` directly.
On DOCA 2.9, the host retrieves the latest per-QPN rates from the PCC DPA mailbox
once per second and feeds the same steering API. `steer_poll()` applies the calculated path share and prints the datapath counters.

> **Marker mechanism.** The selected virtual path is marked on the wire with an
> ICRC-exempt **DSCP bit** (masked modify), *not* a UDP-port rewrite — rewriting the
> RoCEv2 UDP port breaks ICRC and drops rewritten traffic. See
> [`steering/README.md`](steering/README.md).

> **Status.** The standalone ingress and embedded PCC egress paths are
> hardware-validated on DOCA 2.9, 3.1, and 3.4; DOCA 2.7 is not supported yet.
> On 2.9, embedded steering reuses
> the PCC context's open `doca_dev` handle before probing its sender SF.

On 3.x, the host-side per-QPN PCC trace summary remains the primary rate-feed
visibility point:

```text
--- Per-flow rate averages (received=... total=...) ---
  QPN 0x63f: avg_rate=1048576 last_rate=1048576 updates=...
  QPN 0x640: avg_rate=786432 last_rate=786432 updates=...
---
```

The steering pipeline, options, and the DOCA 2.9/3.x compatibility notes are
documented in [`steering/README.md`](steering/README.md). The same unified build
also produces the standalone `doca_flow_steer` binary for testing.

> The former `peer_sim` sender/DRR component has been removed; the sender is now
> just a RoCE traffic generator (e.g. `ib_write_bw`), and DOCA Flow performs the
> PCC-informed path steering.
