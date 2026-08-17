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
  "CE marked:" lines are real; the doca_flow_ecn_pcap banner and sample list are illustrative —
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
- **Part B** — meet DOCA Flow, then write your first pipe: the root pipe that makes a small, complete
  program forward packets, so the idiom is in your hands before Part C.
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
Hitting **line rate** is your proof the whole path works end to end: sender → `p1` → cable → `p0` → the
eSwitch → receiver. If you see a table with a real number, Part A is done.

> You just ran the server and client by hand to see the moving parts. From here on you don't have
> to: **`./scripts/benchmark.sh`** starts both ends together in one command and shows the sender's
> goodput as a live, updating chart (Gb/s), so you never retype those flags again. (It wraps
> `./scripts/run_server.sh` and `./scripts/run_client.sh` if you ever want them separately; Ctrl-C
> stops everything.)

</details>

---

### Part B — Meet DOCA Flow, then write your first pipe

#### What DOCA Flow is

**DOCA Flow** is how you program that eSwitch fast path in C. Your program builds a small graph of
**pipes**. Think of each pipe as one rule with four parts:

| part        | the question it answers                | example                                |
| ----------- | -------------------------------------- | -------------------------------------- |
| **match**   | *which* packets does this pipe act on? | "all IPv4 packets"                     |
| **count**   | *how many* packets hit it?             | a hardware counter you can read        |
| **actions** | *what* to change in the packet?        | rewrite a header field (or nothing)    |
| **forward** | *where* does the packet go next?       | another pipe, a port, the CPU, or drop |

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

You don't start from a blank file. The program you build on —
[`doca_flow_ecn_pcap.c`](doca-2/doca-flow/doca_flow_ecn_pcap.c) — is **a plain forwarder**: packets
arriving from the wire are handed to the receiver, the receiver's replies go back out the wire, and
nothing about the packets changes. It ships **almost** complete — every pipe is written for you
*except the root pipe*, `create_root_pipe`, which is the one piece **you** write here in Part B.
Fill it in and traffic flows at line rate, unmarked; turning that forwarder into an ECN marker is
then the whole of Part C. First, though, meet the two-step idiom every pipe is built from — the same
one you are about to use.

Its **shape is the shape of the whole program**:

```
main()
  ├─ setup_logging → parse_args → open the device → start DOCA Flow   (boilerplate, never changes)
  ├─ build_pipeline()  ───────────────►  create the pipe(s)   ← the ONLY part that differs
  └─ loop: report the forwarded-packet counts, once a second
```

As shipped, [`build_pipeline()`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L837) wires up **two** pipes —
the whole no-op data path:

- **[`create_root_pipe()`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L782)** — the **root**: every packet
  is checked here first, sorted by *which port it arrived on* (`parser_meta.port_meta`). From the
  wire → on to the forwarding pipe; back from the receiver SF → straight out the wire. **This is the
  one you write, in the steps below** — it ships with its two halves left as `TODO`s.
- **[`create_passthrough_pipe()`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L621)** — the **forwarding
  pipe** (given): matches IPv4 and forwards to the receiver SF, unchanged; the smallest complete
  instance of the five-step shape every pipe in the file follows.

In Part C this same `build_pipeline()` grows into a handful of `create_*` pipes. Everything around it
— how the program starts, opens the card, and takes over the eSwitch — is identical, so once this
forwarder makes sense you know your way around the file you are about to edit.

Here is the two-step idiom you'll use to write
[`create_root_pipe()`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L782) — and **every pipe in Part C is
built the same two steps**: first you *build the pipe*, then you *add entries* to it. (Error checks
trimmed for clarity.)

**Step 1 — build the pipe (the template).** A pipe declares *which field it matches on* and *what it
does with a match* — but not the concrete values; those come from the entries. This one matches on
the arrival port and forwards to a target each entry will name:

```c
// match, match_mask, fwd_hit, fwd_miss and pipe are already declared for you.
match.parser_meta.port_meta      = UINT32_MAX;   // match on the arrival port …
match_mask.parser_meta.port_meta = UINT32_MAX;   // … comparing that field exactly (full mask)
fwd_hit.type  = DOCA_FLOW_FWD_CHANGEABLE;        // where a match goes — each entry names it
fwd_miss.type = DOCA_FLOW_FWD_DROP;              // anything matching nothing → dropped
doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask);   // teach the cfg what to match on
doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe);   // build the pipe
```

