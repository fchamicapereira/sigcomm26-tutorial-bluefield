<!--
  DRAFT — Part I of the hands-on tutorial: DOCA Flow. (PCC is Part II, written next.)
  Audience: a networking person who has NOT written DOCA/BlueField/C code before. Hand-hold hard:
  explain every command and what it prints; never tell them to "go read" a C file — walk them
  through it. Part C is split into stages: Stage 1 = ECN marking only (compile & see the counter),
  Stage 2 = add the mirror and see the CE bit in a pcap. Structure follows a guided assignment:
  Parts A/B/C, collapsible "Try it yourself!" blocks with numbered terminal steps + expected output,
  INFO/NOTE callouts, source line-links, Debugging Tips, FAQs.

  Author notes are in HTML comments; delete before publishing. Search "AUTHOR:" for open items.
  Line-links assume this file sits at the repo root. Expected outputs: the ib_write_bw table and the
  "CE marked:" lines are real; the doca_flow_nop banner and sample list are illustrative —
  AUTHOR: replace with real pastes + terminal screenshots.
-->

# Programmable Packet Processing on the BlueField-3 with DOCA Flow

In this part you will program the **data plane** of an NVIDIA BlueField-3 SmartNIC — you tell the
NIC what to do with packets *in its own hardware, at line rate*, before any CPU sees them. By the
end you will have run a program that forwards traffic and written one that inspects live network
traffic and rewrites its headers.

You do **not** need any prior BlueField, DOCA, or C experience. We explain each command as we go and
build up one small piece at a time:

- **Part A** — the card, its two-port loopback, and how to put real traffic on it.
- **Part B** — meet DOCA Flow, then walk through a small, *complete* program that forwards packets,
  so the pieces are familiar before you write any.
- **Part C** — extend a scaffolded program to mark packets with a congestion signal — the signal the
  controller you build in Part II reacts to. You do it in two stages: first make the NIC *mark*
  packets, then *capture* a copy so you can see the mark.

> **Prerequisites.** You are logged into the **Arm cores** of a BlueField-3. That is a normal Ubuntu
> Linux shell that happens to run *inside the NIC* — treat it like any small Linux box. You have this
> repository checked out in your home directory, and your user can run `sudo` (administrator)
> commands. **Every command below is typed on the Arm cores** unless a step says otherwise.

---

## Getting Started

### Part A — The card, the topology, and generating traffic

You have **one** BlueField-3 card. It has **two 100-gigabit ports, `p0` and `p1`, connected directly
to each other with a cable**. Whatever leaves one port arrives on the other — so a single card
behaves like a tiny two-node network that talks to itself.

```
        ┌─────────────────────────  BlueField-3 (one card)  ─────────────────────────┐
        │                                                                             │
        │      port p0  ●────────────────  DAC cable  ────────────────●  port p1       │
        │        ▲                                                        ▲            │
        │        │            traffic on p1 arrives on p0, and vice-versa │            │
        │        ▼                                                        ▼            │
        │   ┌────────────────  ConnectX packet-processing pipeline (eSwitch)  ──────┐  │
        │   │        match  ─►  count  ─►  modify  ─►  forward     ← DOCA Flow       │  │
        │   └─────────────────────────────────────────────────────────────────────-─┘ │
        │                                                                             │
        │   Arm cores (Ubuntu)  —  you ssh in here, compile, and launch programs       │
        └─────────────────────────────────────────────────────────────────────────────┘
```
<!-- AUTHOR: for the HTML artifact, replace this ASCII block with an SVG/mermaid diagram. -->

Two parts of the card matter to us:

- **The Arm cores** run Ubuntu — this is the shell you are typing in.
- **The eSwitch** (the ConnectX pipeline in the picture) is the *fast path*: dedicated hardware that
  looks at every packet as it flies through, with no CPU in the way. You tell it what to do from the
  Arm using **DOCA Flow**. Once you have programmed it, it keeps running on its own.

#### The network devices you will use

Run this to see the card's network interfaces:

