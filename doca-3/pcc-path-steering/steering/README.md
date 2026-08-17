# DOCA Flow two-path RoCE steering

This module implements the DOCA Flow datapath for a two-path PCC experiment on
a BlueField-3 eSwitch. Egress assigns every RoCEv2 packet to virtual path 0 or
1. The path is carried in IP ToS bit `0x04` (DSCP bit 0), which is outside the
RoCEv2 invariant CRC. Ingress applies the ECN policy for the selected path,
clears the private path bit, and delivers the packet to the receiver SF selected
by destination IPv4 address.

The UDP destination port is never used as a path marker. Rewriting 4791 breaks
RoCEv2 ICRC validation even if it is restored before SF delivery.

## Topology

The tested setup uses separate sender and receiver PFs connected through the
physical loopback. The receiver has one SF and destination IP per path.

```text
sender PF (--role egress)

 sender SF
    │
 PORT_DEMUX
    │
 EGRESS_ROCE_CHECK ── non-RoCE ───────────────┐
    │ RoCEv2                                  │
 EGRESS_CLASSIFY (64 random buckets)          │
    │ write path bit                          │
 DELIVER_WIRE ◀───────────────────────────────┘

 returning wire traffic
    │
 EGRESS_CNP_COUNT ── QP1 clone/CM parser ── DELIVER_SF


receiver PF (--role ingress)

 wire
  │
 PORT_DEMUX
  │
 INGRESS_ROCE_CHECK ── non-RoCE ──────────────┐
  │ RoCEv2                                    │
 INGRESS_PATH_DEMUX (DSCP path bit)           │
  ├─ path0 ─ PATH0_IP_MATCH ─ SAMPLE ─ CE_MARK│
  └─ path1 ─ PATH1_IP_MATCH ─ SAMPLE ─ CE_MARK│
                    misses ─ INGRESS_CLEAR_PATH
                              │
                       RECEIVER_IP_DEMUX
                         ├─ destination IP 0 ─ SF0
                         └─ destination IP 1 ─ SF1
```

QP1 packets are cloned in both directions to one DPDK RX queue. The host parses
RDMA-CM REQ packets to learn `(sender QPN, destination IP)`, which directly
assigns each PCC sender QPN to path 0 or path 1. REP parsing records the
responder QPN for diagnostics; it is not required for path grouping.

## Egress control

`EGRESS_CLASSIFY` is a basic exact-match pipe over the low six bits of
`parser_meta.random`. It has 64 live bucket rules and capacity for 128 rules;
the spare capacity is required because HWS temporarily allocates a replacement
rule during an entry update.

The initial assignment is 32 buckets per path. Once per host poll:

1. PCC rate reports are averaged per sender QPN over the complete poll interval.
   That interval sample then enters a persistent per-flow EWMA. Its compile-time
   tuning constants are `PATH_RATE_EWMA_NUMERATOR` and
   `PATH_RATE_EWMA_DENOMINATOR`; the default new-sample weight is `1/8`.
2. RDMA-CM maps each QPN into its destination-IP path group.
3. Full-rate flows (`1 << 20`) are excluded from each reduced-rate sum.
4. When both paths contain reduced flows, path 0 receives
   `sum(path0) / (sum(path0) + sum(path1))` of the 64 buckets.
5. If exactly one path group is entirely full-rate, it receives 61 buckets and
   the constrained group receives the minimum 3 buckets.
6. Only rules crossing the old/new boundary are updated.

Flows without a known RDMA-CM mapping are reported as `pending-map` and excluded
from the calculation. Returning CNP counters are grouped using the same learned
sender-QPN path association.

## Ingress ECN marking

Path selection happens before two separate destination-IP marker chains. A
path's configured percentage describes the intended CE percentage over all
traffic on that path. Only the selected half of the random path/class space is
eligible, so the hardware sampler uses twice the requested percentage, capped
at 100%. Both marker hits and misses clear only the private DSCP path bit before
delivery; unrelated DSCP and existing ECN bits are preserved.

The two `INGRESS_PATH_DEMUX` entries count bytes before destination-IP matching.
Once per poll, the application reports the byte deltas as path0, path1, and total
throughput in Gbps. These counters therefore measure the actual DSCP-selected
virtual-path traffic rather than only packets eligible for a path's ECN marker.

## Build

The root Meson project builds the standalone binary and links the same steering
library into `doca_pcc`:

```bash
meson setup build
ninja -C build
```

To rebuild only the standalone executable:

```bash
ninja -C build doca_flow_steer
```

The source is compile-tested and hardware-validated with the official DOCA 2.9.5001 and 3.1.0105
development images and with native DOCA 3.4.0112. DOCA 2.7 is not supported.
`doca_flow_compat.h` contains the entry, update, query, RSS-forward, RoCE-match,
and shared-resource ABI differences.

The optional PCC DPA resources-file parser uses host APIs introduced in DOCA
3.4. On DOCA 3.1, select execution units with the existing `--threads` option
instead of the resources-file/application-key options.