**Step 2 — add the entries (the values).** A freshly built pipe does nothing until you add entries.
Each one fills in the values the pipe left open — *which* port to match, and *where* to forward:

```c
// install_status, entry, entry_match and entry_fwd are already declared for you.

// from the wire (PF_PORT_ID)  ->  on to the forwarding pipe (wire_target)
entry_match.parser_meta.port_meta = PF_PORT_ID;
entry_fwd.type = DOCA_FLOW_FWD_PIPE;
entry_fwd.next_pipe = wire_target;
doca_flow_pipe_add_entry(PIPE_QUEUE, pipe, &entry_match, NULL, NULL, &entry_fwd,
                         DOCA_FLOW_WAIT_FOR_BATCH, &install_status, &entry);

// from the receiver SF (SF_REP_PORT_ID)  ->  back out the wire (PF_PORT_ID)
entry_match.parser_meta.port_meta = SF_REP_PORT_ID;
memset(&entry_fwd, 0, sizeof(entry_fwd));   // reuse the struct — clear it first
entry_fwd.type = DOCA_FLOW_FWD_PORT;
entry_fwd.port_id = PF_PORT_ID;
doca_flow_pipe_add_entry(PIPE_QUEUE, pipe, &entry_match, NULL, NULL, &entry_fwd,
                         DOCA_FLOW_NO_WAIT, &install_status, &entry);

doca_flow_entries_process(port, PIPE_QUEUE, ENTRY_PROCESS_TIMEOUT_US, nb_entries);   // install both
```

> **INFO — the one idiom you'll reuse for every pipe:** the **pipe** says *which* fields it looks at
> and that it forwards; each **entry** supplies the *values* — the port to match and where to send.
> **Pipe = which fields · entry = what values.** (`0xFF…` / `FWD_CHANGEABLE` in the pipe means "an
> entry fills this in"; the entry gives the real value — and a forward target can be a **pipe** or a
> **port**.)

> **NOTE — the single most important idea in this whole tutorial:** the instant a DOCA Flow program
> starts, **it takes ownership of the eSwitch**. From then on, the NIC forwards *only* what your
> program's pipes say to forward. Nothing moves unless your rules move it. Keep this in mind — it
> explains everything you are about to see, starting with why an unfinished `create_root_pipe`
> forwards nothing at all.

<details>
<summary><b>Try it yourself! — write the root pipe, then compile and run the forwarder</b></summary>

<br>

**Step 1 — write `create_root_pipe`.** Open
[`create_root_pipe()`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L782). Everything mechanical is already
there — the cfg boilerplate (`cfg_create`, name/type/domain, `set_is_root`, `set_nr_entries`), the
teardown, **and every struct you need is already declared for you** (`match`, `match_mask`,
`fwd_hit`, `fwd_miss`, `pipe`; then `install_status`, `entry`, `entry_match`, `entry_fwd`). You only
fill in the fields and the calls, in two `TODO` gaps:

- [**`TODO` — Part B, Step 1**](doca-2/doca-flow/doca_flow_ecn_pcap.c#L802) — *build the pipe*: set
  the fields on `match`/`match_mask`/`fwd_hit`/`fwd_miss`, then `set_match` and
  `doca_flow_pipe_create`. (Copy the **Step 1** snippet above.)
- [**`TODO` — Part B, Step 2**](doca-2/doca-flow/doca_flow_ecn_pcap.c#L815) — *add the entries*: fill
  in and install the two entries — wire → `wire_target` (`WAIT_FOR_BATCH`); receiver SF → the uplink
  port (`NO_WAIT`); then `doca_flow_entries_process`. (Copy the **Step 2** snippet above.)

Copy each snippet into its gap. Until you do, the program builds but forwards nothing.

**Step 2 — build it.** Go to the source folder for your card's DOCA version (`doca-2` here) and
compile with `meson` (which configures the build) and `ninja` (which does the compiling):
```bash
cd doca-2
meson setup build && ninja -C build
```
This produces the program at `build/doca-flow/doca_flow_ecn_pcap`. (You only need `meson setup build`
the first time; after editing code, just `ninja -C build` again.)

**Step 3 — run your forwarder (leave it running in Terminal 1).**
```bash
sudo ./build/doca-flow/doca_flow_ecn_pcap
```
With `create_root_pipe` filled in it is the plain forwarder — it prints its start-up lines and then
sits forwarding. (Run it *without* writing the root pipe and it starts but forwards nothing: the
eSwitch is yours, but you've given it no root to route from.)
<!-- AUTHOR: paste the real as-shipped doca_flow_ecn_pcap start-up banner here. -->

**Step 4 — send traffic through it (Terminal 2).**
```bash
./scripts/benchmark.sh
```
**You should see** the same **line rate** as in Part A — but now every packet is being forwarded by *your
program's* pipes.

**Step 5 — see who is in charge.** Press **Ctrl-C** in Terminal 1 to stop the program, then run the
traffic again: it still works, now over the card's *default* forwarding. Start it again and it flows
through your pipes instead. That switch — default path vs. *your* path — is exactly the control you
will use in Part C.

</details>

---

### Part C — Build it yourself: an ECN-marking pipeline

Now you write a pipeline of your own. The goal: take the live RoCE packets and **mark each one with
an "ECN Congestion Experienced (CE)" flag** in its header, while still forwarding everything
normally.

> **INFO — why marking?** "CE" is a bit-pattern in the IP header that means *"this packet passed
> through congestion."* It is the alarm a congestion-control algorithm listens for. In Part II you
> write the algorithm that *reacts* to CE; here you write the code that *sets* it.

You start from [`doca_flow_ecn_pcap.c`](doca-2/doca-flow/doca_flow_ecn_pcap.c): a **complete, working
forwarder** — the same no-op program you ran in Part B. Everything hard and unrelated — setting up
the device, parsing arguments, the loop that writes a capture file, the counter report — is already
written and you do not touch it. To turn it into an ECN marker you **flip `build_pipeline()` from its
no-op wiring to the ECN pipeline** (comment one line, uncomment a block) and fill in **three** short
functions, marked `TODO 1--3`.

#### The structure of `doca_flow_ecn_pcap.c`

The program already handles everything around the pipeline for you — opening the device, DPDK and
DOCA Flow init, argument parsing (`--percent`, `--pcap`, `--sample`), the packet-capture loop, the
per-second counter report, and `main()`. **What you write is the pipeline itself** — a small set of
**pipe-builder functions** near the bottom of the file. Two are worked examples to model yours on;
three are the ones you fill in:

```
doca_flow_ecn_pcap.c
  … device / DPDK / DOCA Flow setup · arg parsing · capture loop · main() …   (given, don't touch)

  create_passthrough_pipe()     given    the 5-step pipe shape — your template for the rest
  create_to_cpu_pipe()          given    where captured copies go (used in Stage 2)
  create_root_pipe()            Part B   the root you wrote in Part B: sorts by arrival port
  create_forward_to_sf_pipe()   TODO 1   mark each packet CE, forward to the receiver       → Stage 1
  bind_capture_mirror()         TODO 2   mirror a copy of each packet to the CPU            → Stage 2
  create_sampling_pipe()        TODO 3   mark only a fraction of packets                    → optional
  build_pipeline()              given*   wires the pipes together — you flip it from no-op to ECN
```
<!-- AUTHOR: for the HTML artifact, replace this ASCII block with a nicer figure. -->

`build_pipeline()` ships wired as the **no-op forwarder** from Part B; your first move in Stage 1 is
to switch it to the ECN pipeline (comment out one line, uncomment the block under it). The three
`TODO`s are numbered **in the order you fill them in**: `TODO 1` (marking) in Stage 1, `TODO 2`
(capture) in Stage 2, and `TODO 3` (the optional sampler). (In the file each `TODO` sits with its
function, so their positions run in a different order.)

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
three of your functions take:

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
[`build_pipeline()`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L837) is where all the pieces connect.

#### Stage 1 — mark and forward

First switch the program from plain forwarding to marking, then write the one pipe that marks.

**Flip `build_pipeline()` from no-op to ECN.** As shipped (Part B) it runs one line of no-op wiring,
with the ECN pipeline right below it, commented out. In
[`build_pipeline()`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L837), **comment out the single no-op line
and uncomment the block under it** — the comments there mark exactly which line and which block. That
block calls the three `TODO` functions; until you fill them in the compiler warns they are "defined
but not used," which is how you know what is still left to write.

> **NOTE — the return direction is already handled.** Your `create_root_pipe` (from Part B) already
> forwards traffic *coming back from the receiver* straight out the wire, **unmarked** — it carries the RoCE
> acknowledgements and congestion signals, and marking those would corrupt the feedback the Part II
> controller depends on. You only write the *forward* (wire → receiver) marking path.

**`create_forward_to_sf_pipe`** ([`TODO 1`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L746)) — the marking
pipe. Start from the `create_passthrough_pipe` shape shown above (match IPv4, forward to the
receiver), then add two things:

- a **counter** (`monitor.counter_type`) — this is what makes the "CE marked:" number move;
- when `mark` is true, the **action that rewrites the ECN bits to CE** (the codepoint `11`).

<details>
<summary><b>Try it yourself! — make the NIC mark packets</b></summary>

<br>

**Step 1 — flip `build_pipeline()` to ECN, then fill in `create_forward_to_sf_pipe`** (`TODO 1`, the
marking pipe); leave `TODO 2` and `TODO 3` alone for now. When a pipe is wrong, DOCA prints a wall of
red text — the **last** line is the one that matters, and it names the pipe that failed.

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
should show **near line rate** at the same time. You are marking packets in hardware.

</details>

#### Stage 2 — capture a copy, and see the mark

So far the only sign your marking works is a counter ticking up — you have not actually *seen* a CE
bit on a packet. Stage 2 fixes that: you capture live traffic to a file and read the ECN bits with
`tcpdump`. To do that without slowing the data path, the NIC makes a **copy** of each packet and
sends the copy to the CPU (and on to your capture file) while the original keeps forwarding at line
rate. That copy mechanism is a **mirror**; the copies are received by `create_to_cpu_pipe`, which is
already written for you — so the only new thing *you* wire up is the mirror itself.

That is two changes, and the comment above each one in the source walks you through it step by step;
here is what they are and why:

**1. Write `bind_capture_mirror`** ([`TODO 2`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L723)) — it
builds no pipe of its own. It sets up the shared *mirror* once and aims it at `cpu_pipe`, so a copy
of every hit reaches the CPU (and your pcap) while the original keeps going. Follow the numbered
steps in the comment directly above the function.

**2. Switch the mirror on in your marking pipe.** In `create_forward_to_sf_pipe` — the TODO 1 you
already wrote — there is a `mirror` parameter you left unused in Stage 1. Add the short
`if (mirror) …` block the comment points to; it attaches the shared mirror to that pipe. Now, only
when you pass `--pcap` (which is what makes `mirror` true), each marked packet is copied to the
capture too, and the original still forwards at full speed.

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

**Step 3 — start writing and grab a few seconds.** Press **SPACE** in the program's terminal; the `[PAUSED]` disappears and `-> pcap:` climbs. After a few
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
And `benchmark.sh` still showed **line rate** throughout — the copy is made in the eSwitch hardware, so
capturing costs no throughput.

</details>

#### Going further (optional) — mark only some packets

So far every packet is marked (`--percent 100`). To mark only a fraction, fill in the last function,
**`create_sampling_pipe`** ([`TODO 3`](doca-2/doca-flow/doca_flow_ecn_pcap.c#L762)): it matches the
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
Yes. You only fill in three short functions, and each is a small variation of an example shown to you
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

**Do I ever change anything outside the `TODO` functions?**
No. The template and the finished `doca_flow_ecn_pcap.c` are identical everywhere else — a `diff`
should show only your three function bodies (plus the one-line/one-block flip in `build_pipeline`).

---

**Next → Part II: Programmable Congestion Control (DOCA PCC).** You just learned to *mark* the
congestion signal in the data plane; next you write the controller that *reacts* to it, running on
the NIC's Data-Path Accelerator.