```bash
$ ip -br link show | grep -E '^p0|^p1|^enp3'
p0            UP    ...          # physical port 0
p1            UP    ...          # physical port 1
enp3s0f0s0    UP    ...          # a "sub-function" (SF) on the p0 side
enp3s0f1s0    UP    ...          # a "sub-function" (SF) on the p1 side
```

> **INFO — what is a sub-function?** A **sub-function (SF)** is a lightweight virtual NIC carved out
> of a physical port. It shows up as its own network device. We create one SF on each side and use
> the two of them as the *endpoints* of a network flow — so a single card can play both "sender" and
> "receiver" across the cable.

The traffic we will watch is **RoCE** (RDMA over Converged Ethernet), the high-speed, kernel-bypass
transport used in AI and storage networks. RoCE does not use normal sockets; programs reach it
through **RDMA devices** named `mlx5_0`, `mlx5_1`, `mlx5_2`, `mlx5_3`. List them:

```bash
$ rdma link show
link mlx5_0/1 ... netdev pf0hpf       # RDMA device for physical port p0
link mlx5_1/1 ... netdev p1           # RDMA device for physical port p1
link mlx5_2/1 ... netdev enp3s0f0s0   # RDMA device for the SF on the p0 side  ← we use this one
link mlx5_3/1 ... netdev enp3s0f1s0   # RDMA device for the SF on the p1 side  ← and this one
```

Remember this mapping: **`mlx5_2` and `mlx5_3` are the two SF endpoints** we send RoCE between.
`mlx5_0`/`mlx5_1` are the physical ports themselves.

#### Step 1: wire up the two endpoints

The two ports are wired into a loopback and split into two isolated network sandboxes (Linux
**network namespaces**) called `ns0` and `ns1` — one SF in each, each with its own IP address
(`mlx5_2` → `ns0` → `10.0.0.1`, `mlx5_3` → `ns1` → `10.0.0.2`). **This is already set up for you on
the tutorial card**, so there's nothing to run here.

> **INFO — what is a network namespace?** It is a private, isolated network stack inside one Linux
> machine — its own interfaces, IPs, and routes. Putting each SF in its own namespace (`ns0`, `ns1`)
> is what makes them behave like two separate hosts even though they live on the same card. You run a
> command "inside" a namespace with `ip netns exec <name> <command>`.

If a reboot or a later step ever tears the loopback down, re-create it with one script,
`admin/local_scripts/setup_roce_loopback.sh`.

#### Step 2: put real traffic on the loopback

We generate traffic with **`ib_write_bw`** — a standard RoCE benchmarking tool (from the `perftest`
package) that measures how fast one endpoint can write data to another. It needs a **server**
(receiver) and a **client** (sender).

<details>
<summary><b>Try it yourself! — send RoCE across the cable and measure it</b></summary>

<br>

Open **two terminals**, both on the Arm cores.

**Terminal 1 — the receiver (server).** Start this one first; it waits for a client:
```bash
sudo ip netns exec ns0 ib_write_bw -d mlx5_2 -R -x 1 -F --report_gbits
```
What the flags mean: `ip netns exec ns0` runs it inside the `ns0` sandbox; `-d mlx5_2` uses that
namespace's RDMA device; `-R` sets the connection up via the RDMA connection manager (keep this on —
Part II needs it); `-x 1` picks the RoCEv2 address; `-F` ignores a CPU-frequency warning;
`--report_gbits` prints the result in gigabits/second. It prints its settings and then says it is
**waiting for a client**.

**Terminal 2 — the sender (client).** Point it at the server's IP, `10.0.0.1`:
```bash
sudo ip netns exec ns1 ib_write_bw -d mlx5_3 -R -x 1 -F 10.0.0.1 --report_gbits
```

**You should see** a results table appear on both terminals, with the throughput climbing toward the
card's line rate:
```
 #bytes     #iterations   BW peak[Gb/sec]   BW average[Gb/sec]   MsgRate[Mpps]
 65536      529037          0.00              92.46                0.176344
```
That **~92 Gb/s** is your proof the whole path works end to end: sender → `p1` → cable → `p0` → the
eSwitch → receiver. If you see a table with a real number, Part A is done.

