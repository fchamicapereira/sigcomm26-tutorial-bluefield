# `doca_flow_ecn_pcap` — ECN-mark **and** capture-to-pcap in one PF0 program

A DOCA Flow program (PF0, switch mode) that does what [`doca_flow_ecn`](doca_flow_ecn.c) and
[`doca_flow_mirror`](doca_flow_mirror.c) do, **at the same time, in a single program**: it CE-marks
(`ECN=11`) a configurable fraction of the wire traffic *and* mirrors a copy of each packet to a CPU
RSS queue where it's written to a `.pcap` — while the original still forwards to the receiver SF
untouched.

Why combine them? Only **one** DOCA Flow / DPDK primary process can own PF0 (and the
`/var/run/dpdk/rte/config` lock) at a time, so you can't run `doca_flow_ecn` and `doca_flow_mirror`
side by side — the second fails with *"Cannot create lock … another primary process running"*. This
program folds both roles into one eSwitch pipeline so you can mark **and** watch the same traffic.

---

## What it does

```
                     ┌──────────────── PF0 eSwitch (FDB, switch mode) ────────────────┐
 p1 wire ─DAC─► p0 ─►│ PORT_DEMUX ─(wire)─► [RANDOM_SAMPLE(--percent)]                 │
                     │        ┌───────────────┴───────────────┐                       │
                     │   MARK_CAPTURE                     PASS_CAPTURE                 │
                     │  (set CE + fwd mlx5_2 + mirror)   (fwd mlx5_2 + mirror)         │─► receiver
                     │        └──────────────┬────────────────┘                       │
                     │                  (mirror copy) ─► RSS pipe ─► RX queue 0 ─► pcap│
                     │ mlx5_2 SF egress ───────────────────────────────► p0 wire (return path) │
                     └──────────────────────────────────────────────────────────────────────┘
```

- **`--percent`** of wire IPv4 packets are routed to **MARK_CAPTURE** (sets `dscp_ecn = CE`); the rest
  to **PASS_CAPTURE** (no mark). Selection uses the NIC's hardware `parser_meta.random`, so the
  percentage is rounded to the nearest power-of-two fraction.
- **Both** pipes forward the original to `mlx5_2` (port 1) *and* mirror a copy — so every wire IPv4
  packet is captured, `~--percent` of them CE-marked (the marked copies show `CE` in the pcap).
- The mirrored copies land on **CPU RX queue 0**; a poll loop writes them to the pcap with `libpcap`
  (`DLT_EN10MB`). The original packet is never diverted or modified — goodput is unaffected.
- Non-IPv4 packets miss to a plain forward pipe (`PASSTHROUGH`, forward only).

## Build

Part of the `doca-flow` meson build (links `libpcap`):

```bash
meson setup build && ninja -C build      # from the repo root; produces build/doca-flow/doca_flow_ecn_pcap
```

Or natively with gcc (no meson needed — this is a plain host app):

```bash
export PKG_CONFIG_PATH=/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig
PKGS="doca-flow doca-argp doca-common doca-dpdk-bridge libdpdk"
gcc -O2 -Wno-missing-braces -Wno-missing-field-initializers -D DOCA_ALLOW_EXPERIMENTAL_API \
  doca_flow_ecn_pcap.c -o doca_flow_ecn_pcap \
  $(pkg-config --cflags $PKGS) -Wl,--no-as-needed $(pkg-config --libs $PKGS) -lpcap
```

Needs `libpcap-dev` (the [`Dockerfile`](../Dockerfile) installs it for the container build).

## Run

1. Bring up the loopback once per boot (SFs → ns0/ns1, OVS bridges, hugepages):
   ```bash
   ./setup_roce_loopback.sh
   ```
2. Start the program on PF0. Run it in the **foreground** — the SPACE toggle needs a real terminal:
   ```bash
   sudo ./build/doca-flow/doca_flow_ecn_pcap -- --pcap /tmp/cap.pcap --percent 50 --sample 8
   #   omit --pcap entirely for pure ECN-mark mode (no capture, full goodput)
   ```
   pcap writing **starts PAUSED** — press **SPACE** (or `c`/`p`) to start/stop writing at runtime.
   Marking and forwarding always run regardless of the toggle.
