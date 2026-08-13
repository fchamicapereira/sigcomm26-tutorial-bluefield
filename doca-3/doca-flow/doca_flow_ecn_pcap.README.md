# `doca_flow_ecn_pcap` — ECN-mark **and** capture-to-pcap in one PF0 program (DOCA 3.x)

A DOCA Flow program (PF0, switch mode) that CE-marks (`ECN=11`) a configurable fraction of the wire
traffic *and* copies each packet to a CPU RSS queue where it's written to a `.pcap` — while the
original still forwards to the receiver SF untouched.

Why combine them? Only **one** DOCA Flow / DPDK primary process can own PF0 (and the
`/var/run/dpdk/rte/config` lock) at a time, so marking and capturing cannot run as two programs
side by side — the second fails with *"Cannot create lock … another primary process running"*. This
program folds both roles into one eSwitch pipeline so you can mark **and** watch the same traffic.

This is the DOCA 3.x build of [`doca-2/doca-flow/doca_flow_ecn_pcap.c`](../../doca-2/doca-flow/doca_flow_ecn_pcap.c).
It is the same program with the same structure; see [What differs from the 2.x build](#what-differs-from-the-2x-build).
One source serves every DOCA 3.x release — verified on 3.1 and 3.4 — with the handful of API
differences between the minors absorbed by [`doca_flow_compat.h`](doca_flow_compat.h).

---

## What it does

```
                     ┌──────────────── PF0 eSwitch (FDB, switch mode) ───────────────┐
 p1 wire ─DAC─► p0 ─►│ PORT_DEMUX ─(wire)─► [RANDOM_SAMPLE(--percent)]                │
                     │        ┌───────────────┴───────────────┐                       │
                     │   MARK_CAPTURE                     PASS_CAPTURE                │
                     │  (set CE, then FLOOD)             (no mark, then FLOOD)        │─► receiver
                     │        └──────────────┬────────────────┘                       │
                     │            FLOOD ─┬─► entry 0 -> PASSTHROUGH -> mlx5_2         │
                     │                   └─► entry 1 -> TO_CPU -> RX queue 0 -> pcap  │
                     │ mlx5_2 SF egress ──────────────────────────► p0 wire (return)  │
                     └───────────────────────────────────────────────────────────────┘
```

- **`--percent`** of wire IPv4 packets are routed to **MARK_CAPTURE** (sets `dscp_ecn = CE`); the rest
  to **PASS_CAPTURE** (no mark). Selection uses the NIC's hardware `parser_meta.random`, so the
  percentage is rounded to the nearest power-of-two fraction.
- **Both** pipes fan the packet out through **FLOOD** — so every wire IPv4 packet reaches the
  receiver *and* the pcap, `~--percent` of them CE-marked (the marked copies show `CE` in the pcap).
- The copies land on **CPU RX queue 0**; a poll loop writes them to the pcap with `libpcap`
  (`DLT_EN10MB`). The original packet is never diverted or modified — goodput is unaffected.
- Non-IPv4 packets miss to a plain forward pipe (`PASSTHROUGH`, forward only), which also serves as
  FLOOD's ordered production target.

## Build

Part of the `doca-flow` meson build (links `libpcap`):

```bash
cd doca-3 && meson setup build && ninja -C build   # produces build/doca-flow/doca_flow_ecn_pcap
```

Needs `libpcap-dev` (the [`Dockerfile`](../Dockerfile) installs it for the container build).

## Run

1. Bring up the loopback once per boot (SFs → ns0/ns1, OVS bridges, hugepages):
   ```bash
   ./admin/local_scripts/setup_roce_loopback.sh
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
`CE marked: <n>, passthrough: <m> (<pct>% marked) | flooded: <x> -> pcap: <y> [PAUSED]`.

## Flags

| flag | meaning |
|---|---|
| `--pcap <file>` | **Optional.** Enable capture to `<file>`. Omit for pure ECN-mark mode (no flooding pipe, full goodput). |
| `--percent N` | Fraction of packets to CE-mark, `[0,100]` (rounded to a power-of-two fraction; default `100` = mark all). All packets are captured regardless. |
| `--sample N` | Write only ~1-in-N captured packets to the pcap (default `1` = every packet). Marking/forwarding are unaffected — only the pcap shrinks. |

**`-- ` before the app flags is required** (DOCA argp separates the DPDK EAL args from the app args).

## What differs from the 2.x build

The two files are deliberately kept as close as possible; everything below is forced by the DOCA
version, not by preference.

| | `doca-2` | `doca-3` |
|---|---|---|
| copy to the pcap | shared mirror (`DOCA_FLOW_SHARED_RESOURCE_MIRROR`) attached via `monitor.shared_mirror_id` | `DOCA_FLOW_PIPE_HASH` pipe running `DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING` |
| SF representor | DPDK port index passed as a devargs string | `doca_dev_rep` opened via `doca_devinfo_rep_create_list` |
| port config | `doca_flow_port_cfg_set_devargs` | `..._set_port_id` + `..._set_actions_mem_size` |
| entry install | `doca_flow_pipe_add_entry`, `action_idx` inside `doca_flow_actions` | `doca_flow_pipe_basic_add_entry`, explicit `action_idx` argument and `DOCA_FLOW_ENTRY_FLAGS_*` |
| RSS forward | flat `fwd.rss_queues` / `num_of_queues` / `rss_outer_flags` | nested `fwd.rss.queues_array` / `nr_queues` / `outer_flags` |
| source port match | `parser_meta.port_meta` (`uint32`) | `parser_meta.port_id` (`uint16`) |
| mode args | `switch,hws,isolated,disable_switch_rss` | `switch,hws` |
| probe args | `dv_flow_en=2,fdb_def_rule_en=1,repr_matching_en=0,representor=sf0` | `dv_flow_en=2,fdb_def_rule_en=1,representor=sf0` |
| EAL allowlist | one dummy `-a pci:00:00.0` | also `-a auxiliary:`, or EAL probes the SFs now living in ns0/ns1 |

**The mirror is gone from 3.2 onwards.** DOCA Flow 3.2 removed `DOCA_FLOW_SHARED_RESOURCE_MIRROR`,
`struct doca_flow_resource_mirror_cfg` and `doca_flow_mirror_target`; by 3.4 no DOCA header mentions
"mirror" and `libdoca_flow.so` exports no mirror symbol. The documented replacement is a flooding
hash pipe, which delivers the packet to **every** entry in the pipe.

DOCA 3.1 is the exception: it still has the shared mirror *and* the flooding pipe. This tree uses
flooding there anyway, so one set of sources serves every 3.x release — and the flooding path is the
one that has to keep working on 3.2+, so it is the one worth exercising. If you are reading this on
a 3.1 box and wondering why the mirror is not used: it would work, and it is a dead end.

Two constraints shape the flooding pipe:

- packet order is only guaranteed for the destination of the **first** entry, so entry 0 carries the
  production path (to the SF) and the pcap copy is entry 1;
- a hash pipe's entry count must be a power of two (2 here), and flooding tops out at 254 entries.

Both entries forward with the same `fwd` **type** (`FWD_PIPE`), which is why entry 0 goes through the
`PASSTHROUGH` pipe rather than straight to `FWD_PORT` — the pipe is created from a single fwd
template and mixing types across its entries is not expressible. That is also why `build_pipeline`
creates `PASSTHROUGH` first here, where the 2.x build creates it after the capture path.

## Notes / design

- **Uses plain `"switch,hws"` mode.** The 2.x build additionally passes
  `"isolated,disable_switch_rss"` (and calls `rte_flow_isolate`) to stop DOCA Flow building an
  internal FDB RSS suffix context that BlueField firmware rejects at port start; on 3.x that is not
  needed, and the capture path needs RSS in the FDB domain.
- **`--sample` is done in software** (write every Nth captured packet), keeping the mark ratio exact
  and independent of the capture ratio — `parser_meta.random` is already used by `--percent`.
- **The SPACE toggle needs a TTY.** Under a non-interactive launch (e.g. `</dev/null`, `nohup`) there
  is no terminal, so writing stays paused and the pcap stays empty by design — run in the foreground.
- Point PCC at the uplink **PF** and RoCE at the **SF** — this program runs on PF0 (`mlx5_0`); traffic
  uses the SFs (`mlx5_2`/`mlx5_3`).

Key source: [`doca_flow_ecn_pcap.c`](doca_flow_ecn_pcap.c) — `create_forward_to_sf_pipe()` (mark +
fan-out), `create_sampling_pipe()` (`--percent`), `create_to_cpu_pipe()` / `create_flood_pipe()` (the
flood→pcap path), and the `rte_eth_rx_burst` → `pcap_dump` loop in `run_capture_loop()` with the
SPACE/`--sample` gates.

The participant exercise built from this file is [`doca_flow_template.README.md`](doca_flow_template.README.md).