> You just ran the server and client by hand to see the moving parts. From here on you don't have
> to: **`./scripts/benchmark.sh`** starts both ends together in one command and shows the sender's
> goodput as a live, updating chart (Gb/s), so you never retype those flags again. (It wraps
> `./scripts/run_server.sh` and `./scripts/run_client.sh` if you ever want them separately; Ctrl-C
> stops everything.)

</details>

---

### Part B — Meet DOCA Flow, then walk through a working forwarder

#### What DOCA Flow is

**DOCA Flow** is how you program that eSwitch fast path in C. Your program builds a small graph of
**pipes**. Think of each pipe as one rule with four parts:

| part | the question it answers | example |
| --- | --- | --- |
| **match** | *which* packets does this pipe act on? | "all IPv4 packets" |
| **count** | *how many* packets hit it? | a hardware counter you can read |
| **actions** | *what* to change in the packet? | rewrite a header field (or nothing) |
| **forward** | *where* does the packet go next? | another pipe, a port, the CPU, or drop |

You describe these rules once; the NIC then applies them to every packet in hardware. That's the
whole idea: **match → count → modify → forward.**

**Pipes form a graph.** A pipe's *forward* can hand a packet to a **port** (out the wire), to the
**CPU**, to **drop**, or **to another pipe** — and that last option is how pipes chain. You build
several small pipes and wire them together by their forwards; every packet enters at one designated
**root** pipe (the top of the graph) and is routed pipe-to-pipe until it leaves. Building a pipeline
is therefore just wiring those forwards so the root eventually reaches every packet's destination —
which is exactly what the forwarder below, and your own pipeline in Part C, do. (*How* you create
each pipe — the exact calls — is in Part C, where you write them.)

NVIDIA ships a library of tiny example programs on the card, each demonstrating one DOCA Flow
concept, under:

```bash
$ ls /opt/mellanox/doca/samples/doca_flow/
flow_hairpin_vnf   flow_modify_header   flow_monitor_meter   flow_match_comparison   ... (~20 more)
```

Each folder holds a `.c` file and a `README`, and each isolates one idea — `flow_hairpin_vnf`
forwards one port to another, `flow_modify_header` rewrites header fields, `flow_monitor_meter`
attaches counters. They are worth a look when you want to see a single concept on its own.

For the hands-on part we use programs written for *this* card's two-port loopback, so every command
below is exact and works as typed — starting with our minimal forwarder, next.
<!-- AUTHOR / TODO: if we want a fully hand-held compile-AND-run of an /opt NVIDIA sample here, we
     must pick one sample and verify its exact command + output on the card first (these take
     device-specific args and are not built for the p0<->p1 loopback). -->

#### How our minimal forwarder works

You don't start from a blank file — we walk through a working example first, so the pieces feel
familiar when you build your own in Part C. The example is our minimal forwarder, `doca_flow_nop`.
All it does is **forward**: packets arriving from the wire are handed to the receiver, the receiver's
replies are sent back out the wire, and nothing about the packets changes (`nop` = "no operation").

It is the smallest complete DOCA Flow program: **one pipe with two entries**, one per direction. The
pipe matches only on *which port a packet arrived on* (`parser_meta.port_meta`), and each entry
forwards to the other port:

- **Entry 1** — a packet from the wire (port 0) → forward to the receiver SF (port 1).
- **Entry 2** — a reply from the receiver SF (port 1) → forward back out the wire (port 0).

Both are plain **forward-to-port** entries — nothing in the packet changes.

What matters most is its **shape**, because the file you edit in Part C
([`doca_flow_ecn_pcap.c`](doca-2/doca-flow/doca_flow_ecn_pcap.c)) is built the *exact same way*:

```
main()
  ├─ setup_logging → parse_args → open the device → start DOCA Flow   (boilerplate, never changes)
  ├─ build_pipeline()  ───────────────►  create the pipe(s)   ← the ONLY part that differs
  └─ loop: report the forwarded-packet counts, once a second
```