3. Drive RoCE traffic across the loopback:
   ```bash
   sudo ip netns exec ns0 ib_write_bw -d mlx5_2 -x 1 -F -R --report_gbits            # receiver (server)
   sudo ip netns exec ns1 ib_write_bw -d mlx5_3 -x 1 -F -R 10.0.0.1 --report_gbits   # sender  (client)
   ```
4. Stop with Ctrl-C (flushes and closes the pcap) and read it:
   ```bash
   tcpdump -vnn -r /tmp/cap.pcap | head          # CE-marked show "tos 0x3,CE"; unmarked "tos 0x2"
   # tos histogram over the capture:
   tcpdump -vnn -c 8000 -r /tmp/cap.pcap 2>/dev/null | grep -oE 'tos 0x[0-9a-f]+(,CE)?' | sort | uniq -c
   ```

The program logs a per-second line:
`CE marked: <n>, passthrough: <m> (<pct>% marked) | mirrored: <x> -> pcap: <y> [PAUSED]`.

## Flags

| flag | meaning |
|---|---|
| `--pcap <file>` | **Optional.** Enable capture to `<file>`. Omit for pure ECN-mark mode (no mirror, full goodput). |
| `--percent N` | Fraction of packets to CE-mark, `[0,100]` (rounded to a power-of-two fraction; default `100` = mark all). All packets are captured regardless. |
| `--sample N` | Write only ~1-in-N captured packets to the pcap (default `1` = every packet). Marking/forwarding are unaffected — only the pcap shrinks. |

**`-- ` before the app flags is required** (DOCA argp separates the DPDK EAL args from the app args).

## Measured on 2×BlueField-3 (DOCA 2.9.1, p0↔p1 loopback, 64 KB `ib_write_bw`)

| run | marked (HW counter) | captured → pcap | client goodput |
|---|---|---|---|
| `--percent 100` | 100.0 % | 2.7 M (all) | **92.46 Gb/s** |
| `--percent 50 --sample 8` | **50.01 %** | 16.7 M mirrored → **2.09 M** (exactly 1-in-8) | **92.46 Gb/s** |

The sampled `--percent 50` pcap tos histogram ≈ **50 % `tos 0x2`** (ECT, unmarked) / **50 % `tos 0x3,CE`**
(marked), confirming the mirror copies the packet *after* the CE action. **Mirroring does not cost
goodput** — the copy is made in eSwitch hardware; the original forwards unimpeded.

## Notes / design

- **Uses plain `"switch,hws"` mode** (RSS/mirror capture needs it) — not the
  `"switch,hws,isolated,disable_switch_rss"` + `rte_flow_isolate` that standalone `doca_flow_ecn` uses
  on some testbeds to suppress RSS. Isolation would break the capture path.
- **A mirror target cannot be an inline-RSS fwd** on this firmware (bind fails *"Invalid mirror list
  format"*). The mirror points at a dedicated **RSS pipe** (`FWD_PIPE → rss_pipe`), which delivers to
  CPU queue 0. The shared-mirror resource is 0-indexed with id 0 reserved as "no mirror", so the app
  reserves `MIRROR_ID + 1` resources.
- **`--sample` is done in software** (write every Nth mirrored packet), keeping the mark ratio exact
  and independent of the capture ratio. (`doca_flow_mirror` samples in hardware via
  `parser_meta.random`; here that field is already used by `--percent`.)
- **The SPACE toggle needs a TTY.** Under a non-interactive launch (e.g. `</dev/null`, `nohup`) there
  is no terminal, so writing stays paused and the pcap stays empty by design — run in the foreground.
- Point PCC at the uplink **PF** and RoCE at the **SF** — this program runs on PF0 (`mlx5_0`); traffic
  uses the SFs (`mlx5_2`/`mlx5_3`).

Key source: [`doca_flow_ecn_pcap.c`](doca_flow_ecn_pcap.c) — `create_capture_pipe()` (mark + mirror),
`create_random_sample_pipe()` (`--percent`), `create_rss_pipe()` / `setup_capture_mirror()` (the
mirror→pcap path), and the `rte_eth_rx_burst` → `pcap_dump` loop with the SPACE/`--sample` gates in
`main()`.
