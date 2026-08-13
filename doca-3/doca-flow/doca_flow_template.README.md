# `doca_flow_template` — build the ECN-marking pipeline yourself (DOCA 3.x)

This is [`doca_flow_ecn_pcap.c`](doca_flow_ecn_pcap.c) with the DOCA Flow pipeline removed. Your job
is to put it back: five functions, marked `TODO 1`–`TODO 5` in
[`doca_flow_template.c`](doca_flow_template.c).

Everything else is already written — EAL and device setup, the mbuf pool, DOCA Flow init, port
start, the argument parser, the SIGUSR1/SPACE capture toggle, the RX-burst loop that writes the
pcap, and the counter report. You should not need to touch any of it.

## What you are building

```
                     ┌──────────────── PF0 eSwitch (FDB, switch mode) ────────────────┐
 p1 wire ─DAC─► p0 ─►│ PORT_DEMUX ─(wire)─► [RANDOM_SAMPLE(--percent)]                │
                     │        ┌───────────────┴───────────────┐                       │
                     │   MARK_CAPTURE                     PASS_CAPTURE                │
                     │  (set CE, then FLOOD)             (no mark, then FLOOD)        │─► receiver
                     │        └──────────────┬────────────────┘                       │
                     │            FLOOD ─┬─► entry 0 -> mlx5_2 (ordered)              │
                     │                   └─► entry 1 -> TO_CPU -> RX queue 0 -> pcap  │
                     │ mlx5_2 SF egress ──────────────────────────► p0 wire (return)  │
                     └───────────────────────────────────────────────────────────────┘
```

`build_pipeline()` is given to you, and it is worth reading first: it shows which pipes exist under
which options and how they chain together. What it calls is what you write.

## Start here: the worked example

`create_passthrough_pipe()` is fully implemented. Read it before anything else — every pipe in this
file has the same five-part shape, and that is the smallest instance of it:

1. **describe the match** — `m` holds the fields that take part, `mm` the mask over them
2. **describe the forward** — `fwd` is where a hit goes; some pipes also pass a miss forward
3. **build the pipe** — `cfg_create`, `set_name` / `set_type` / `set_domain` / `set_is_root` /
   `set_nr_entries` / `set_match`, then `doca_flow_pipe_create`, then destroy the cfg
4. **add the entries** — `doca_flow_pipe_add_entry`, once per entry
5. **install and check** — `doca_flow_entries_process`, then confirm `nb_processed`

### The match idiom, which is easy to get backwards

Setting `m.outer.ip4.dscp_ecn = 0xFF` at *pipe creation* declares "this pipe matches on the
DSCP/ECN byte". The `0x00` in `mm` says "…but ignore its value". The **entry** added later supplies
the value actually compared. Pipe-level match = which fields; entry-level match = what values.