In `doca_flow_nop` that pipeline is a single `create_root_pipe()`; in Part C it grows into a handful
of `create_*` pipes. Everything around it — how the program starts, opens the card, and takes over
the eSwitch — is identical. So once this forwarder makes sense, you already know your way around the
file you are about to edit.

Here is how [`create_root_pipe()`](doca-2/doca-flow/doca_flow_nop.c) actually does it — worth reading
once, because **every pipe in Part C is built the same two steps**: first you *build the pipe*, then
you *add entries* to it. (Error checks trimmed for clarity.)

**Step 1 — build the pipe (the template).** A pipe declares *which field it matches on* and *what it
does with a match* — but not the concrete values; those come from the entries. This one matches on
the arrival port, forwards (to a port each entry will name), and counts:

```c
match.parser_meta.port_meta = UINT32_MAX;   // the field this pipe matches on (0xFF… = "look at this")
struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};   // where to send — set per entry
struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};  // count hits

doca_flow_pipe_cfg_create(&cfg, port);
doca_flow_pipe_cfg_set_is_root(cfg, true);   // every packet is checked against this pipe first
doca_flow_pipe_cfg_set_nr_entries(cfg, 2);   // it will hold two entries, one per direction
doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);
doca_flow_pipe_cfg_set_monitor(cfg, &monitor);
doca_flow_pipe_create(cfg, &fwd, &fwd_miss, &pipe);   // fwd_miss = DROP anything that matches nothing
```

**Step 2 — add the entries (the values).** A freshly built pipe does nothing until you add entries.
Each one fills in the values the pipe left open — *which* port to match, and *which* port to forward
to:

```c
// arrived on port 0 (the wire)  ->  forward to port 1 (the receiver SF)
entry_match.parser_meta.port_meta = 0;
entry_fwd = (struct doca_flow_fwd){.type = DOCA_FLOW_FWD_PORT, .port_id = 1};
doca_flow_pipe_add_entry(0, pipe, &entry_match, NULL, &monitor, &entry_fwd,
                         DOCA_FLOW_WAIT_FOR_BATCH, &status, &to_sf);

// arrived on port 1 (the SF)    ->  forward back out port 0 (the wire)
entry_match.parser_meta.port_meta = 1;
entry_fwd = (struct doca_flow_fwd){.type = DOCA_FLOW_FWD_PORT, .port_id = 0};
doca_flow_pipe_add_entry(0, pipe, &entry_match, NULL, &monitor, &entry_fwd,
                         /* flush the batch */ 0, &status, &to_wire);

doca_flow_entries_process(port, 0, 10000, 2);   // install both entries into the NIC
```

> **INFO — the one idiom you'll reuse for every pipe:** the **pipe** says *which* fields it looks at,
> and that it forwards and counts; each **entry** supplies the *values* — the port to match and the
> port to send to. **Pipe = which fields · entry = what values.** (`0xFF…` in the pipe's match means
> "this field participates"; the entry gives the real number.)

> **NOTE — the single most important idea in this whole tutorial:** the instant a DOCA Flow program
> starts, **it takes ownership of the eSwitch**. From then on, the NIC forwards *only* what your
> program's pipes say to forward. Nothing moves unless your rules move it. Keep this in mind — it
> explains everything you are about to see.

<details>
<summary><b>Try it yourself! — compile and run the forwarder</b></summary>

<br>

**Step 1 — build it.** Go to the source folder for your card's DOCA version (`doca-2` here) and
compile with `meson` (which configures the build) and `ninja` (which does the compiling):
```bash
cd doca-2
meson setup build && ninja -C build
```
This produces the program at `build/doca-flow/doca_flow_nop`. (You only need `meson setup build` the
first time; after editing code, just `ninja -C build` again.)

**Step 2 — run it (leave it running in Terminal 1).**
```bash
sudo ./build/doca-flow/doca_flow_nop
```
It prints some start-up lines and then a forwarded-packet count once a second.
<!-- AUTHOR: paste the real doca_flow_nop start-up banner + a couple of counter lines here. -->