The PCC device build script also detects whether the SDK provides
`dpa-app-attributes2blob`. DOCA 3.1 invokes `dpacc` without process attributes;
newer SDKs generate the YAML attribute blob and pass `--dpa-proc-attr`. The
3.1 path also selects the first `dpacc_mcpu` target (`nv-dpa-bf3` by default),
because its compiler does not accept the newer comma-separated multi-target
form.

For the egress classifier, DOCA 3.1 and 3.4 use a native 64-entry RANDOM HASH pipe.
Each immutable HASH entry writes its index to application scratch `meta.u32[4]`
and forwards to one 64-entry BASIC dispatch pipe. Path-share changes update the
dispatch entry's changeable forward; `u32[4]` avoids the scratch region used
internally by HASH pipes. The DOCA 2.9 compatibility branch instead hashes
`parser_meta.random` into the same 64 metadata buckets and uses the same BASIC dispatch table.

Ingress installs explicit ARP steering before the IPv4/RoCE chains. ARP from
wire is flooded to both receiver SFs, while ARP from either SF is forwarded to
wire, so neighbor discovery does not depend on default-miss/FDB behavior.

### DOCA 2.9 compatibility

DOCA 2.9 uses the same logical pipeline and PCC path-share calculation, with these
version-specific backends:

- The CLI remains identical to 3.x: `-r` and `-R` accept
  `pci/<BDF>,pf<N>sf<N>`. The 2.9 callback parses the PF BDF and SF numbers,
  then probes them with the mlx5 `representor=sf...` devarg. Embedded PCC also
  verifies that the `-r` PF matches its already-open `--device` handle.
- The egress classifier uses `parser_meta.random` as the key of a 64-entry HASH pipe. Each entry
  writes its bucket to metadata and forwards to the BASIC dispatch table. Live updates supply the
  full rewrite action as required by the 2.9 entry-update API. The 3.x native RANDOM HASH plus
  metadata-dispatch implementation remains unchanged.
- QP1 observation uses shared mirror resources 1 (wire ingress) and 2 (SF egress), plus one DPDK RX queue. The
  public 2.9 Flow API cannot match BTH destination QPN, so the first-pass
  backend mirrors all IPv4 UDP/4791 packets and rejects non-QP1 packets in the
  software parser. This is functionally correct but may be expensive at line
  rate; hardware validation should measure RX clone load before considering a
  direct `rte_flow` IB-BTH rule.
  The shared mirror targets a fixed-size, bidirectional BASIC pipe, which then
  forwards to DPDK RSS queue 0; switch-mode mirrors cannot target RSS directly
  or target a resizable/non-bidirectional pipe. Packet registers are not
  expected to survive the mirror copy.
- Receiver ARP fan-out uses shared mirror resource 3 to deliver wire ARP to
  both SFs.
- Exact hardware CNP counters are disabled because the same public BTH matcher
  is unavailable. PCC-side CNP statistics remain available.
- DOCA 2.9 lacks the 3.x PCC binary trace callback used for per-QPN rate reports.
  The DPA stores the newest rate per QPN and the host retrieves a mailbox
  snapshot once per steering poll (one second), then feeds it to the unchanged
  grouping calculation.

The 2.9 backend is hardware-validated, including shared-mirror behavior, dual-SF logical port
ordering, classifier entry updates, QP1 clone handling, and teardown on 2.9.5.

## Run

Receiver/ingress, with two representors and destination IPs:

```bash
sudo ./build/doca_flow_steer \
  -r pci/0000:03:00.0,pf0sf0 \
  -R pci/0000:03:00.0,pf0sf4 \
  --path0-ip 10.0.0.1 --path1-ip 10.0.0.11 \
  --role ingress --path0-percent 0.025 --path1-percent 0.05
```

Sender/egress standalone diagnostic:

```bash
sudo ./build/doca_flow_steer \
  -r pci/0000:03:00.1,pf1sf0 \
  --path0-ip 10.0.0.1 --path1-ip 10.0.0.11 \
  --role egress
```

The same commands and representor syntax are used on DOCA 2.9.5, 3.1, and 3.4. On 2.9 only the
internal conversion to mlx5 representor devargs is
different.

The normal sender deployment embeds the egress role in `doca_pcc`. On 3.x,
the PCC trace handler calls `steer_update_pcc_rate()` directly. On 2.9, the
once-per-second host poll fetches the DPA mailbox snapshot before calling
`steer_poll()`.

| Option | Meaning |
|---|---|
| `-r`, `--path0-rep` | sender SF for egress, or receiver path-0 SF for ingress |
| `-R`, `--path1-rep` | receiver path-1 SF; required for ingress |
| `--path0-ip`, `--path1-ip` | destination IPs used for receiver delivery and PCC grouping |
| `--role ingress\|egress\|both` | pipeline half to build |
| `--path0-percent`, `--path1-percent` | intended per-path all-traffic CE percentages |
| `--device` | PCC RDMA device name; independent of the steering `-r` syntax |

## Shutdown

On exit, rate callbacks are quiesced first. The proxy port owns every pipe, so
one proxy flush removes the complete pipeline. The representor child ports are
then stopped before their proxy parent. Empty representor ports are not flushed
because that triggers a DOCA 3.4 dual-representor teardown failure.