The same split applies to actions: the pipe declares an action *template* (`0xFF` = "entries may
write this field"), and the entry supplies the value to write.

## The five functions

Work in this order. Each one gets the program a step further.

### TODO 1 — `create_to_cpu_pipe`
Where captured copies land. A `BASIC` pipe in the `DEFAULT` domain, not a root pipe, one entry.
Match IPv4 only — the parser records what it saw in `match.parser_meta.outer_l3_type`, and the value
to compare against is `DOCA_FLOW_L3_META_IPV4`. Forward with `DOCA_FLOW_FWD_RSS` to a single queue
(`rssq`, already holding queue 0), and set `rss_outer_flags` to hash over IPv4 + UDP — RoCEv2 rides
on UDP.

### TODO 2 — `create_flood_pipe`
How a packet reaches both the receiver and the pcap. DOCA Flow 3.2 removed the shared mirror the
2.x build used, so 3.x does this with a `DOCA_FLOW_PIPE_HASH` pipe running
`DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING`, which delivers every packet to *all* of its entries
instead of hashing to one.

Two entries, and the order matters: entry `FLOOD_ENTRY_PRODUCTION` (0) forwards to
`production_pipe` and on to the SF, entry `FLOOD_ENTRY_CAPTURE` (1) forwards to `capture_pipe`, the
TO_CPU pipe. Ordering is only guaranteed for entry 0, which is why the real data path is pinned
there and the pcap copy — where reordering costs nothing — takes entry 1. A hash pipe's entry count
must be a power of two, hence exactly two.

A hash pipe selects by index rather than by match, so pass `NULL` as the match mask. Clear `ef`
between the two `add_entry` calls.

### TODO 3 — `create_forward_to_sf_pipe`
The marking itself, and the biggest of the five. It is called twice — once as `PASS_CAPTURE`
(`mark=false`) and once as `MARK_CAPTURE` (`mark=true`) — so everything mark-specific is behind
`if (mark)`.

- Match any IPv4 packet whatever its current ECN bits (the idiom above).
- Give the pipe a **non-shared counter** via `mon.counter_type`, or the `CE marked:` line stays at
  zero and you cannot tell whether anything works.
- When `flood_pipe` is non-NULL, forward through it with `DOCA_FLOW_FWD_HASH_PIPE`, naming both
  the pipe and the flooding algorithm. When it is NULL there is no capture, so forward straight to
  port 1 instead.
- When `mark` is true, declare the DSCP/ECN rewrite as a pipe-level action template, and set the
  per-entry action value to the ECN codepoint **CE**. RFC 3168: `Not-ECT 00`, `ECT(1) 01`,
  `ECT(0) 10`, `CE 11`.
- Miss goes to `miss_pipe`; hits forward to port 1 (the SF representor).
- Write the entry handle back through `out_entry` — that is what the counter report queries.

### TODO 4 — `create_sampling_pipe`
Only used when `--percent` is strictly between 0 and 100. The parser hands every packet a random
16-bit value in `parser_meta.random`. `mask` has already been computed as a power-of-two-minus-one
(`0x0001` = half, `0x0003` = a quarter, …). Match that field against the value `0` under `mask`, and
exactly 1-in-`(mask+1)` packets hit. Hits go to `hit`, misses to `miss`.

### TODO 5 — `create_root_pipe`
The **root** pipe: every packet entering the eSwitch hits it first, and it sorts traffic by the port
it arrived on (`parser_meta.port_meta`, declared and fully masked with `UINT32_MAX`). Two entries,
each supplying its own forward — that is what the pipe-level `FWD_CHANGEABLE` defers to them:

| `port_meta` | source | forward to |
| --- | --- | --- |
| `0` | the wire, via PF uplink p0 | `wire_target` — the head of the marking chain |
| `1` | the receiver SF coming back | port `0`, i.e. straight out to the wire |

The second entry is the RoCE return path carrying ACKs and CNPs. Marking it would corrupt the very
feedback the congestion controller in Part IV reacts to.

Clear `ef` between the two `add_entry` calls, or fields leak from the first into the second. The
`DOCA_FLOW_WAIT_FOR_BATCH` flag on the first and `0` on the second are already correct: together
they queue both entries and push them to hardware in one batch.

## Building and running

```bash
cd doca-3 && meson setup build && ninja -C build
sudo ./build/doca-flow/doca_flow_template -- --pcap /tmp/out.pcap --percent 100
```

The template **compiles and runs before you write anything**, but it does not forward: with no root
pipe installed, DOCA Flow still takes ownership of PF0's eSwitch and every packet is dropped. While
it is running, `run_server.sh` will not connect and even `ping` between the two namespaces gets
100% loss; stop the program and the link comes straight back.

That is your starting point, not a bug — and it is the first thing the exercise teaches. Your
program owns the data path from the moment it starts, so nothing moves until *you* build the
pipeline that moves it. `create_root_pipe` (TODO 5) is the root pipe, and until it exists
none of the other pipes are reachable no matter how correct they are.

To generate traffic, run `admin/local_scripts/setup_roce_loopback.sh` once, then `run_server.sh` and
`run_client.sh` from the repository root in two other shells.

### Knowing it works

- `CE marked:` climbs, and `passthrough:` stays near zero at `--percent 100`.
- `flooded:` climbs, showing copies reaching the CPU queue.
- Press **SPACE** (or `kill -USR1 <pid>`) to start writing the pcap, then check the ECN bits with
  `admin/local_scripts/pcap_ecn_stats.py /tmp/out.pcap` — you want `ce` to equal `ipv4`.
- `run_server.sh` should still report near line rate. If throughput collapses, the data path is
  going somewhere it should not.

### When a pipe fails

DOCA prints a wall of internal errors and then exits. The **last** line is the one that matters:

```
[CRT][doca_flow_template.c:NNN][doca_check] rss create: Operation not supported
```

The text before the colon names the pipe, so `rss create` sends you to `create_to_cpu_pipe`.

## Checking your answer

The finished version is right next to it:

```bash
diff doca_flow_template.c doca_flow_ecn_pcap.c
```

Only the five function bodies should differ — nothing else, anywhere.

The two files are deliberately identical in every other respect, including the program name they
register with, so `--help` and the log lines read `doca_flow_ecn_pcap` in both. That is not a
mistake: it keeps the diff down to your work and nothing else.