**Step 3 — send traffic through it (Terminals 2 and 3).** Same two commands as Part A:
```bash
sudo ip netns exec ns0 ib_write_bw -d mlx5_2 -R -x 1 -F --report_gbits            # server
sudo ip netns exec ns1 ib_write_bw -d mlx5_3 -R -x 1 -F 10.0.0.1 --report_gbits   # client
```
**You should see** the same ~90 Gb/s table as before — but now every packet is being forwarded by
*your program's* pipes, and `doca_flow_nop`'s per-second counter climbs along with it.

**Step 4 — see who is in charge.** Press **Ctrl-C** in Terminal 1 to stop `doca_flow_nop`, then run
the traffic again: it still works, now over the card's *default* forwarding. Start `doca_flow_nop`
again and it flows through your pipes instead. That switch — default path vs. *your* path — is
exactly the control you will use in Part C.

</details>

---

### Part C — Build it yourself: an ECN-marking pipeline

Now you write a pipeline of your own. The goal: take the live RoCE packets and **mark each one with
an "ECN Congestion Experienced (CE)" flag** in its header, while still forwarding everything
normally.

> **INFO — why marking?** "CE" is a bit-pattern in the IP header that means *"this packet passed
> through congestion."* It is the alarm a congestion-control algorithm listens for. In Part II you
> write the algorithm that *reacts* to CE; here you write the code that *sets* it.

You start from [`doca_flow_ecn_pcap.c`](doca-2/doca-flow/doca_flow_ecn_pcap.c): the **complete
program with the pipeline removed**. Everything hard and unrelated — setting up the device, parsing
arguments, the loop that writes a capture file, the counter report — is already written and you do
not touch it. You fill in **four** short functions, marked `TODO 1`–`TODO 4`.

#### The structure of `doca_flow_ecn_pcap.c`

The program already handles everything around the pipeline for you — opening the device, DPDK and
DOCA Flow init, argument parsing (`--percent`, `--pcap`, `--sample`), the packet-capture loop, the
per-second counter report, and `main()`. **What you write is the pipeline itself** — a small set of
**pipe-builder functions** near the bottom of the file. Two are worked examples to model yours on;
four are the ones you fill in:

```
doca_flow_ecn_pcap.c
  … device / DPDK / DOCA Flow setup · arg parsing · capture loop · main() …   (given, don't touch)

  create_passthrough_pipe()     given    the 5-step pipe shape — your template for the rest
  create_to_cpu_pipe()          given    where captured copies go (used in Stage 2)
  create_root_pipe()            TODO 1   the root: sort packets by the port they arrived on → Stage 1
  create_forward_to_sf_pipe()   TODO 2   mark each packet CE, forward to the receiver       → Stage 1
  bind_capture_mirror()         TODO 3   mirror a copy of each packet to the CPU            → Stage 2
  create_sampling_pipe()        TODO 4   mark only a fraction of packets                    → optional
  build_pipeline()              given    wires the pipes above together
```
<!-- AUTHOR: for the HTML artifact, replace this ASCII block with a nicer figure. -->

The `TODO`s are numbered **in the order you fill them in**: Stage 1 is `TODO 1` (the root) and
`TODO 2` (marking), Stage 2 adds `TODO 3` (capture), and `TODO 4` is the optional sampler. (In the
file each `TODO` sits with the function it belongs to, so their positions run in a different order.)

#### First: the shape every pipe follows

Every pipe is built the same way, in **two layers** — get this split and the rest is mechanical:

- **The pipe is a template.** When you create a pipe you describe its *shape* — which header fields
  it matches on, which fields its actions may rewrite, and where a match forwards — but **not** the
  concrete values. You mark a field with `0xFF` to mean "this field participates," without yet saying
  *what* to look for. The call that compiles that shape into the hardware is `doca_flow_pipe_create()`.
- **Entries fill in the values.** A freshly created pipe does nothing until you add at least one
  **entry**, and each entry supplies the concrete values for the fields the pipe declared — "match
  IPv4 packets *with this DSCP/ECN byte*," "rewrite the ECN bits *to this value*." One pipe can carry
  many entries (a root pipe, for instance, has one per direction). You add an entry with
  `doca_flow_pipe_add_entry()`, then install it in the NIC with `doca_flow_entries_process()`.

