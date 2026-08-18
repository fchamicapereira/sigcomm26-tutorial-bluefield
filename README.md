# Programming SmartNICs: From Packet Processing to Programmable Transport

This is the **organizers' repository** for the tutorial "Programming SmartNICs: From Packet
Processing to Programmable Transport", held at [SIGCOMM 2026](https://conferences.sigcomm.org/sigcomm/2026/tutorials/smartnic/).
It holds the exercise sources, the guides that were handed out, the fleet tooling that ran a dozen
BlueField-3 cards across five sites, and the bring-up notes behind all of it.

**The tutorial has been delivered and this repository is frozen.** It is kept as a record and as a
starting point for anyone who wants to run the same exercises on their own BlueField-3 — not as an
actively maintained project. Everything below describes what was actually built and run; nothing
here is a plan.

Participants worked from a separate, much smaller repository —
[`sigcomm26-tutorial-bluefield-participants`](https://github.com/fchamicapereira/sigcomm26-tutorial-bluefield-participants)
— which is **generated from this one** by [`admin/update_participants_repo_on_github.py`](admin/update_participants_repo_on_github.py).
No solutions ship there. If you want the exercises as a participant saw them, read that repo; if you
want the answers, they are here.

## Repository layout

| Path                 | What                                                                                                                                                                                                                                                                         |
| -------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| [`guides/`](guides/) | **The participant-facing material.** One Markdown source per guide plus the rendered PDF that was handed out. This is the best entry point to the tutorial's actual content.                                                                                                 |
| [`doca-2/`](doca-2/) | The exercises built against **DOCA 2.x** (developed on 2.9): `doca-flow/`, `doca-pcc-ecn/`, and a symlink to the shared `pcc-path-steering/`. Self-contained meson project.                                                                                                  |
| [`doca-3/`](doca-3/) | The same exercises against **DOCA 3.x** (verified on 3.1 and 3.4), plus the real `pcc-path-steering/` tree. Self-contained meson project.                                                                                                                                    |
| [`admin/`](admin/)   | Fleet tooling. [`fleet.py`](admin/fleet.py) drives every card over ssh; [`local_scripts/`](admin/local_scripts/) are the per-card scripts it ships; [`update_participants_repo_on_github.py`](admin/update_participants_repo_on_github.py) regenerates the participant repo. |
| [`docs/`](docs/)     | Graphviz sources and rendered figures used by the guides and this README (`make -C docs`).                                                                                                                                                                                   |
| [`slides/`](slides/) | The hands-on slide deck and the screenshots in it.                                                                                                                                                                                                                           |
| Root scripts         | [`run_container.sh`](run_container.sh), [`run_server.sh`](run_server.sh), [`run_client.sh`](run_client.sh), [`benchmark.sh`](benchmark.sh), [`check_ecn_bits_from_pcap.sh`](check_ecn_bits_from_pcap.sh) — traffic generation and helper wrappers.                     |

**Why two `doca-N/` trees.** A BlueField-3 only builds against the DOCA release it is running, and
the fleet was not uniform: some cards shipped DOCA 2.x, others 3.x. Rather than branch inside the
sources, each release gets its own directory with its own `meson.build`.
The exercise sources are deliberately kept as close as possible between them; where the SDK forced a
difference it is absorbed by a `doca_flow_compat.h` shim and documented in the relevant README.
`admin/fleet.py doca` prints which tree each card needs.

`doca-2/pcc-path-steering` is a **symlink** to `doca-3/pcc-path-steering` — the path-steering bonus
exercise is one tree serving both, with its DOCA 2.9/3.x differences handled internally. (It used to
be a git submodule; it was frozen and vendored into this repository, so no `--recurse-submodules` is
needed.)

> **`guides/` is the only description of the exercises.** Earlier root-level drafts
> (`tutorial-doca-flow.md`, `tutorial-doca-pcc.md`) were removed once they had drifted from the code;
> `git log` has them if you ever want them. Anything that describes an exercise now lives in
> `guides/`, next to the PDF that was actually handed out.

# The exercises

Three hands-on exercises, each with its own guide in [`guides/`](guides/). They are one continuous
story: mark congestion, react to it, then use the reaction to steer traffic.

| Guide                                                                | Exercise                                                                                                                                          | Sources                                                                                        |
| -------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| [`guides/doca-flow.md`](guides/doca-flow.md) (Part 1)                | Program the eSwitch with **DOCA Flow** to CE-mark a chosen fraction of live RoCE traffic. The participant writes three pipe-building functions.   | [`doca-2/doca-flow/`](doca-2/doca-flow/), [`doca-3/doca-flow/`](doca-3/doca-flow/)             |
| [`guides/doca-pcc.md`](guides/doca-pcc.md) (Part 2)                  | Write the two rate reactions of a DCQCN-style **DOCA PCC** reaction point, running on the **DPA**, that responds to the CNPs those marks provoke. | [`doca-2/doca-pcc-ecn/`](doca-2/doca-pcc-ecn/), [`doca-3/doca-pcc-ecn/`](doca-3/doca-pcc-ecn/) |
| [`guides/pcc-path-steering.md`](guides/pcc-path-steering.md) (bonus) | Combine the two: feed per-QP PCC rate reports back into a live DOCA Flow hash pipe to split traffic across two virtual paths.                     | [`doca-3/pcc-path-steering/`](doca-3/pcc-path-steering/)                                       |
| [`guides/tailscale.md`](guides/tailscale.md)                         | Pre-tutorial setup: how participants joined the tailnet that reached the cards.                                                                   | —                                                                                              |

Each participant worked on **one** BlueField-3, whose two ports are reachable to each other, so a
single card behaves like a small two-node network talking to itself: an SF on each port acts as sender
and receiver, and traffic leaving `p1` arrives at `p0` where the marking happens. The cards were ours
and remote — 12 of them across five sites, reached over Tailscale ([The fleet](#the-fleet)).

Everything is **DOCA Flow** and **DOCA PCC** on a BlueField-3, in C, built and run on the card's own
Arm cores.

# Physical setup

Everything from here on is the **operational record**: how a card has to be configured for the
exercises to work, what was measured, and which of it was hard-won. The concrete values below are
from `bf3-ulisbon-1`, a 100G card with its two ports joined by a DAC cable — it is not special, just
the card these notes were taken on; see
[The fleet](#the-fleet) for the cards the tutorial actually ran on and
[How much the cards differ](#how-much-the-cards-differ) for what varies between them. All commands are
run on the Arm cores of the BlueField-3.

```bash
ubuntu@bluefield-1:~/sigcomm26-tutorial-bluefield$ sudo bfver
--/dev/mmcblk0boot1
BlueField ATF version: v2.2(release):4.9.1-21-gfc25b08d9
BlueField UEFI version: 4.9.1-36-g0c3239837a
BlueField BSP version: 4.9.1.13442

OS Release Version: bf-bundle-2.9.1-40_24.11-ubuntu-22.04_prod

ubuntu@bluefield-1:~/sigcomm26-tutorial-bluefield$ sudo mst status -v
MST modules:
------------
    MST PCI module is not loaded
    MST PCI configuration module loaded
PCI devices:
------------
DEVICE_TYPE             MST                           PCI       RDMA            NET                                     NUMA
BlueField3(rev:1)       /dev/mst/mt41692_pciconf0.1   03:00.1   mlx5_1          net-p1,net-pf1hpf,net-en3f1pf1sf0       -1

BlueField3(rev:1)       /dev/mst/mt41692_pciconf0     03:00.0   mlx5_0          net-pf0hpf,net-en3f0pf0sf0,net-p0       -1

ubuntu@bluefield-1:~/sigcomm26-tutorial-bluefield$ sudo mlxlink -d /dev/mst/mt41692_pciconf0 -p 1 -m -c

Operational Info
----------------
State                              : Active
Physical state                     : LinkUp
Speed                              : 100G
Width                              : 4x
FEC                                : Standard RS-FEC - RS(528,514)
Loopback Mode                      : No Loopback
Auto Negotiation                   : ON

Supported Info
--------------
Enabled Link Speed (Ext.)          : 0x00000200 (100G_4X)
Supported Cable Speed (Ext.)       : 0x000002f2 (100G_4X,50G_2X,40G,25G,10G,1G)

Troubleshooting Info
--------------------
Status Opcode                      : 0
Group Opcode                       : N/A
Recommendation                     : No issue was observed

Tool Information
----------------
Firmware Version                   : 32.43.2402
amBER Version                      : 3.6
MFT Version                        : mft 4.30.1-8

Module Info
-----------
Temperature [C]                    : N/A
Voltage [mV]                       : N/A
Bias Current [mA]                  : N/A
Rx Power Current [dBm]             : N/A
Tx Power Current [dBm]             : N/A
Identifier                         : QSFP28
Compliance                         : 100GBASE-CR4, 25GBASE-CR CA-25G-L or 50GBASE-CR2 with RS (Clause91) FEC
Cable Technology                   : Copper cable, passive, unequalized
Cable Type                         : Passive copper cable
OUI                                : Other
Vendor Name                        : FS
Vendor Part Number                 : Q28-PC03
Vendor Serial Number               : S2202655321-1
Rev                                : A
Wavelength [nm]                    : N/A
Transfer Distance [m]              : 3
Attenuation (5g,7g,12g)[dB]        : 0,0,0
FW Version                         : N/A
Digital Diagnostic Monitoring      : No
Power Class                        : N/A
CDR RX                             : N/A
CDR TX                             : N/A
LOS Alarm                          : N/A
SNR Media Lanes [dB]               : N/A
SNR Host Lanes [dB]                : N/A
IB Cable Width                     : 1x,2x,4x
Memory Map Revision                : 8
Linear Direct Drive                : 0
Cable Breakout                     : Channels implemented [1,2,3,4]/Cable with single far-end with 4 channels implemented, or separable module with a 4-channel connector
SMF Length                         : N/A
MAX Power                          : 0
Cable Rx AMP                       : N/A
Cable Rx Emphasis                  : N/A
Cable Rx Post Emphasis             : N/A
Cable Tx Equalization              : N/A
Wavelength Tolerance               : N/A
Module State                       : N/A
DataPath state [per lane]          : N/A
Rx Output Valid [per lane]         : N/A
Nominal bit rate                   : 0.000Gb/s
Rx Power Type                      : OMA
Manufacturing Date                 : 10_03_22
Active Set Host Compliance Code    : N/A
Active Set Media Compliance Code   : N/A
Error Code Response                : N/A
Module FW Fault                    : N/A
DataPath FW Fault                  : N/A
Tx Fault [per lane]                : N/A
Tx LOS [per lane]                  : N/A
Tx CDR LOL [per lane]              : N/A
Rx LOS [per lane]                  : N/A
Rx CDR LOL [per lane]              : N/A
Tx Adaptive EQ Fault [per lane]    : N/A

Physical Counters and BER Info
------------------------------
Time Since Last Clear [Min]        : 25074.8
Effective Physical Errors          : 0
Effective Physical BER             : 15E-255
Raw Physical Errors Per Lane       : 0,0,0,0
Link Down Counter                  : 0
Link Error Recovery Counter        : 0
Raw Physical BER                   : 15E-255

ubuntu@bluefield-1:~/sigcomm26-tutorial-bluefield$ sudo mlxlink -d /dev/mst/mt41692_pciconf0.1 -p 1 -m -c

Operational Info
----------------
State                              : Active
Physical state                     : LinkUp
Speed                              : 100G
Width                              : 4x
FEC                                : Standard RS-FEC - RS(528,514)
Loopback Mode                      : No Loopback
Auto Negotiation                   : ON

Supported Info
--------------
Enabled Link Speed (Ext.)          : 0x00000200 (100G_4X)
Supported Cable Speed (Ext.)       : 0x000002f2 (100G_4X,50G_2X,40G,25G,10G,1G)

Troubleshooting Info
--------------------
Status Opcode                      : 0
Group Opcode                       : N/A
Recommendation                     : No issue was observed

Tool Information
----------------
Firmware Version                   : 32.43.2402
amBER Version                      : 3.6
MFT Version                        : mft 4.30.1-8

Module Info
-----------
Temperature [C]                    : N/A
Voltage [mV]                       : N/A
Bias Current [mA]                  : N/A
Rx Power Current [dBm]             : N/A
Tx Power Current [dBm]             : N/A
Identifier                         : QSFP28
Compliance                         : 100GBASE-CR4, 25GBASE-CR CA-25G-L or 50GBASE-CR2 with RS (Clause91) FEC
Cable Technology                   : Copper cable, passive, unequalized
Cable Type                         : Passive copper cable
OUI                                : Other
Vendor Name                        : FS
Vendor Part Number                 : Q28-PC03
Vendor Serial Number               : S2202655321-2
Rev                                : A
Wavelength [nm]                    : N/A
Transfer Distance [m]              : 3
Attenuation (5g,7g,12g)[dB]        : 0,0,0
FW Version                         : N/A
Digital Diagnostic Monitoring      : No
Power Class                        : N/A
CDR RX                             : N/A
CDR TX                             : N/A
LOS Alarm                          : N/A
SNR Media Lanes [dB]               : N/A
SNR Host Lanes [dB]                : N/A
IB Cable Width                     : 1x,2x,4x
Memory Map Revision                : 8
Linear Direct Drive                : 0
Cable Breakout                     : Channels implemented [1,2,3,4]/Cable with single far-end with 4 channels implemented, or separable module with a 4-channel connector
SMF Length                         : N/A
MAX Power                          : 0
Cable Rx AMP                       : N/A
Cable Rx Emphasis                  : N/A
Cable Rx Post Emphasis             : N/A
Cable Tx Equalization              : N/A
Wavelength Tolerance               : N/A
Module State                       : N/A
DataPath state [per lane]          : N/A
Rx Output Valid [per lane]         : N/A
Nominal bit rate                   : 0.000Gb/s
Rx Power Type                      : OMA
Manufacturing Date                 : 10_03_22
Active Set Host Compliance Code    : N/A
Active Set Media Compliance Code   : N/A
Error Code Response                : N/A
Module FW Fault                    : N/A
DataPath FW Fault                  : N/A
Tx Fault [per lane]                : N/A
Tx LOS [per lane]                  : N/A
Tx CDR LOL [per lane]              : N/A
Rx LOS [per lane]                  : N/A
Rx CDR LOL [per lane]              : N/A
Tx Adaptive EQ Fault [per lane]    : N/A

Physical Counters and BER Info
------------------------------
Time Since Last Clear [Min]        : 25075.2
Effective Physical Errors          : 0
Effective Physical BER             : 15E-255
Raw Physical Errors Per Lane       : 0,0,0,0
Link Down Counter                  : 0
Link Error Recovery Counter        : 0
Raw Physical BER                   : 15E-255
```

## Configuration setup

The DPU runs in **DPU mode (switchdev/embedded CPU)**: the two physical ports and all
representors sit behind the eSwitch (FDB), and traffic is forwarded between eSwitch ports
under our control (this is what lets DOCA Flow intercept and mark packets).

### PCIe functions and physical ports

| Function     | PCI            | RDMA dev | Netdev                         | Role                                            |
| ------------ | -------------- | -------- | ------------------------------ | ----------------------------------------------- |
| PF0 uplink   | `0000:03:00.0` | `mlx5_0` | `p0` (MAC `f0:fb:7f:e2:e2:76`) | Port 0 of the DPU — **DOCA Flow ECN runs here** |
| PF1 uplink   | `0000:03:00.1` | `mlx5_1` | `p1` (MAC `f0:fb:7f:e2:e2:77`) | Port 1 of the DPU — **DOCA PCC (RP) runs here** |
| PF0 host rep | —              | —        | `pf0hpf`                       | Host-side representor of PF0 (unused here)      |
| PF1 host rep | —              | —        | `pf1hpf`                       | Host-side representor of PF1 (unused here)      |

The MACs above are `bf3-ulisbon-1`'s; every other value is the same on any BlueField-3 in DPU mode.

What the tutorial requires of the two physical ports is only that **a frame leaving p1 arrives at
p0**. Some cards get that from a single DAC cable wired straight between the two ports, so everything
happens inside one DPU. Others have no such cable and reach p0 from p1 across two lab leaf switches,
which measures just as well — two switch hops tracked a private DAC to within 0.6% on the same
exercise. Either way, packets that leave p1 re-enter at p0, which is all the rest of this README
assumes.

### OVS bridges — the default forwarding path

Each PF sits in its own pre-existing OVS bridge, an ASAP²-hardware-offloaded L2 learning bridge
that provides the "default" forwarding path referenced throughout this README (this is what
`fdb_def_rule_en=1` keeps active on whichever PF isn't under a `doca-flow` program's exclusive
control):

| Bridge   | Ports                         | Covers                                                                                                                                                                                                                            |
| -------- | ----------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ovsbr1` | `p0`, `pf0hpf`, `en3f0pf0sf0` | PF0 — fully bypassed while a `doca-flow` program runs (its root pipe takes absolute hardware priority; verified via zero OVS packet/flow activity on `ovsbr1` during a live test that was actively losing packets one layer down) |
| `ovsbr2` | `p1`, `pf1hpf`, `en3f1pf1sf0` | PF1 — always active; this is the actual mechanism that gets the sender's traffic onto the wire, since no `doca-flow` program ever touches PF1                                                                                     |

`en3f0pf0sf0`/`en3f1pf1sf0` are the SFs' host-side **representors**: distinct, always-present
netdevs on the switch side (not the same object as `enp3s0f0s0`/`enp3s0f1s0` below, which are the
SFs' own consumer-side netdevs).

[`setup_roce_loopback.sh`](admin/local_scripts/setup_roce_loopback.sh) **deletes every OVS bridge on the DPU** and
recreates exactly these two. See below for why it imposes the layout rather than adapting to it.

### Scalable Functions (SFs) — the RoCE endpoints

**Three** SFs carry the RoCE traffic — one per PF for the sender/receiver pair the main exercises use,
plus a second receiver on PF0 that only the path-steering bonus needs. Each SF's netdev is moved into
its own **network namespace** so that the RoCE traffic is forced out onto the wire (p1 → p0) instead of
being delivered locally by the host kernel:

| SF             | RDMA dev | Netdev (in ns) | Representor   | Namespace | IP          | Role                                     |
| -------------- | -------- | -------------- | ------------- | --------- | ----------- | ---------------------------------------- |
| sfnum 0 on PF0 | `mlx5_2` | `enp3s0f0s0`   | `en3f0pf0sf0` | `ns0`     | `10.0.0.1`  | **Receiver / server (NP)**               |
| sfnum 0 on PF1 | `mlx5_3` | `enp3s0f1s0`   | `en3f1pf1sf0` | `ns1`     | `10.0.0.2`  | **Sender / client (RP)**                 |
| sfnum 4 on PF0 | `mlx5_4` | `enp3s0f0s4`   | `en3f0pf0sf4` | `ns0_1`   | `10.0.0.11` | Second receiver — **path steering only** |

Parts 1 and 2 only ever touch the first two; `ns0_1` sits idle unless you are running the bonus
exercise, whose ingress role delivers path 0 to `10.0.0.1` and path 1 to `10.0.0.11`. It is created
unconditionally so one setup run serves all three exercises.

Run [`setup_roce_loopback.sh`](admin/local_scripts/setup_roce_loopback.sh) to build this whole layout. **None of it
survives a reboot or power-cycle, so re-run it after every boot:**

```bash
sudo ./admin/local_scripts/setup_roce_loopback.sh
```

> **The script imposes this layout; it does not discover it.** It deletes every OVS bridge and
> every SF on both PFs, then creates exactly the three SFs above, rebuilds `ovsbr1`/`ovsbr2`,
> reserves hugepages, creates the namespaces, and asserts every line of the table before exiting
> — any deviation is a hard failure, so a run that finishes is a run you can trust. That is
> deliberate: the *sfnum* of an SF is yours to choose, but its **RDMA device index (`mlx5_N`) is
> handed out in probe order**, so the only way to reliably land on `mlx5_2`/`mlx5_3` — which
> `run_server.sh`, `run_client.sh` and every command line in this README hardcode — is to start
> from zero SFs and create ours in a fixed order. It also means the tutorial never needs a
> `--sf-num` flag, since the receiver is always sfnum 0.
>
> Because it is destructive, a card staged for something else loses that staging. For the
> `bf3-nvidia-*` cards, [`admin/local_scripts/reset_nvidia_dpu_to_original_config.sh`](admin/local_scripts/reset_nvidia_dpu_to_original_config.sh)
> puts the original layout back (see [Handing a borrowed card back](#handing-a-borrowed-card-back)).

> **Why the SF MACs are derived, not fixed.** Each SF is created with an explicit hardware address
> (`mlnx-sf -a create -m ...`), because an SF created without one can come up with
> `hw_addr 00:00:00:00:00:00` — and since the RDMA **node GUID is derived from it**, a zero there
> breaks RoCE connection setup. That was observed on PF1 of a `bf3-nvidia-*` card, whose PF0 has a MAC
> pool configured but whose PF1 does not.
>
> The address must not be a *constant*, though: on a DPU whose ports are cabled into a shared
> fabric rather than to each other, these MACs are visible to every other machine in the same
> broadcast domain, so a hardcoded value would put duplicate MACs on the VLAN as soon as the
> tutorial ran on a second DPU in the same lab. The script instead derives each one from that PF
> uplink's burned-in (globally unique) address by setting the locally-administered bit — stable
> across re-runs on one box, distinct across boxes:
>
> | | p0 → receiver SF | p1 → sender SF |
> |---|---|---|
> | `bf3-ulisbon-1` | `f0:fb:7f:e2:e2:76` → `f2:fb:7f:e2:e2:76` | `f0:fb:7f:e2:e2:77` → `f2:fb:7f:e2:e2:77` |
> | a `bf3-nvidia-*` card | `5c:25:73:e6:00:d0` → `5e:25:73:e6:00:d0` | `5c:25:73:e6:00:d1` → `5e:25:73:e6:00:d1` |

> **Why namespaces.** They stop the *Linux kernel* from delivering `10.0.0.1 ↔ 10.0.0.2` locally
> — both IPs sit on this one host, so without isolation the kernel short-circuits them and RoCE
> never touches the wire. Each SF in its own netns forces the traffic out. The packet then reaches
> the wire because **p0 and p1 are independent switchdev eSwitches** — PF1 has no vport for
> `mlx5_2`, so the frame has nowhere to go inside PF1's domain and `ovsbr2` sends it out p1.
> (There is no cross-PF eSwitch shortcut to defeat.)
>
> **Why `ovsbr2` gets two explicit flows.** Left to its own devices, `ovsbr2` has never seen the
> receiver's MAC as a *source*, so every frame the sender emits is unknown-unicast and gets
> **flooded** out every port. On a card with a private DAC that is harmless — the only place a flooded
> frame can go is across the cable to p0. On a card wired into a shared fabric it would flood the whole
> lab VLAN at 92 Gb/s, which is antisocial and may trip storm control. The script therefore pins both
> directions
> (`dl_dst=<receiver MAC> → p1`, `dl_dst=<sender MAC> → the sender SF`) so the traffic is plain
> unicast; everything else still falls through to `NORMAL` learning.
>
> **Why the real MAC, not a fake one.** An earlier version of this tutorial pinned the sender's
> neighbor to a made-up, unknown MAC, and had DOCA Flow rewrite it to the real MAC before
> delivery — reasoning that this way, PF0 traffic could *only* ever be handled by the DOCA Flow
> pipe (an unknown MAC has no entry in `ovsbr1`'s FDB, so nothing but our own root pipe could
> deliver it). That rewrite turned out to cost ~70x throughput (changing the dst MAC's value
> specifically triggers heavy packet loss on this NIC/firmware — see the `doca-flow/` programs
> below), so this tutorial now uses the real MAC directly and never rewrites it. The tradeoff:
> if no `doca-flow` program is running, PF0 traffic silently falls back to `ovsbr1` and reaches
> the receiver **unmarked** instead of erroring loudly — worth it for a ~70x speedup, but worth
> knowing if a marking exercise seems to have no effect: check something is actually running.

### Firmware NV-config (PCC prerequisite)

DOCA PCC (Part 2) needs two NV-config knobs:

- **`USER_PROGRAMMABLE_CC=1`** (default `0`) — enables the programmable-CC / PCC object. Without it
  any PCC program (ours or NVIDIA's stock `doca_pcc`) fails with
  `PCC CONFIG object is not supported on this device`.
- **`DPA_AUTHENTICATION=0`** — this is the *factory default*, but a DPU may ship hardened to `1`. With it `1`, the firmware only runs **signed** DPA images and rejects a locally `dpacc`-built one, so *both* our controller **and the stock DOCA `doca_pcc`** fail at startup with `flexio_create_prm_process ... Failed to create PRM process` (syndrome `0x8f333`). We disable it because tutorial participants recompile the DPA algo on every tweak; authenticating each build is a heavyweight, beta, static-link-only signing chain (generate an OEM root CA → install a signed cert container with `mlxdpa`/`flint` → sign the ELF), so it's impractical here — see NVIDIA's [DPA Development](https://networking-docs.nvidia.com/doca/sdk/dpa-development) guide if you do need signed images.

`REAL_TIME_CLOCK_ENABLE` is **not** needed (the RTT loop uses the free-running clock).

1. **Stage both knobs** (writes NV-config; can be done from the Arm):

   ```bash
   sudo mlxconfig -y -d /dev/mst/mt41692_pciconf0 set USER_PROGRAMMABLE_CC=1 DPA_AUTHENTICATION=0
   ```

2. **Apply it by fully power-cycling the DPU — a reboot is NOT enough.** The staged value only
   becomes live after the DPU chip actually loses power and re-reads NV-config on boot. The
   BlueField-3 has its **own power rail that survives a host OS reboot**, so `sudo reboot`, a BMC
   *power reset* (warm reset), and `chassis power cycle` (too quick to drain the rail) all leave
   `Current` unchanged — verified. On the Arm, `mlxfwreset` is also rejected outright
   (`Synchronization by driver is not supported`).

   **Do a real power-off with a delay so the DPU rail drops**, e.g. over IPMI to the host's BMC:

   ```bash
   ipmitool -H <bmc> -U <user> -P <pass> chassis power off
   sleep 30                     # let the DPU power rail actually drain
   ipmitool -H <bmc> -U <user> -P <pass> chassis power on
   ```

   (A physical full power-off — hold the power button off / pull AC for ~30 s — does the same.)

   > `mlxfwreset` from the host is the "intended" tool but was unreliable here: level 3 needs a
   > driver sync the DPU rejects; level 1 only permits Arm-side reset types that don't reload NIC
   > NV-config; and level 4 (warm reboot) blocks on a multi-function sync barrier ("waiting for
   > mlxfwreset to run on all other hosts") that needs the reset requested on **both** PFs
   > (`mt41692_pciconf0` and `mt41692_pciconf0.1`) concurrently. A clean power-cycle sidesteps all of
   > it.

3. **Verify the *Current* (live) values with `-e`** — plain `q` prints only the Next Boot column and
   will read the new value even before the power-cycle has taken effect:

   ```bash
   sudo mlxconfig -d /dev/mst/mt41692_pciconf0 -e q | grep -E 'USER_PROGRAMMABLE_CC|DPA_AUTHENTICATION'
   #                                          Default      Current      Next Boot
   #   USER_PROGRAMMABLE_CC                   False(0)     True(1)      True(1)      <- Current must be 1
   #   DPA_AUTHENTICATION                     False(0)     False(0)     False(0)     <- Current must be 0
   ```

   Until **Current** reads `USER_PROGRAMMABLE_CC=1` *and* `DPA_AUTHENTICATION=0`, the controller
   refuses to start — with `PCC CONFIG object is not supported on this device` (knob 1) or
   `Failed to create PRM process` / syndrome `0x8f333` (knob 2).

> **Gotcha: `USER_PROGRAMMABLE_CC=1` appears to disable the NIC's stock DCQCN, not just add an
> optional path alongside it.** With the Flow program marking 100% of packets CE and no PCC controller
> running, expect **no** throughput drop — traffic stays pinned at line rate (~92.6 Gb/s)
> even with `-R` left off entirely (i.e. no rdma_cm/ECE QP→algo-slot negotiation at all, so this
> isn't an artifact of ECE binding the QP to an empty slot). CNPs genuinely are flowing and being
> processed in hardware the whole time — `/sys/class/infiniband/mlx5_0/hw_counters/np_cnp_sent` and
> `mlx5_1/hw_counters/rp_cnp_handled` climb in lockstep, `rp_cnp_ignored` stays `0` — so this isn't
> "CNPs are missing." It also isn't "the stock decrease is just gentle": the classic DCQCN decrease
> factor is tunable live at `/sys/devices/.../net/p1/ecn/roce_rp/rpg_gd` (sender-side port, since
> the sender is the RP); pushing it from the default `11` (≈0.05% rate cut per CNP) down to `3`
> (≈12.5% per CNP, a ~250x more aggressive cut) made zero measurable difference. Working
> explanation: `USER_PROGRAMMABLE_CC=1` is a persistent NV-config mode, not a per-QP setting — once
> it's live, the firmware seems to route RP rate decisions through the DPA algorithm slot
> unconditionally, whether or not a program is loaded there. With no controller running that slot
> is empty, so CNPs still get parsed and counted but nothing ever writes a new rate back to the QP.
> Practically: **on a DPU configured for this tutorial, there's no way to observe plain stock
> DCQCN** — either a PCC algorithm is loaded and reacting, or nothing reacts, regardless of `-R`.
> (Not yet confirmed by the one fully decisive test — `USER_PROGRAMMABLE_CC=0` + a full
> power-cycle — since that would also temporarily break the PCC exercise.)
>
> **A data point against that explanation:** a card that still had `USER_PROGRAMMABLE_CC=0` handled
> 2.4 M CNPs — stock DCQCN nominally in charge — and throughput still did not move (92.10 → 92.07
> Gb/s). If the `=1` mode were what suppressed the stock reaction, that run should have slowed down.
> It is not conclusive either way: the default `rpg_gd=11` is a ≈0.05% cut per CNP, which a single QP
> at line rate may simply absorb. Treat the explanation above as unresolved.

## The fleet

The tutorial ran on **12 BlueField-3 cards at five sites**. The inventory is
[`admin/machines.txt`](admin/machines.txt), and it is the single source of truth for
[`admin/fleet.py`](admin/fleet.py):

| Site prefix        | Cards |
| ------------------ | ----- |
| `bf3-ulisbon-`     | 3     |
| `bf3-nvidia-`      | 4     |
| `bf3-umich-`       | 2     |
| `bf3-uwashington-` | 2     |
| `bf3-uwaterloo-`   | 1     |

Cards are addressed by their **Tailscale** names, so there is no `ssh_config` and no addresses to
track; participants reached them over the same tailnet ([`guides/tailscale.md`](guides/tailscale.md)).
`fleet.py` is transport and aggregation only — every verb ships one of
[`admin/local_scripts/`](admin/local_scripts/) to each card over stdin and renders one table of
results, so any card the tool cannot reach can still be driven by running the same script on it by
hand. Per-card output from the last runs is archived in `admin/results/` (gitignored).

The fleet was not uniform in DOCA version or in wiring, which is why the exercises exist in two
`doca-N/` trees and why `setup_roce_loopback.sh` imposes its layout rather than discovering it.
`admin/fleet.py doca`, `firmware`, `links` and `pcc-ready` report the spread.

## How much the cards differ

The fleet is not uniform, and the exercises had to survive that. No card is special or canonical —
they are all just cards, and every one of them runs the same two `doca-N` trees unmodified. Here is
what each site actually has, as reported by the last full `admin/fleet.py` sweep:

| Cards | DOCA | Firmware | Link speed † | Tree |
|---|---|---|---|---|
| `bf3-ulisbon-1..3` | `2.9.1008` | `32.43.2402` | 100G | `doca-2` |
| `bf3-nvidia-1..4` | **`2.7.0085`** | `32.41.1000` (PSID `MT_0000000884`) | 200G | `doca-2` |
| `bf3-umich-1..2` | `2.9.1008` | `32.43.2402` | 100G or 200G † | `doca-2` |
| `bf3-uwashington-1..2` | `3.1.0105` | `32.46.1006` | 100G or 200G † | `doca-3` |
| `bf3-uwaterloo-1` | **`3.4.0112`** | — | 200G | `doca-3` |

Read that table for the spread, not as a spec sheet: **DOCA 2.7 through 3.4**, three firmware versions,
and a mix of 100G and 200G links. The `doca-2`/`doca-3` split plus the `doca_flow_compat.h` shims are
what make one set of sources cover all of it.

> † **Link speeds are not reliably pinned down by this data, so do not quote them.** The two sweeps
> that record speed disagree on four of the twelve cards: `print_link_status.sh` (via `mlxlink`) reports
> 100G for both `bf3-umich-*` where `devices_check` (via sysfs `/sys/class/net/pN/speed`) reports
> `200000`; for `bf3-uwashington-*`, `mlxlink` reports an asymmetric 100G on p0 and 200G on p1, while
> sysfs reports `200000`/`200000` on `-1` and `100000`/`100000` on `-2`. The sweeps ran at different
> times, so a port may genuinely have been re-cabled or renegotiated between them. Only `bf3-ulisbon-*`
> (100G) and `bf3-nvidia-*` (200G) have both tools in agreement. Re-run `admin/fleet.py links` against
> a live card if you need the real number.

Goodput is bounded by a single QP at 1500-byte MTU, not by the link: the 200G NVIDIA cards land in
the same ~92 Gb/s band as the 100G ones. `bf3-uwaterloo-1` is the exception at ~196 Gb/s — worth
knowing if you compare numbers across sites, and worth remembering that the PCC exercise's "rate
collapses to ~0.08 Gb/s" result holds regardless.

Regenerate any of this with `admin/fleet.py doca`, `firmware`, `links` and `pcc-ready`.

> Where a value above is missing, or where a `fleet.py test-tutorial` run did not come back clean,
> it was a **per-run condition, not a property of the card**:
>
> - **`bf3-uwaterloo-1` firmware** — not captured; that sweep hit `ERROR: passwordless sudo is
>   required (logged in as s26t)`. The card itself is fine, and passes the exercise.
> - **`bf3-umich-2`** — the only card with no completed run at all, and **not because anything is
>   wrong with it**. Its `test-tutorial` run got through every on-card step — SFs deleted and recreated
>   with derived MACs, `mlx5_2`/`mlx5_3` and their representors up, `ovsbr1`/`ovsbr2` built, hugepages
>   reserved, all three namespaces configured, sender neighbour pinned — and then failed the *one*
>   step that depends on the wire: `2 packets transmitted, 0 received, 100% packet loss` pinging
>   `10.0.0.1` from `ns1`. `@@build=fail` / `@@flow=fail` are cascade effects of that single failure.
>   The card's loopback demonstrably does work: a later `cleanup-cards` sweep caught it running
>   `benchmark.sh` with four live `ib_write_bw` processes across `ns0`/`ns1`, which is impossible
>   unless p1 → p0 is carrying traffic. So this was a transient — most plausibly contention, since
>   `setup_roce_loopback.sh` is destructive and asserts every line it sets up, and will fight a card
>   that is already mid-exercise.
> - **`bf3-umich-1`** — Part 1 passed completely here (7483/7483 captured IPv4 packets CE-marked);
>   only the PCC step was **skipped**, because the test's inline `pcc-ready` check found the firmware
>   knobs not yet live. A later `fleet.py pcc-ready` sweep reports `@@upcc=1 @@dpa=0 → ready` on this
>   card, so the knobs were staged after that test ran.
> - **`bf3-uwashington-1`** — skipped as `busy — in use by another workload (1 process(es)) — not
>   touched`, deliberately, so it was never exercised in that sweep; its identically configured sibling
>   `bf3-uwashington-2` passed.

**None of that needs any manual build setup.** `meson setup build doca-2` (or `doca-3`) followed by
`ninja` works as-is on every card in the fleet, DOCA 2.7 included — the `doca-N` split plus the
`doca_flow_compat.h` shims absorb the SDK differences, and there is no `PKG_CONFIG_PATH` or other
environment fiddling to do by hand. `admin/fleet.py test-tutorial` is what establishes this: it does a
clean from-scratch build and then runs the whole exercise end to end — loopback, flow program, RoCE
traffic, ECN marking verified in a pcap, PCC controller, rate collapse — and reports pass/fail per
card. It passes on the fleet.

### Where the divergence actually bites

One group of cards arrives in an *as-shipped* state that differs enough to matter, and it is why
[`setup_roce_loopback.sh`](admin/local_scripts/setup_roce_loopback.sh) wipes and rebuilds rather than
adapting. The NVIDIA cards ship with **both SFs on PF0**:

| | Typical (`bf3-ulisbon-*`) | NVIDIA cards |
|---|---|---|
| SF layout as shipped | one per PF | **both on PF0** |
| Receiver SF | `pci/0000:03:00.0/229408` → `en3f0pf0sf0` / `enp3s0f0s0` / `mlx5_2` | `pci/0000:03:00.0/229408` → `en3f0pf0sf2` / `enp3s0f0s2` / `mlx5_2` |
| Sender SF | `pci/0000:03:00.1/294944` → `en3f1pf1sf0` / `enp3s0f1s0` / `mlx5_3` | `pci/0000:03:00.0/229409` → `en3f0pf0sf3` / `enp3s0f0s3` / `mlx5_3` |
| OVS bridges | `ovsbr1` (`p0`,`pf0hpf`,`en3f0pf0sf0`), `ovsbr2` (`p1`,`pf1hpf`,`en3f1pf1sf0`) | `host_br` (`pf0hpf`,`en3f0pf0sf2`), `wire_br` (`p0`,`en3f0pf0sf3`); **`p1` and `pf1hpf` in no bridge** |
| `p0` ↔ `p1` | direct DAC (FS `Q28-PC03`, 3 m) | **no DAC** — each port goes to a *different* leaf switch |

Both SFs on PF0 can never work here, because the sender has to sit on PF1 for its traffic to leave on
p1 and re-enter at p0 where DOCA Flow marks it.

After the script runs, **every card is identical** — `mlx5_2`/`mlx5_3` (plus `mlx5_4` for the bonus),
`ovsbr1`/`ovsbr2` — and every command in this README works unchanged on any of them.

Nothing about the SF layout is fixed in hardware: SFs are created and destroyed at run time
(`mlnx-sf`, a wrapper over `devlink port add/del`), their sfnum is chosen by us, and
`PER_PF_NUM_SF=False(0)` everywhere means the SF pool is device-wide, so PF1 can host one with
no NV-config change or power-cycle. None of it survives a reboot.

The NV-config knobs that matter (`mlxconfig -e q`, *Current* column) ended up the same everywhere:
`USER_PROGRAMMABLE_CC=True(1)`, `DPA_AUTHENTICATION=False(0)`, and
`PER_PF_NUM_SF=False(0)` / `PF_TOTAL_SF=0` / `NUM_OF_VFS=16`. Getting there took a staging step on
some cards — the NVIDIA ones shipped with `USER_PROGRAMMABLE_CC=False(0)`, which needs a real
power-cycle to change — but by the tutorial `admin/fleet.py pcc-ready` reported
`@@upcc=1 @@dpa=0 → ready` on **all twelve**.

All cards run DPU mode with **both PFs in `switchdev`** (`devlink dev eswitch show pci/0000:03:00.{0,1}`
→ `mode switchdev inline-mode none encap-mode basic`), so the eSwitch assumptions in
[Configuration setup](#configuration-setup) hold throughout.

### When the two ports are not cabled to each other

All the tutorial needs of the two physical ports is that **a frame leaving p1 arrives at p0**. On the
`bf3-ulisbon-*` cards that comes from a direct DAC, so "the wire" is a closed 3 m loop. The
`bf3-nvidia-*` cards have no such cable: their ports go into a production-style fabric, and **not even
to the same switch**. Passive LLDP capture (`tcpdump -i pN -e 'ether proto 0x88cc'`) shows a different
neighbour on each port:

```
p0:  b0:cf:0e:2b:fd:68  l28-compleaf-1.pod23.m3.pdx01.us.nvidia.com
p1:  b0:cf:0e:2b:dd:68  l28-compleaf-2.pod23.m3.pdx01.us.nvidia.com
```

The two leaves nevertheless share one broadcast domain — both leaf routers appear as neighbours on
`p0`, on `p1` *and* on `oob_net0`:

```
$ ip -6 neigh show | grep b0:cf
fe80::b2cf:eff:fe2b:fd50 dev oob_net0 lladdr b0:cf:0e:2b:fd:50 router STALE
fe80::b2cf:eff:fe2b:dd50 dev oob_net0 lladdr b0:cf:0e:2b:dd:50 router STALE
fe80::b2cf:eff:fe2b:fd50 dev p1       lladdr b0:cf:0e:2b:fd:50 router STALE
fe80::b2cf:eff:fe2b:dd50 dev p1       lladdr b0:cf:0e:2b:dd:50 router STALE
fe80::b2cf:eff:fe2b:fd50 dev wire_br  lladdr b0:cf:0e:2b:fd:50 router STALE
fe80::b2cf:eff:fe2b:dd50 dev wire_br  lladdr b0:cf:0e:2b:dd:50 router STALE
```

and **p1 → fabric → p0 is confirmed reachable**. Probe: a macvlan on `p1` in its own netns,
against an address on `wire_br` (the OVS internal port of the bridge that holds `p0`):

```
$ sudo ip netns exec l2probe ping -c 4 10.77.77.1
64 bytes from 10.77.77.1: icmp_seq=1 ttl=64 time=47.5 ms      <- ARP resolution
64 bytes from 10.77.77.1: icmp_seq=2 ttl=64 time=0.718 ms
64 bytes from 10.77.77.1: icmp_seq=3 ttl=64 time=0.184 ms
64 bytes from 10.77.77.1: icmp_seq=4 ttl=64 time=0.157 ms
4 packets transmitted, 4 received, 0% packet loss
```

So the p1 → p0 hop the tutorial depends on **exists either way**; across the fabric it is just two
switch hops over a shared lab VLAN instead of a private cable. Three consequences follow, and they are
why the setup script looks the way it does:

- **The loopback must not rely on flooding.** `ovsbr2` would otherwise flood the receiver's MAC as
  unknown-unicast out `p1`, which is harmless on a private DAC but on a shared fabric sprays every
  frame across the lab VLAN at 92 Gb/s. `setup_roce_loopback.sh` pins both directions with explicit
  OpenFlow rules so the fabric unicasts p1 → leaf-2 → leaf-1 → p0; see
  [Why `ovsbr2` gets two explicit flows](#scalable-functions-sfs--the-roce-endpoints).
- **SF MACs must be unique per DPU**, since they are visible to the whole VLAN — hence deriving
  them from each PF uplink's burned-in address rather than hardcoding.
- **The uplink netdevs cannot be moved into a network namespace.** `ip link set p1 netns ...` fails
  with `RTNETLINK answers: Invalid argument` on the mlx5 switchdev uplink; use a macvlan (as above)
  if you need to probe a port from an isolated namespace.

### Handing a borrowed card back

`setup_roce_loopback.sh` destroys the staging the `bf3-nvidia-*` cards ship with. To put it back:

```bash
sudo ./admin/local_scripts/reset_nvidia_dpu_to_original_config.sh
```

It recreates NVIDIA's two PF0 SFs with their original sfnums (2 and 3) and hardware addresses
(`44:38:39:00:00:02` / `:03`, trust on), rebuilds `host_br` and `wire_br`, leaves PF1 empty, and
verifies each of those before exiting non-zero if anything is off. A **reboot also restores the box
unconditionally** — SFs, bridges and namespaces are all runtime state — so the script only exists
to save a reboot.

# Testing the setup

Confirming that the two ports really do reach each other at line rate. Run this on the card; the
`--eth-peer` MACs below are `bf3-ulisbon-1`'s, so substitute the p0/p1 MACs of the card you are on:

```
$ sudo /opt/mellanox/dpdk/bin/dpdk-testpmd \
  -l 1-15 -n 4 \
  -a 0000:03:00.0 -a 0000:03:00.1 \
  -- \
  --forward-mode=txonly --txonly-multi-flow \
  --eth-peer=0,f0:fb:7f:e2:e2:77 \
  --eth-peer=1,f0:fb:7f:e2:e2:76 \
  --rxq=8 --txq=8 --nb-cores=14 \
  --txd=4096 --rxd=4096 --burst=64 \
  --total-num-mbufs=524288 \
  --stats-period=1 -i
testpmd> set promisc all on
testpmd> set txpkts 1518
testpmd> start
testpmd> show port stats all
```

# Requirements and assumptions

The build and run steps below assume:

- **DOCA is installed under `/opt/mellanox/doca`.** The BSP install provides the runtime libraries
  and the DPA build toolchain (`dpacc` at `/opt/mellanox/doca/tools/dpacc`). Which release the card
  runs decides which tree you build: `doca-2/` for DOCA 2.x, `doca-3/` for 3.x. Check with
  `admin/local_scripts/print_doca_version.sh`, or `admin/fleet.py doca` across the fleet.
- **`meson` and `ninja` are on `PATH`** (system packages). Together with `dpacc` they are the only
  tools needed to build **every** exercise **natively on the Arm cores**, which is how the whole
  tutorial was built and run. Nothing is copied out of `/opt/mellanox/doca/applications` and nothing is
  patched at build time: all of the sources are in this repository.
- **`libpcap-dev`** is needed by the one program that writes captures (`doca_flow_ecn_pcap`);
  `libbsd-dev` is picked up if present. `admin/local_scripts/install_deps.sh` installs the full set
  (`admin/fleet.py deps --install` does it fleet-wide).
- **`perftest` (`ib_write_bw`) and `mlxconfig`/`mlxfwreset` (MFT)** are installed for driving RoCE
  traffic and for the firmware NV-config step above.
- Hugepages are reserved (DPDK/DPA programs — the `doca-flow` programs and the PCC controller — need
  them). This is done for you by [`setup_roce_loopback.sh`](admin/local_scripts/setup_roce_loopback.sh)
  (`dpdk-hugepages.py --reserve 4G`); it does not persist across reboots, so re-run
  `setup_roce_loopback.sh` after every boot/power-cycle.

# Building

Each `doca-N/` directory is a self-contained meson project that builds **every** exercise for that
DOCA release — the Flow programs and the PCC controller together. Point meson at the one matching the
card:

```bash
$ meson setup build doca-3      # or: doca-2 — whichever release the card runs
$ ninja -C build
```

(Equivalently `cd doca-3 && meson setup build && ninja -C build`. There is no meson project at the
repository root.) The path-steering bonus exercise is a *separate* project nested inside — see
[Path steering](#path-steering-bonus) below.

## The DOCA Flow programs

`doca-N/doca-flow/` builds four binaries into `build/doca-flow/`. They share the same
PORT_DEMUX/eSwitch-forwarding scaffold and differ in what they do to a packet before delivering it to
the receiver's SF:

| Binary                   | What it is                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| ------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **`doca_flow_solution`** | The **answer key**: CE-marks a configurable fraction of IPv4 packets from the wire, with hardware counters. `--percent N`, `[0,100]` — `100` marks everything and `0` marks nothing (both exact); anything between samples on a hardware random field, rounded to the nearest power-of-two fraction. Default `100`.                                                                                                                             |
| **`doca_flow_template`** | The **participant exercise**: `doca_flow_solution` with three pipe-building functions hollowed out to `TODO`s. Same flags. **Generated** from the solution — see below.                                                                                                                                                                                                                                                                         |
| **`doca_flow_ecn_pcap`** | The solution *plus* a hardware copy of the traffic to a `.pcap`, so you can see the CE bit on the wire. `--pcap <file>` (optional — omit for pure marking), `--percent N`, `--sample N`. Writing starts paused and toggles with **SPACE**, or `kill -USR1 <pid>` when there is no tty (piped, `nohup`, script-driven — the app prints its own pid at startup). Not part of the exercise; hand-maintained; the only target that links `libpcap`. |
| **`doca_flow_nop`**      | A standalone minimal forwarder, no header-modify action at all — the performance control group. Older than the rest and not used by the exercise, which carries its own no-op root pipe. Takes `--sf-num`.                                                                                                                                                                                                                                      |

Per-program detail is in `doca-N/doca-flow/doca_flow_ecn_pcap.README.md` and
`doca_flow_template.README.md`; the exercise itself is written up in
[`guides/doca-flow.md`](guides/doca-flow.md).

> **`doca_flow_template.c` is generated — do not hand-edit it.**
> [`admin/local_scripts/regen_templates.py`](admin/local_scripts/regen_templates.py) derives it from
> `doca_flow_solution.c`, so `diff doca_flow_template.c doca_flow_solution.c` is exactly the three
> functions a participant writes. Change the solution, then re-run the script; `--check` verifies the
> two are in step.

Running (only one DOCA Flow / DPDK primary process can own PF0 at a time, so pick one):

```bash
$ sudo ./build/doca-flow/doca_flow_solution  -- --percent 50
$ sudo ./build/doca-flow/doca_flow_template  -- --percent 50
$ sudo ./build/doca-flow/doca_flow_ecn_pcap  -- --pcap /tmp/capture.pcap --percent 50 --sample 8
$ sudo ./build/doca-flow/doca_flow_nop
```

**`-- ` before the app flags is required**: every one of these registers its EAL bring-up with
`doca_argp_set_dpdk_program()`, and DOCA argp uses `--` to separate the DPDK EAL args from the app
args.

> **Naming, if you are cross-reading the guides.** `update_participants_repo_on_github.py` ships
> `doca_flow_template.c` renamed to **`doca_flow_ecn.c`**, so
> [`guides/doca-flow.md`](guides/doca-flow.md) says `doca_flow_ecn` throughout. "Template" is our word
> for it — an artefact of how the exercise is derived here — and means nothing to a participant, who
> simply has one program with three functions to write.

# How Parts 1 and 2 fit together

The two parts are one closed loop. Participants first built a DOCA Flow program that sets the ECN
bits on packets, then a DOCA PCC controller that reacts to the CNPs those marks provoke. Both run on
the Arm cores of a single card, with a server and a client driving RoCE traffic across the two-port
loopback, so the effect of the marking is visible as a throughput change.

- Server: [run_server.sh](run_server.sh)
- Client: [run_client.sh](run_client.sh)
- Both, with a live throughput chart: [benchmark.sh](benchmark.sh) (needs `ttyplot` —
  [`admin/local_scripts/setup_ttyplot.sh`](admin/local_scripts/setup_ttyplot.sh))

## End-to-end data path (both parts together)

![End-to-end data path: sender (ns1/mlx5_3) sends a RoCE WRITE out p1, across the 100G DAC cable to p0, through PF0's eSwitch where DOCA Flow ECN marks CE and delivers to mlx5_2; the receiver emits a CNP that returns the same way, and the RP PCC controller on mlx5_1 reacts by cutting the sender QP's rate.](docs/end-to-end-data-path.png)

(source: [`docs/end-to-end-data-path.dot`](docs/end-to-end-data-path.dot). Two companion figures show
the same path *before* PCC is loaded: [`docs/end-to-end-data-path-pre-pcc.dot`](docs/end-to-end-data-path-pre-pcc.dot)
— `USER_PROGRAMMABLE_CC=1` with an empty DPA algo slot, so CNPs arrive, are handled, and nothing
reacts — and [`docs/end-to-end-data-path-pre-pcc-default-cc.dot`](docs/end-to-end-data-path-pre-pcc-default-cc.dot)
— `USER_PROGRAMMABLE_CC=0`, where the NIC's stock DCQCN reaction point cuts the rate instead.
Regenerate after editing any `.dot` with `make -C docs` — it renders each `.dot` to a same-named
`.png` at 300 DPI, sharp enough to drop straight into slides. Use `make -C docs DPI=96` for quick
screen-sized renders, or `make -C docs -B` to force a full re-render.)

The `PF0 domain` node above is a whole graph of flow tables collapsed to one box. Zoomed in:

![PF0's eSwitch table graph: PORT_DEMUX is the root table and matches on source vport; wire ingress (vport 0) is steered to ECN_MARK, RANDOM_SAMPLE, or INGRESS_PASSTHROUGH depending on --percent, while SF egress (vport 1) is output straight to the p0 wire and anything else is dropped.](docs/pf0-eswitch-pipes.png)

(source: [`docs/pf0-eswitch-pipes.dot`](docs/pf0-eswitch-pipes.dot)) — a DOCA Flow **pipe is a flow
table**, not a pipeline stage. The eSwitch is a multi-table match-action switch: every packet, from
the wire or from an SF, starts at the root table (`PORT_DEMUX`), and each lookup either jumps to
another table, outputs to a vport, or drops. Which table wire ingress lands in is chosen at startup
by `--percent`, so `RANDOM_SAMPLE` only exists for `0 < percent < 100` and `ECN_MARK` isn't built at
all at `percent 0`. Each PF owns its own domain with its own root table; only PF0's is programmed
here — `fdb_def_rule_en=1` leaves the kernel's default FDB rules in place for PF1.

- **The Flow program on PF0 (`mlx5_0`)** replaces the physical switch's WRED/ECN marking that the
  original 2×BlueField-3 PCC testbed relied on. At `--percent 100` it marks **CE on every IPv4 packet**
  arriving from the wire (unconditional, not ECT→CE — so any RoCE generator drives the loop).
- **The receiver (`mlx5_2`) generates CNPs** in hardware when it sees CE-marked packets — standard
  RoCE behavior, no PCC instance needed on the NP side.
- **`doca-pcc-ecn` (RP) on PF1 (`mlx5_1`)** is the reaction point: each CNP triggers a
  multiplicative rate decrease on the sender's QP. See
  [`doca-2/doca-pcc-ecn/README.md`](doca-2/doca-pcc-ecn/README.md) for how it works.

## The PCC controller (Part 2)

`doca-N/doca-pcc-ecn/` is a **standalone project in this repository** — real sources under `device/`
and `host/`, built by the same meson project as the Flow programs. There is **no copy-and-patch of
`/opt/mellanox/doca/applications` any more**: nothing is vendored at build time and there is no patch
to apply. `meson.build` calls `build_device_code.sh`, which invokes `dpacc` to compile `device/` into
the DPA image; the host loader in `host/pcc_ecn_rp.c` is a ~250-line replacement for NVIDIA's
multi-mode sample app, exposing just `-d`/`--device` and `-l`/`--log-level`.

So the build is just the build from [above](#building):

```bash
$ meson setup build doca-3 && ninja -C build
# => build/doca-pcc-ecn/doca_pcc_ecn_rp           the finished controller (answer key)
# => build/doca-pcc-ecn/doca_pcc_ecn_rp_template  the same with the two reactions hollowed out
```

The `_template` variant is the participant exercise: identical to `doca_pcc_ecn_rp` except that its
DPA algorithm is `device/algo/rtt_template_exercise.c`, a copy of `rtt_template.c` with the CNP
multiplicative decrease (TODO 1) and the TX additive increase (TODO 2) removed. It builds and runs as
shipped, but never moves the rate until the TODOs are filled — which is exactly what the guide's first
stage exploits. It is generated by
[`admin/make_pcc_exercise.py`](admin/make_pcc_exercise.py) (`--check` verifies it is in step with the
controller); `update_participants_repo_on_github.py` ships it renamed to `rtt_template.c`.

Requires the firmware knob (`USER_PROGRAMMABLE_CC=1`, *Current* value) to be live, or the controller
refuses to start — see [Firmware NV-config](#firmware-nv-config-pcc-prerequisite).

The **device mapping is converged to our single-DPU setup** as follows:

| Step          | Original 2×BF3 testbed         | **Our single-DPU loopback**              |
| ------------- | ------------------------------ | ---------------------------------------- |
| ECN marking   | SONiC switch WRED on DSCP26→Q3 | the Flow program on PF0 (`mlx5_0`)       |
| RP PCC device | `mlx5_1` on the sender host    | `mlx5_1` (PF1 uplink — sender is p1/ns1) |
| NP PCC device | `mlx5_1` on the receiver host  | none (receiver HW CNP is enough)         |
| Sender RoCE   | `mlx5_3` on sender host        | `mlx5_3` in `ns1` (client)               |
| Receiver RoCE | `mlx5_3` on receiver host      | `mlx5_2` in `ns0` (server)               |

Combined run (start Flow first, then the RP controller, then drive traffic):

```bash
# 1. ECN marker (PF0) — leave running:
sudo ./build/doca-flow/doca_flow_solution -- --percent 100

# 2. RP PCC controller on PF1 — stays Active for the whole window:
sudo timeout 40 stdbuf -oL ./build/doca-pcc-ecn/doca_pcc_ecn_rp -d mlx5_1 -l 50 > rp.log 2>&1 &

# 3. Drive RoCE traffic. -R (rdma_cm) is MANDATORY: the QP→algo-slot-0 binding is negotiated
#    via RoCE ECE, which perftest only performs with -R. Without it the custom controller never
#    fires — and, per the gotcha above, no rate reaction happens at all (the "default algo"
#    fallback is not observed to actually reduce rate on this NV-config).
sudo ip netns exec ns0 ib_write_bw -d mlx5_2 -R -x 1 -F --report_gbits --run_infinitely -D 1            # server
sudo ip netns exec ns1 ib_write_bw -d mlx5_3 -R -x 1 -F 10.0.0.1 --report_gbits --run_infinitely -D 1   # client

grep PURE_ECN rp.log                  # rate walking down as CNPs arrive => the loop is live
sudo pkill -INT -x doca_pcc_ecn_rp    # stop gracefully (never SIGKILL: leaves a ghost DPA context)
```

> **If no CNPs arrive** (`grep PURE_ECN rp.log` stays empty while the Flow program reports rising CE
> counts): the receiver's HW **CNP generation** may be priority-scoped. Add a traffic class to steer
> traffic onto an ECN-enabled priority — e.g. `--tclass=104` (DSCP 26 → TC3) on both `ib_write_bw`
> ends — even though the Flow program already marks CE regardless of queue.

Design and bring-up notes live next to the sources:
[`doca-2/doca-pcc-ecn/README.md`](doca-2/doca-pcc-ecn/README.md) (what is in the directory and how it
is tuned), [`doca_pcc_guide.md`](doca-2/doca-pcc-ecn/doca_pcc_guide.md) (host↔DPA split, the event
loop, how a rate reaches the NIC) and
[`doca_pcc_findings.md`](doca-2/doca-pcc-ecn/doca_pcc_findings.md) (the operational gotchas).

## Path steering (bonus)

[`doca-3/pcc-path-steering/`](doca-3/pcc-path-steering/) is a **separate meson project** nested inside
`doca-3/` — it is NVIDIA's `applications` tree with our steering module added, so it is configured on
its own rather than by `doca-N/meson.build`:

```bash
$ cd doca-3/pcc-path-steering
$ meson setup build && ninja -C build
# => build/doca_pcc          PCC controller with embedded egress steering (-r <sender SF rep>)
# => build/doca_flow_steer   the standalone ingress/egress steering datapath
```

It feeds per-QP PCC rate reports into a live DOCA Flow hash pipe to split traffic across two virtual
paths marked by an ICRC-exempt DSCP bit. `doca-2/pcc-path-steering` is a symlink to this same tree;
its DOCA 2.9 and 3.x differences are handled internally by `doca_flow_compat.h` and
`pcc_doca_compat.h`. Details in [`doca-3/pcc-path-steering/README.md`](doca-3/pcc-path-steering/README.md)
and [`steering/README.md`](doca-3/pcc-path-steering/steering/README.md); the walkthrough is
[`guides/pcc-path-steering.md`](guides/pcc-path-steering.md).
