# `doca_flow_mirror` — mirror wire packets to RSS and capture them to a pcap

A DOCA Flow program (PF0, switch mode) that **forwards p0-wire ingress to the receiver SF
(`mlx5_2`) exactly as normal, and in parallel mirrors a *copy* of each packet to a CPU RSS queue**,
where a small poll loop writes every captured packet to a `.pcap` file. It reuses the same
PORT_DEMUX / eSwitch-forwarding scaffold as [`doca_flow_ecn`](doca_flow_ecn.c) (see the repo
[README](../README.md#configuration-setup) for the p0↔p1 DAC loopback and the ns0/ns1 SFs), so the
data path is untouched — capture is purely additive.

It's the "observe the traffic on the host" building block: point it at the loopback and you get a
Wireshark-readable trace of the live RoCEv2 flow that Part II's ECN marking / Part IV's PCC loop act
on.

---

## What it does

```
                     ┌──────────────── PF0 eSwitch (FDB, switch mode) ────────────────┐
 p1 wire ─DAC─► p0 ─►│ PORT_DEMUX ─(wire)─► [RANDOM_SAMPLE] ─► CAPTURE ─► mlx5_2 (SF, port 1) │─► receiver
                     │                                          │                              │
                     │                                          └─(mirror copy)─► RSS pipe ─► RX queue 0 ─► pcap
                     │ mlx5_2 SF egress ─────────────────────────────────────────► p0 wire (return path)   │
                     └──────────────────────────────────────────────────────────────────────────────────┘
```

- **CAPTURE pipe** matches all IPv4, forwards to `mlx5_2` (port 1) via its normal `fwd`, and — through
  its monitor's `shared_mirror_id` — mirrors a **copy** to an **RSS pipe** that delivers to CPU RX
  queue 0. Non-IPv4 misses to a plain PASSTHROUGH pipe (forward only).
- **The original packet is never diverted or modified** — only copied. Goodput is unaffected
  (measured: 92.47 Gb/s with capture-all vs 92.48 Gb/s baseline — see below).
- A CPU loop (`rte_eth_rx_burst` on PF0 queue 0) writes each mirrored packet to the pcap with
  `libpcap` (`DLT_EN10MB`).

## Build

Part of the `doca-flow` meson build (links `libpcap`):

```bash
meson setup build && ninja -C build      # from the repo root; produces build/doca-flow/doca_flow_mirror
```

Or natively with gcc (no meson needed — this is a plain host app):

```bash
export PKG_CONFIG_PATH=/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig
PKGS="doca-flow doca-argp doca-common doca-dpdk-bridge libdpdk"
gcc -O2 -Wno-missing-braces -Wno-missing-field-initializers -D DOCA_ALLOW_EXPERIMENTAL_API \
  doca_flow_mirror.c -o doca_flow_mirror \
  $(pkg-config --cflags $PKGS) -Wl,--no-as-needed $(pkg-config --libs $PKGS) -lpcap
```

Needs `libpcap-dev` (the [`Dockerfile`](../Dockerfile) installs it for the container build).

## Run

1. Bring up the loopback once per boot (SFs → ns0/ns1, OVS bridges, hugepages):
   ```bash
   ./setup_roce_loopback.sh
   ```
2. Start the mirror/capture program on PF0 (leave running):
   ```bash
   sudo ./build/doca-flow/doca_flow_mirror --pcap /tmp/capture.pcap
   #   add --sample N to capture only ~1-in-N packets (see below)
   ```
3. Drive RoCE traffic across the loopback (from the repo scripts or by hand):
   ```bash
   sudo ip netns exec ns0 ib_write_bw -d mlx5_2 -x 1 -F -R --report_gbits            # receiver (server)
   sudo ip netns exec ns1 ib_write_bw -d mlx5_3 -x 1 -F -R 10.0.0.1 --report_gbits   # sender  (client)
   ```
4. Stop with Ctrl-C (flushes and closes the pcap) and read it:
   ```bash
   tcpdump -nr /tmp/capture.pcap | head        # RoCEv2 = UDP dst 4791
   # or open /tmp/capture.pcap in Wireshark (decodes RoCEv2/IB automatically)
   ```

The program logs a per-second line: `captured(pcap): <n> | capture-pipe pkts: <m>`.

## `--sample N` — capture only some packets

At ~line rate the RSS-to-CPU path can't keep up and most mirrored copies are dropped by the NIC (you
still get a large pcap, just not every packet). When you only want a representative sample, use
`--sample N` to mirror **~1 of every N packets** — everything still forwards to `mlx5_2`, only the
copy is gated:

```bash
sudo ./build/doca-flow/doca_flow_mirror --pcap /tmp/sample.pcap --sample 10000
```

Sampling uses the NIC's hardware `parser_meta.random` field, which only supports **power-of-two**
rates, so `N` is rounded to the nearest one and the actual rate is logged. `--sample 10000` →
**1/8192**. Default is `--sample 1` (capture everything).

## Measured on 2×BlueField-3 (DOCA 2.9.1, p0↔p1 loopback)

| run | client goodput | packets written to pcap (5 s) |
|---|---|---|
| baseline (no mirror program) | **92.48 Gb/s** | — |
| capture-all (`--sample 1`) | **92.47 Gb/s** | ~2.8 M |
| `--sample 10000` → 1/8192 | ~line rate | ~6.8 k |

**Mirroring does not cost goodput** — the copy is made in the eSwitch hardware; the original forwards
unimpeded. Captured packets are genuine RoCEv2 (`10.0.0.2.<sport> > 10.0.0.1.4791 UDP`).

## Notes / design

- **A mirror target cannot be an inline-RSS fwd** on this firmware (bind fails with *"invalid mirror
  list format"*). The mirror target must be a PORT or a PIPE, so the program creates a dedicated
  **RSS pipe** and points the shared mirror at it (`FWD_PIPE → rss_pipe`). This matches NVIDIA's own
  `flow_shared_mirror` / `flow_switch_to_wire` samples.
- The shared-mirror resource is 0-indexed with id 0 reserved as "no mirror", so the app reserves
  `MIRROR_ID + 1` resources to use id 1.
- The pcap link is truncated to the first mbuf segment (`data_len`); the RoCE frames here are
  single-segment, so nothing is lost. `DLT_EN10MB` (Ethernet) is used.
- Point PCC at the uplink **PF** and RoCE at the **SF** — the mirror program runs on PF0 (`mlx5_0`);
  traffic uses the SFs (`mlx5_2`/`mlx5_3`).

Key source: [`doca_flow_mirror.c`](doca_flow_mirror.c) — `create_rss_pipe()`, `setup_capture_mirror()`,
`create_capture_pipe()` (the `shared_mirror_id` attach), and the `rte_eth_rx_burst` → `pcap_dump` loop
in `main()`.