So the one idiom behind every pipe: **the pipe says *which* fields, the entry says *what* values** —
and the same split applies to actions (the pipe declares "entries may rewrite this field," the entry
supplies the value to write).

You are not writing from a blank page: two functions are **already written for you** as examples, and
every pipe follows the exact same **five-step shape**. Here is the smaller of the two,
[`create_passthrough_pipe()`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L621), in outline — the shape all
four of your functions take:

```c
// 1. MATCH — say which fields this pipe looks at (the values come later, per entry)
match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;   // "this pipe is about IPv4 packets"
match.outer.ip4.dscp_ecn = 0xFF;               // "…and it looks at the DSCP/ECN byte"
//   (match_mask leaves that byte 0x00 = "but ignore its actual value")

// 2. FORWARD — say where a matching packet goes
struct doca_flow_fwd fwd_hit = { .type = DOCA_FLOW_FWD_PORT, .port_id = SF_REP_PORT_ID };

// 3. BUILD the pipe:  cfg_create → set_name → set_type → set_domain → set_is_root →
//                     set_nr_entries → set_match → doca_flow_pipe_create → cfg_destroy
// 4. ADD an entry:    doca_flow_pipe_add_entry(...)   ← the entry supplies the value to match/write
// 5. INSTALL & CHECK: doca_flow_entries_process(...), then confirm it installed
```

The other worked example, [`create_to_cpu_pipe()`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L670), is
where captured copies land — you'll use it in Stage 2, so set it aside for now.

With that shape in hand, you build your pipeline in **two stages**: **Stage 1** gets the NIC
*marking* packets and forwarding them — you watch the counter climb; **Stage 2** adds a mirror so you
can capture a copy and actually *see* the CE bit on the wire.
[`build_pipeline()`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L809) is where all the pieces connect.

#### Stage 1 — mark and forward

Fill in **two** functions and leave the other two alone for now.

**`create_root_pipe`** ([`TODO 1`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L794)) — the **root**. Every
packet hits this first; it sorts by the port a packet arrived on: from the wire → your marking pipe;
coming back from the receiver → straight out the wire. It is the same idea as the root pipe in the
Part B forwarder — two entries keyed on `parser_meta.port_meta`.

> **NOTE:** the "coming back from the receiver" direction must be forwarded **without** marking — it
> carries the RoCE acknowledgements and congestion signals, and marking those would corrupt the
> feedback the Part II controller depends on.

**`create_forward_to_sf_pipe`** ([`TODO 2`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L749)) — the marking
pipe. Start from the `create_passthrough_pipe` shape shown above (match IPv4, forward to the
receiver), then add two things:

- a **counter** (`monitor.counter_type`) — this is what makes the "CE marked:" number move;
- when `mark` is true, the **action that rewrites the ECN bits to CE** (the codepoint `11`).

<details>
<summary><b>Try it yourself! — make the NIC mark packets</b></summary>

<br>

**Step 1 — fill in `create_root_pipe` and `create_forward_to_sf_pipe`** (the root pipe and the
marking pipe); leave the other two functions alone for now. When a pipe is wrong, DOCA prints a wall
of red text — the **last** line is the one that matters, and it names the pipe that failed.

**Step 2 — build it and run it, with traffic.** Build, start your program in Terminal 1, then start
traffic with `benchmark.sh` in Terminal 2:
```bash
cd doca-2 && meson setup build && ninja -C build
sudo ./build/doca-flow/doca_flow_ecn_pcap -- --percent 100   # Terminal 1 — leave it running
./scripts/benchmark.sh                                        # Terminal 2 — server + client + chart
```
> **INFO — the `--` and `--percent`.** The `--` is required: everything *before* it is for the DPDK
> library, everything *after* it is for our program. For Stage 1 the only flag you need is
> **`--percent N`** — CE-mark this share of packets, `0`–`100` (default `100` = mark everything).
> (The program also accepts `--pcap` and `--sample`; those come in Stage 2 and the optional step.)

Your marking program prints a line once a second — with no capture it looks like:
```
CE marked: 57060637, passthrough: 0 (100% marked)
```
**`CE marked:` climbing** means packets are flowing through your marking pipe, and `benchmark.sh`
should show near line rate (~92 Gb/s) at the same time. You are marking packets in hardware.

</details>

#### Stage 2 — capture a copy, and see the mark

Marking works, but so far you are trusting a counter. Now send a **copy** of every packet to a
capture file so you can look at the ECN bits yourself. The copy is made by a **mirror**, and it lands
in the `create_to_cpu_pipe` you set aside earlier (already written for you).

Two small additions, both about the mirror:

**1. `bind_capture_mirror`** ([`TODO 3`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L723)). It builds no
pipe — it configures a shared "mirror" resource and points it at `cpu_pipe`, so a *copy* of each
packet is delivered to the CPU (and your pcap) while the original keeps forwarding. Follow the
step-by-step in the comment above it.

**2. Use the `mirror` parameter** you ignored in Stage 1. Back in `create_forward_to_sf_pipe`, add the
small `if (mirror) …` block that attaches the shared mirror to the marking pipe (the comment there
shows how). Now, and only when `--pcap` makes `mirror` true, each marked packet is also copied to the
capture — while the original still forwards at line rate.

<details>
<summary><b>Try it yourself! — capture the traffic and see the CE mark</b></summary>

<br>

**Step 1 — fill in `bind_capture_mirror`**, rebuild, and run **with a capture file** this time:
```bash
sudo ./build/doca-flow/doca_flow_ecn_pcap -- --pcap /tmp/out.pcap --percent 100
```

**Step 2 — traffic.** `./scripts/benchmark.sh` from Stage 1 can keep running — or start it again if
you stopped it. The program's per-second line now has a capture half, and it starts **paused**:
```
CE marked: 57060637, passthrough: 0 (100% marked) | mirrored: 2743653 -> pcap: 0 [PAUSED]
```
`mirrored:` climbing = copies are reaching the CPU. `-> pcap: 0 [PAUSED]` = writing to the *file*
hasn't started (marking and forwarding never pause — only the file writing does).

**Step 3 — start writing and grab a few seconds.** Press **SPACE** in the program's terminal (or, from
another shell, `sudo kill -USR1 <pid>`); the `[PAUSED]` disappears and `-> pcap:` climbs. After a few
seconds, stop the program with **Ctrl-C** — that flushes and closes the file cleanly.

**Step 4 — see your mark.** Open the capture with `tcpdump`:
```bash
tcpdump -vnn -r /tmp/out.pcap | head
#   marked packets show   tos 0x3,CE     ← this is your CE mark
#   unmarked ones show     tos 0x2        (the original value, "ECT")
```
A quick tally — at `--percent 100` essentially every packet should be `CE`:
```bash
tcpdump -vnn -c 8000 -r /tmp/out.pcap 2>/dev/null | grep -oE 'tos 0x[0-9a-f]+(,CE)?' | sort | uniq -c
#   7994 tos 0x3,CE
```
And `benchmark.sh` still showed ~92 Gb/s throughout — the copy is made in the eSwitch hardware, so
capturing costs no throughput.

</details>

#### Going further (optional) — mark only some packets

So far every packet is marked (`--percent 100`). To mark only a fraction, fill in the last function,
**`create_sampling_pipe`** ([`TODO 4`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L771)): it matches the
NIC's built-in random field (`parser_meta.random`) so that 1-in-N packets take the marking path and
the rest are forwarded unmarked. Then run, for example:
```bash
sudo ./build/doca-flow/doca_flow_ecn_pcap -- --pcap /tmp/out.pcap --percent 50 --sample 8
```
and the tally from Stage 2 shows a roughly even split of `tos 0x2` (unmarked) and `tos 0x3,CE`
(marked), with only ~1-in-8 packets written to the file.

#### Check your answer

The finished program ships in the **solutions** repository at the same path. A `diff` should show
**only the function bodies you wrote** changed — nothing else:
```bash
diff doca-2/doca-flow/doca_flow_ecn_pcap.c <solutions-repo>/doca-2/doca-flow/doca_flow_ecn_pcap.c
```

---

## What you built

You can now:

- treat the card as a two-port loopback and put real RoCE traffic on it (`mlx5_2`/`mlx5_3`,
  `ib_write_bw`);
- follow how **match → count → modify → forward** pipes chain from a root pipe;
- write a pipeline that **matches** IPv4, **counts** it, **rewrites** its ECN bits to CE,
  **forwards** it, and **mirrors** a copy to a capture file — the exact building block the congestion
  controller in Part II reacts to.

---

## Debugging Tips

- **Read the *last* error line, not the first.** A failed pipe prints a wall of internal DOCA errors.
  The final `[CRT]…[doca_check] <name>: <reason>` line names the pipe (`<name>`) and the reason. The
  word before the colon tells you which function to open.
- **"It runs but nothing forwards"** → you have not filled in the **root pipe** (`create_root_pipe`).
  With no root, none of the other pipes are reachable, no matter how correct they are.
- **Throughput collapses when your program runs** → the data path is going somewhere it should not.
  Re-check the forwards in `create_forward_to_sf_pipe` and the two entries in `create_root_pipe`.
- **`CE marked:` stays at 0** → either your marking pipe has no counter (`monitor.counter_type`), or
  the root pipe never sends wire traffic into it.
- **The pcap file stays empty** → capturing **starts paused** — press **SPACE** to begin, and run the
  program in the **foreground** (SPACE needs a real terminal; launched with `nohup`/`</dev/null` it
  can never be un-paused). The `[PAUSED]` tag on the per-second line tells you which state you're in.
- **A new run says the device is busy** → only one DOCA Flow program can own the eSwitch at a time.
  Stop the previous one (Ctrl-C). If it was killed hard, `sudo pkill -f doca_flow` and, if the
  namespaces are gone, re-run `setup_roce_loopback.sh`.
- **`ib_write_bw` says "Couldn't connect"** → start the **server** (`ns0`) *before* the client
  (`ns1`), and make sure something (your forwarder, or the default path) is actually moving packets.

---

## FAQs

**I've never written C or used DOCA. Can I still do Part C?**
Yes. You only fill in four short functions, and each is a small variation of an example shown to you
(`create_passthrough_pipe`, `create_to_cpu_pipe`, and the forwarder from Part B). The comment above
each `TODO` walks you through it, and you can always `diff` against the finished file to check
yourself.

**Why do I need two sub-functions (`mlx5_2`/`mlx5_3`) and not just the two ports?**
The ports carry the wire; the SFs are the *endpoints* that send and receive a flow. One SF per
namespace makes the single card behave like two hosts talking across the cable.

**Why does all traffic stop the instant I start `doca_flow_ecn_pcap`?**
Because your program takes over the eSwitch, and an empty pipeline forwards nothing. Install the root
pipe and the pipes it points to and traffic flows again.

**What's the difference between a pipe's match and an entry's match?**
The pipe declares *which fields* participate (`0xFF` = "this field participates"); each entry supplies
the *value* to compare or write. One pipe can have many entries with different values.

**Why does Stage 1 use no `--pcap`?**
Stage 1 is only about *marking*. Capturing to a file needs the mirror you add in Stage 2, so until
then there is nothing to write — you confirm marking with the `CE marked:` counter instead.

**Why `-R` on `ib_write_bw`?**
It makes RoCE establish its connection through the RDMA connection manager. It is harmless here and is
required later so the Part II controller can attach to the flow — so just always keep it on.

**Do I ever change anything outside the four `TODO` functions?**
No. The template and the finished `doca_flow_ecn_pcap.c` are identical everywhere else — a `diff`
should show only your four function bodies.

---

**Next → Part II: Programmable Congestion Control (DOCA PCC).** You just learned to *mark* the
congestion signal in the data plane; next you write the controller that *reacts* to it, running on
the NIC's Data-Path Accelerator.
