This repository contains the source code and materials for the tutorial "Programming SmartNICs: From Packet Processing to Programmable Transport" to happen at SIGCOMM 2026.

Here is the brief overview of the tutorial, as shown in the SIGCOMM 2026 [website](https://conferences.sigcomm.org/sigcomm/2026/tutorials/smartnic/):

# Overview

## Summary
Programmable switches transformed networking research by making the data plane accessible and programmable. A similar shift is now happening at the network edge: SmartNICs, DPUs, and IPUs are evolving into programmable computing platforms capable not only of packet processing, but also of stateful services and transport-layer functionality. Rather than being fixed-function offload devices, they are becoming heterogeneous subsystems tightly integrated with host software stacks.


This tutorial provides a unified, systems-oriented introduction to SmartNIC programmability, spanning four tightly coupled dimensions: data-plane packet processing, stateful network function design, transport-layer programmability, and host-level integration. It combines conceptual foundations with guided hands-on exercises on NVIDIA BlueField platforms using NVIDIA Launchpad, allowing participants to gain both architectural understanding and practical experience with packet-processing and transport programmability.


By the end of the tutorial, attendees will understand the design space across SmartNIC, DPU, and IPU platforms; write and deploy packet-processing logic on NIC targets; design and evaluate stateful services; experiment with transport-layer customization and programmable congestion control; and integrate NIC-based functionality with host control planes and software stacks.


## Motivation
Over the past decade, P4 and programmable switches opened the door to line-rate packet processing research. Today, SmartNICs extend that opportunity beyond switches and into end hosts, where networking, systems, and transport concerns intersect. Modern NIC platforms increasingly support programmable high-performance pipelines, embedded CPUs, accelerators, and tighter coordination with host software, enabling new designs for offload, isolation, efficiency, congestion control, and AI-aware networking.


Despite strong research and industrial momentum, the community still lacks a structured, hands-on tutorial that systematically teaches how to program SmartNIC packet processing, build stateful network functions, experiment with transport functionality on NICs, and integrate programmable NIC logic into end-host systems. This tutorial is designed to fill that gap.


## Outline
The tutorial follows a progressive structure from foundations to advanced functionality.


Part I — Architectures and Packet Processing Foundations
We begin by introducing the architectural landscape of SmartNICs, DPUs, and IPUs, including their heterogeneous execution models and their relationship to host software stacks. We then discuss packet-processing programming models, including match-action pipelines, stateful logic, and control-plane coordination.


Part II — Hands-On Stateful Services
Participants then move to hands-on exercises on NVIDIA BlueField-3 platforms using NVIDIA Launchpad. They develop packet-processing functionality, including forwarding, filtering, table lookups, and packet transformations, in the ConnectX packet processing pipeline using DPL (DOCA Pipeline Language) and DOCA Flow, reinforcing the concepts introduced earlier.


Part III — Transport Programming on SmartNICs
The tutorial then expands from packet processing into transport-layer customization. This session covers programmable congestion control, custom transport logic, NIC-based transport abstractions, and the broader research challenges that arise when transport functionality moves onto programmable NICs.


Part IV — Hands-On Transport Experimentation and Host Integration
Finally, participants explore transport programmability experimentally. Through hands-on exercises using DOCA PCC (Programmable Congestion Control) and DPA (Data Path Accelerator) programming, they customize transport-layer functionality, evaluate performance trade-offs, and examine host-NIC coordination. The goal is to present SmartNICs not as isolated packet devices, but as heterogeneous, programmable systems spanning packet processing, transport functionality, slow-path execution, and host-level integration.

# Physical setup

Here are the outputs of some commands to show the setup of the environment for the tutorial. The commands are run on the ARM cores of a BlueField-3 DPU. The Bluefield DPU has a single 100G link connecting both ports to each other.

```
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

| Function | PCI | RDMA dev | Netdev | Role |
|---|---|---|---|---|
| PF0 uplink | `0000:03:00.0` | `mlx5_0` | `p0` (MAC `f0:fb:7f:e2:e2:76`) | Port 0 of the DPU — **DOCA Flow ECN runs here** |
| PF1 uplink | `0000:03:00.1` | `mlx5_1` | `p1` (MAC `f0:fb:7f:e2:e2:77`) | Port 1 of the DPU — **DOCA PCC (RP) runs here** |
| PF0 host rep | — | — | `pf0hpf` | Host-side representor of PF0 (unused here) |
| PF1 host rep | — | — | `pf1hpf` | Host-side representor of PF1 (unused here) |

The MACs above are testbed A's; every other value is the same on any BlueField-3 in DPU mode.

What the tutorial requires of the two physical ports is only that **a frame leaving p1 arrives at
p0**. Testbed A gets that from a single 100G DAC cable wired straight between them, so everything
happens inside one DPU. Testbed B has no such cable and instead reaches p0 from p1 across two lab
leaf switches — measurably just as good (see [Reference testbeds](#reference-testbeds)). Either
way, packets that leave p1 re-enter at p0, which is all the rest of this README assumes.

### OVS bridges — the default forwarding path

Each PF sits in its own pre-existing OVS bridge, an ASAP²-hardware-offloaded L2 learning bridge
that provides the "default" forwarding path referenced throughout this README (this is what
`fdb_def_rule_en=1` keeps active on whichever PF isn't under a `doca-flow` program's exclusive
control):

| Bridge | Ports | Covers |
|---|---|---|
| `ovsbr1` | `p0`, `pf0hpf`, `en3f0pf0sf0` | PF0 — fully bypassed while a `doca-flow` program runs (its root pipe takes absolute hardware priority; verified via zero OVS packet/flow activity on `ovsbr1` during a live test that was actively losing packets one layer down) |
| `ovsbr2` | `p1`, `pf1hpf`, `en3f1pf1sf0` | PF1 — always active; this is the actual mechanism that gets the sender's traffic onto the wire, since no `doca-flow` program ever touches PF1 |

`en3f0pf0sf0`/`en3f1pf1sf0` are the SFs' host-side **representors**: distinct, always-present
netdevs on the switch side (not the same object as `enp3s0f0s0`/`enp3s0f1s0` below, which are the
SFs' own consumer-side netdevs).

[`setup_roce_loopback.sh`](setup_roce_loopback.sh) **deletes every OVS bridge on the DPU** and
recreates exactly these two. See below for why it imposes the layout rather than adapting to it.

### Scalable Functions (SFs) — the RoCE endpoints

Two SFs, one per PF, carry the actual RoCE traffic. Each SF's netdev is moved into its own
**network namespace** so that the RoCE traffic is forced out onto the wire (p1 → p0) instead of
being delivered locally by the host kernel:

| SF | RDMA dev | Netdev (in ns) | Representor | Namespace | IP | Role |
|---|---|---|---|---|---|---|
| sfnum 0 on PF0 | `mlx5_2` | `enp3s0f0s0` | `en3f0pf0sf0` | `ns0` | `10.0.0.1` | **Receiver / server (NP)** |
| sfnum 0 on PF1 | `mlx5_3` | `enp3s0f1s0` | `en3f1pf1sf0` | `ns1` | `10.0.0.2` | **Sender / client (RP)** |

Run [`setup_roce_loopback.sh`](setup_roce_loopback.sh) to build this whole layout. **None of it
survives a reboot or power-cycle, so re-run it after every boot:**

```bash
sudo ./setup_roce_loopback.sh
```

> **The script imposes this layout; it does not discover it.** It deletes every OVS bridge and
> every SF on both PFs, then creates exactly the two SFs above, rebuilds `ovsbr1`/`ovsbr2`,
> reserves hugepages, creates the namespaces, and asserts every line of the table before exiting
> — any deviation is a hard failure, so a run that finishes is a run you can trust. That is
> deliberate: the *sfnum* of an SF is yours to choose, but its **RDMA device index (`mlx5_N`) is
> handed out in probe order**, so the only way to reliably land on `mlx5_2`/`mlx5_3` — which
> `run_server.sh`, `run_client.sh` and every command line in this README hardcode — is to start
> from zero SFs and create ours in a fixed order. It also means the tutorial never needs a
> `--sf-num` flag, since the receiver is always sfnum 0.
>
> Because it is destructive, a DPU staged for something else loses that staging. For the NVIDIA lab
> DPU, [`reset_nvidia_dpu_to_original_config.sh`](reset_nvidia_dpu_to_original_config.sh) puts its
> original layout back (see [Reference testbeds](#reference-testbeds)).

> **Why the SF MACs are derived, not fixed.** Each SF is created with an explicit hardware address
> (`mlnx-sf -a create -m ...`), because an SF created without one can come up with
> `hw_addr 00:00:00:00:00:00` — and since the RDMA **node GUID is derived from it**, a zero there
> breaks RoCE connection setup. That was observed on PF1 of the NVIDIA lab DPU, whose PF0 has a MAC
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
> | Testbed A | `f0:fb:7f:e2:e2:76` → `f2:fb:7f:e2:e2:76` | `f0:fb:7f:e2:e2:77` → `f2:fb:7f:e2:e2:77` |
> | Testbed B | `5c:25:73:e6:00:d0` → `5e:25:73:e6:00:d0` | `5c:25:73:e6:00:d1` → `5e:25:73:e6:00:d1` |

> **Why namespaces.** They stop the *Linux kernel* from delivering `10.0.0.1 ↔ 10.0.0.2` locally
> — both IPs sit on this one host, so without isolation the kernel short-circuits them and RoCE
> never touches the wire. Each SF in its own netns forces the traffic out. The packet then reaches
> the wire because **p0 and p1 are independent switchdev eSwitches** — PF1 has no vport for
> `mlx5_2`, so the frame has nowhere to go inside PF1's domain and `ovsbr2` sends it out p1.
> (There is no cross-PF eSwitch shortcut to defeat.)
>
> **Why `ovsbr2` gets two explicit flows.** Left to its own devices, `ovsbr2` has never seen the
> receiver's MAC as a *source*, so every frame the sender emits is unknown-unicast and gets
> **flooded** out every port. On testbed A that is harmless — the only place a flooded frame can go
> is across the DAC to p0. On testbed B it would flood pod23's entire VLAN at 92 Gb/s, which is
> antisocial and may trip storm control. The script therefore pins both directions
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

DOCA PCC (Part IV) needs two NV-config knobs:

- **`USER_PROGRAMMABLE_CC=1`** (default `0`) — enables the programmable-CC / PCC object. Without it
  `doca_pcc` fails with `PCC CONFIG object is not supported on this device`.
- **`DPA_AUTHENTICATION=0`** — this is the *factory default*, but a DPU may ship hardened to `1`. With
  it `1`, the firmware only runs **signed** DPA images and rejects a locally `dpacc`-built one, so
  *both* our controller **and the stock DOCA `doca_pcc`** fail at startup with
  `flexio_create_prm_process ... Failed to create PRM process` (syndrome `0x8f333`). We disable it
  because tutorial participants recompile the DPA algo on every tweak; authenticating each build is a
  heavyweight, beta, static-link-only signing chain (generate an OEM root CA → install a signed cert
  container with `mlxdpa`/`flint` → sign the ELF), so it's impractical here — see NVIDIA's
  [DPA Development](https://networking-docs.nvidia.com/doca/sdk/dpa-development) guide if you do need
  signed images.

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

   Until **Current** reads `USER_PROGRAMMABLE_CC=1` *and* `DPA_AUTHENTICATION=0`, `doca_pcc` refuses
   to start — with `PCC CONFIG object is not supported on this device` (knob 1) or
   `Failed to create PRM process` / syndrome `0x8f333` (knob 2).

> **Gotcha: `USER_PROGRAMMABLE_CC=1` appears to disable the NIC's stock DCQCN, not just add an
> optional path alongside it.** With `doca_flow_ecn` marking 100% of packets CE and `doca_pcc`
> **not** running, expect **no** throughput drop — traffic stays pinned at line rate (~92.6 Gb/s)
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
> unconditionally, whether or not a program is loaded there. With `doca_pcc` not running that slot
> is empty, so CNPs still get parsed and counted but nothing ever writes a new rate back to the QP.
> Practically: **on a DPU configured for this tutorial, there's no way to observe plain stock
> DCQCN** — either a PCC algorithm is loaded and reacting, or nothing reacts, regardless of `-R`.
> (Not yet confirmed by the one fully decisive test — `USER_PROGRAMMABLE_CC=0` + a full
> power-cycle — since that would also temporarily break Part IV.)

## Reference testbeds

Everything above describes **testbed A**, the development box. The tutorial is also being brought
up on **testbed B**, a BlueField-3 in an NVIDIA lab pod, to make sure the exercises don't silently
depend on one machine's wiring. The two differ in almost every axis that the data path touches, so
they are worth stating explicitly — most of the portability work in `doca-flow/` exists because of
a line in this table.

| | **Testbed A** (`bluefield-1`, dev box) | **Testbed B** (`dpu`, NVIDIA lab) |
|---|---|---|
| Board | BlueField-3, 2×100G | BlueField-3 **B3220**, 2×200G |
| BSP / OS release | `bf-bundle-2.9.1-40_24.11-ubuntu-22.04_prod` | `bf-bundle-2.7.0-33_24.04_ubuntu-22.04_prod` |
| Kernel | `5.15.0-1057-bluefield` | `5.15.0-1042-bluefield` |
| **DOCA** | **2.9.1** | **2.7.0** (`doca-devel 2.7.0085-1`) |
| Firmware | `32.43.2402` | `32.41.1000` (PSID `MT_0000000884`) |
| DOCA/OFED | `24.10-1.1.4` | `24.04-0.6.6` |
| OVS | `2.9.1-0013-24.11-based-3.3.3` | `2.7.0-0056-24.01-based-2.17.8` |
| **p0 ↔ p1** | **direct 100G DAC** (FS `Q28-PC03`, 3 m) | **no DAC** — each port goes to a *different* leaf switch |
| p0 / p1 MAC | `f0:fb:7f:e2:e2:76` / `:77` | `5c:25:73:e6:00:d0` / `:d1` |
| MTU | 1500 | 1500 |
| **SF layout _as shipped_** | **one per PF** | **both on PF0** |
| Receiver SF, as shipped | `pci/0000:03:00.0/229408` → `en3f0pf0sf0` / `enp3s0f0s0` / `mlx5_2` | `pci/0000:03:00.0/229408` → `en3f0pf0sf2` / `enp3s0f0s2` / `mlx5_2` |
| Sender SF, as shipped | `pci/0000:03:00.1/294944` → `en3f1pf1sf0` / `enp3s0f1s0` / `mlx5_3` | `pci/0000:03:00.0/229409` → `en3f0pf0sf3` / `enp3s0f0s3` / `mlx5_3` |
| OVS bridges, as shipped | `ovsbr1` (`p0`,`pf0hpf`,`en3f0pf0sf0`), `ovsbr2` (`p1`,`pf1hpf`,`en3f1pf1sf0`) | `host_br` (`pf0hpf`,`en3f0pf0sf2`), `wire_br` (`p0`,`en3f0pf0sf3`); **`p1` and `pf1hpf` in no bridge** |
| **Build** | native (`ninja -C build`, DOCA 2.9 matches) | **container** — native fails, DOCA 2.7 ships no `doca-common.pc` |

The "as shipped" rows are what each box looks like *before* the tutorial touches it, and they are
why [`setup_roce_loopback.sh`](setup_roce_loopback.sh) wipes and rebuilds rather than adapting:
testbed B ships with **both SFs on PF0**, which can never work here, because the sender has to sit
on PF1 for its traffic to leave on p1 and re-enter at p0 where DOCA Flow marks it. After the script
runs, **both boxes are identical** — one SF per PF at sfnum 0, `mlx5_2`/`mlx5_3`, `ovsbr1`/`ovsbr2`
— and every command in this README works unchanged on either.

Nothing about the SF layout is fixed in hardware: SFs are created and destroyed at run time
(`mlnx-sf`, a wrapper over `devlink port add/del`), their sfnum is chosen by us, and
`PER_PF_NUM_SF=False(0)` on both boxes means the SF pool is device-wide, so PF1 can host one with
no NV-config change or power-cycle. None of it survives a reboot.
| `USER_PROGRAMMABLE_CC` | `True(1)` | **`False(0)`** — Part IV (PCC) will not start until this is flipped |
| `DPA_AUTHENTICATION` | `False(0)` | `False(0)` |
| `PER_PF_NUM_SF` / `PF_TOTAL_SF` / `NUM_OF_VFS` | `False(0)` / `0` / `16` | `False(0)` / `0` / `16` |

Both boxes run DPU mode with **both PFs in `switchdev`** (`devlink dev eswitch show pci/0000:03:00.{0,1}`
→ `mode switchdev inline-mode none encap-mode basic`), so the eSwitch assumptions in
[Configuration setup](#configuration-setup) hold on either.

### Testbed B: what sits on the wire

Testbed A's two ports are cabled to each other, so "the wire" is a closed 3 m loop. Testbed B's
ports are cabled into a production-style fabric, and **not to the same switch** — passive LLDP
capture (`tcpdump -i pN -e 'ether proto 0x88cc'`) shows a different neighbour on each port:

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

So the p1 → p0 hop the tutorial depends on **does exist on testbed B**; it is just two switch hops
across a shared lab VLAN instead of a private cable. Two consequences follow:

- **The loopback must not rely on flooding.** `ovsbr2` would otherwise flood the receiver's MAC as
  unknown-unicast out `p1`, which is harmless on a private DAC but on testbed B sprays every frame
  across pod23's VLAN at 92 Gb/s. `setup_roce_loopback.sh` pins both directions with explicit
  OpenFlow rules so the fabric unicasts p1 → leaf-2 → leaf-1 → p0; see
  [Why `ovsbr2` gets two explicit flows](#scalable-functions-sfs--the-roce-endpoints).
- **SF MACs must be unique per DPU**, since they are visible to the whole VLAN — hence deriving
  them from each PF uplink's burned-in address rather than hardcoding.
- **The uplink netdevs cannot be moved into a network namespace.** `ip link set p1 netns ...` fails
  with `RTNETLINK answers: Invalid argument` on the mlx5 switchdev uplink; use a macvlan (as above)
  if you need to probe a port from an isolated namespace.

### Testbed B: building

Testbed B ships **DOCA 2.7**, and a native build fails at configure time — 2.7 does not install the
pkg-config files `meson.build` looks for:

```
Run-time dependency doca-common found: NO (tried pkgconfig)
meson.build:36:0: ERROR: Dependency "doca-common" not found, tried pkgconfig
```

Use the container instead, which carries DOCA 2.9 userspace regardless of what the DPU ships:

```bash
./run_container.sh
```

**DOCA 2.9 userspace drives testbed B's older OFED 24.04 kernel driver without complaint** — the
HWS pipes build, the SF representor is found, and packets forward. That skew is the thing worth
knowing: the container does not need a matching host DOCA version.

### Testbed B: validation

The full exercise was run on both boxes after `setup_roce_loopback.sh`, with `ib_write_bw`
(`-R`, 64 KB writes, 10 s) and `doca_flow_ecn --percent 100`. CNP counters are read from the **PF**
devices in the default namespace (`mlx5_0` = receiver/NP side, `mlx5_1` = sender/RP side) — the
SFs' own RDMA devices have no `hw_counters` directory:

| | testbed A (DAC) | testbed B (fabric) |
|---|---|---|
| baseline throughput | 92.61 Gb/s | 92.10 Gb/s |
| baseline `np_cnp_sent` Δ | 0 | 169 |
| `--percent 100` throughput | 92.61 Gb/s | 92.07 Gb/s |
| `--percent 100` `np_cnp_sent` Δ | 2,399,569 | 2,413,989 |
| `--percent 100` `rp_cnp_handled` Δ | 2,399,568 | 2,413,990 |
| packets CE-marked | 113,053,352 | 113,510,981 |

Two switch hops cost essentially nothing: testbed B tracks testbed A's private DAC to within 0.6%,
and the marking → CNP loop is unambiguously attributable to the DOCA Flow program (169 → 2.4 M).

> Note for Part IV: testbed B handled 2.4 M CNPs with `USER_PROGRAMMABLE_CC=0` — i.e. with stock
> DCQCN nominally in charge — and throughput still did not move (92.10 → 92.07 Gb/s). That is a
> data point against the working explanation in the
> [`USER_PROGRAMMABLE_CC` gotcha](#firmware-nv-config-pcc-prerequisite) above, which assumed the
> `=1` setting was what suppressed the stock reaction. It is not conclusive: the default
> `rpg_gd=11` is a ≈0.05% cut per CNP, which a single QP at line rate may simply absorb.

### Testbed B: handing the DPU back

`setup_roce_loopback.sh` destroys the staging that testbed B ships with. To put it back:

```bash
sudo ./reset_nvidia_dpu_to_original_config.sh
```

It recreates NVIDIA's two PF0 SFs with their original sfnums (2 and 3) and hardware addresses
(`44:38:39:00:00:02` / `:03`, trust on), rebuilds `host_br` and `wire_br`, leaves PF1 empty, and
verifies each of those before exiting non-zero if anything is off. A **reboot also restores the box
unconditionally** — SFs, bridges and namespaces are all runtime state — so the script only exists
to save a reboot.

Note that Part IV (PCC) needs `USER_PROGRAMMABLE_CC=1`, which testbed B does **not** have set. That
is an NV-config change requiring a real power-cycle, so as it stands testbed B runs Parts I–III (the
DOCA Flow exercises) but not Part IV.

# Testing the setup

Checking if we indeed get the 100G link between the two ports of the BlueField-3 DPU:

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

- **DOCA is installed under `/opt/mellanox/doca`.** The BSP install provides the runtime libraries,
  the build toolchain used for the DPA (`dpacc` at `/opt/mellanox/doca/tools/dpacc`), and the
  pristine sample/application sources at `/opt/mellanox/doca/applications` (notably
  `/opt/mellanox/doca/applications/pcc`, the base for Part IV).
- **`meson` and `ninja` are on `PATH`** (system packages). Together with `dpacc` they are the only
  tools needed to build both parts **natively on the Arm** — no DOCA devel container is required
  (though one is provided as an alternative — see below).
- **`perftest` (`ib_write_bw`) and `mlxconfig`/`mlxfwreset` (MFT)** are installed for driving RoCE
  traffic and for the firmware NV-config step above.
- Hugepages are reserved (DPDK/DPA programs — the `doca-flow` programs and `doca_pcc` — need them). This is
  done for you by [`setup_roce_loopback.sh`](setup_roce_loopback.sh)
  (`dpdk-hugepages.py --reserve 4G`); it does not persist across reboots, so re-run
  `setup_roce_loopback.sh` after every boot/power-cycle.

## Optional: run the whole tutorial in a container

If you would rather not build against the DPU's own DOCA install — or want a disposable,
reproducible environment — [`run_container.sh`](run_container.sh) builds the image described by
the [`Dockerfile`](Dockerfile) and drops you into a shell inside it:

```bash
./run_container.sh
```

The base is `nvcr.io/nvidia/doca/doca:2.9.1-devel` — the same DOCA release as the DPU
(`2.9.1008`), which matters because the `doca-flow` sources here target the 2.9 Flow API and the
Part IV DPA algo is loaded by the host's driver/firmware stack. (The `doca-3.2` and `doca-3.4`
branches carry this same setup ported to those newer releases.)

What you get:

- The three `doca-flow` programs are **already built** at `/workspace/build/doca-flow/`, and
  `ttyplot` at `/workspace/ttyplot/ttyplot` — both compiled at image-build time.
- The container runs as the `ubuntu` user with **passwordless sudo**, so every script in this repo
  (`setup_roce_loopback.sh`, `run_server.sh`, `run_client.sh`, `benchmark.sh`) works unchanged.
- It starts `--privileged --net=host` with `/dev/infiniband`, `/dev/hugepages` and
  `/run/openvswitch` bind-mounted, so the DPU hardware is fully reachable from inside.
- [`docker-entrypoint.sh`](docker-entrypoint.sh) runs `setup_roce_loopback.sh` on start, so the
  per-boot RoCE loopback is already up when the shell appears. `ns0`/`ns1` are created **inside**
  the container's network namespace and disappear with it, so nothing leaks onto the host. Skip
  this step with `docker run -e SKIP_ROCE_SETUP=1 ...` if you only want to build.

The Part IV PCC build stays a **runtime** step inside the container, exactly as documented below —
run the same commands from `Building and running DOCA PCC (Part IV)`. One difference from the DPU:
in the container `VERSION` already sits inside `/opt/mellanox/doca/applications/`, so the tree
copies over complete and `meson setup` needs no extra help.

# Building the DOCA Flow programs

`doca-flow/` builds three programs, sharing the same PORT_DEMUX/eSwitch-forwarding scaffold but
differing in what (if anything) they do to a packet before delivering it to the receiver's SF:

- **`doca_flow_nop`** — forwards packets untouched, no header-modify action at all. Performance
  control group, and the base file to build a pipeline on top of.
- **`doca_flow_mac`** — rewrites the dst MAC on every packet to the receiver's real MAC, no ECN.
  Since the wire already carries that same MAC, this rewrite is currently an identity op and
  runs at full line rate — all three programs currently perform identically.
- **`doca_flow_ecn`** — sets ECN CE on a configurable fraction of IPv4 packets (`--percent`,
  `[0, 100]`, fractional values allowed). `--percent 100` marks everything, `--percent 0` marks
  nothing (both exact); anything in between samples via a HW random field, rounded down to the
  nearest power-of-two fraction. Default: 100.

Initial setup of the build directory:

```
$ meson setup build
```

Building (all three programs):

```
$ cd build
$ ninja
```

Running (pick one):

```
$ sudo ./doca-flow/doca_flow_nop
$ sudo ./doca-flow/doca_flow_mac
$ sudo ./doca-flow/doca_flow_ecn
```

# Tutorial exercises

We want to exercise both the DOCA Flow pipeline and the DOCA PCC pipeline. The exercises are designed to be run on NVIDIA BlueField-3 platforms. The exercises are organized into two main parts: DOCA Flow and DOCA PCC. The plan is to have the participants build first a DOCA Flow application that sets the ECN bits of some packets, and later build a DOCA PCC application that reacts to the ECN bits set by the DOCA Flow application.

The example will be tested by running both a server and a client on the ARM cores, and see how the throughput changes when the ECN bits are set by the DOCA Flow application.

- Server: [run_server.sh](run_server.sh)
- Client: [run_client.sh](run_client.sh)

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

- **`doca_flow_ecn` on PF0 (`mlx5_0`)** replaces the physical switch's WRED/ECN marking that the
  original 2×BlueField-3 PCC testbed relied on. It marks **CE on every IPv4 packet** arriving from
  the wire (unconditional, not ECT→CE — so any RoCE generator drives the loop).
- **The receiver (`mlx5_2`) generates CNPs** in hardware when it sees CE-marked packets — standard
  RoCE behavior, no PCC instance needed on the NP side.
- **`doca-pcc-ecn` (RP) on PF1 (`mlx5_1`)** is the reaction point: each CNP triggers a
  multiplicative rate decrease on the sender's QP (see `doca-pcc-ecn/`).

## Building and running DOCA PCC (Part IV)

The PCC controller (`doca-pcc-ecn/pureecn_dcqcn.patch`) patches NVIDIA's stock `rtt_template` PCC
application and builds **natively on the Arm** from the DOCA install (see
[Requirements and assumptions](#requirements-and-assumptions)):

```bash
cd doca-pcc-ecn

# 1. Writable copy of the (read-only) system application tree, into app/ (git-ignored).
#    /opt ships a prebuilt build/ subdir — drop it so we configure our own from scratch.
cp -a /opt/mellanox/doca/applications ./app
rm -rf app/build

# 2. Apply the pure-ECN controller patch (1 file, touches only algo/rtt_template.c):
patch -p1 -d app < pureecn_dcqcn.patch

# 3. Configure + build. Put dpacc on PATH so build_device_code.sh finds it; ninja invokes it to
#    compile the DPA (device-side) algo. Patch BEFORE meson setup — the device build target does
#    not re-trigger on algo edits, so after any algo change reconfigure from a clean build dir.
cd app
PATH="/opt/mellanox/doca/tools:$PATH" meson setup build -Denable_all_applications=false -Denable_pcc=true
PATH="/opt/mellanox/doca/tools:$PATH" ninja -C build
# => doca-pcc-ecn/app/build/pcc/doca_pcc  (host loader; the patched algo is baked into the DPA image)
```

Requires the firmware knob (`USER_PROGRAMMABLE_CC=1`, *Current* value) to be live, or `doca_pcc`
refuses to start — see [Firmware NV-config](#firmware-nv-config-pcc-prerequisite).

The **device mapping is converged to our single-DPU setup** as follows:

| Step | Original 2×BF3 testbed | **Our single-DPU DAC loopback** |
|---|---|---|
| ECN marking | SONiC switch WRED on DSCP26→Q3 | `doca_flow_ecn` on PF0 (`mlx5_0`) |
| RP PCC device | `mlx5_1` on the sender host | `mlx5_1` (PF1 uplink — sender is p1/ns1) |
| NP PCC device | `mlx5_1` on the receiver host | none (receiver HW CNP is enough) |
| Sender RoCE | `mlx5_3` on sender host | `mlx5_3` in `ns1` (client) |
| Receiver RoCE | `mlx5_3` on receiver host | `mlx5_2` in `ns0` (server) |

Combined run (start Flow first, then the RP controller, then drive traffic):

```bash
# 1. ECN marker (PF0) — leave running:
sudo ./build/doca-flow/doca_flow_ecn

# 2. RP PCC controller on PF1 — FOREGROUND, stays Active for the whole window:
sudo timeout 40 stdbuf -oL ./doca-pcc-ecn/app/build/pcc/doca_pcc -d mlx5_1 -l 50 > rp.log 2>&1 &

# 3. Drive RoCE traffic. -R (rdma_cm) is MANDATORY: the QP→algo-slot-0 binding is negotiated
#    via RoCE ECE, which perftest only performs with -R. Without it the custom controller never
#    fires — and, per the gotcha above, no rate reaction happens at all (the "default algo"
#    fallback is not observed to actually reduce rate on this NV-config).
sudo ip netns exec ns0 ib_write_bw -d mlx5_2 -R -x 1 -F --report_gbits --run_infinitely -D 1            # server
sudo ip netns exec ns1 ib_write_bw -d mlx5_3 -R -x 1 -F 10.0.0.1 --report_gbits --run_infinitely -D 1   # client

grep PURE_ECN rp.log            # rate walking down as CNPs arrive => the loop is live
sudo pkill -INT -x doca_pcc     # stop gracefully (never SIGKILL: leaves a ghost DPA context)
```

> **If no CNPs arrive** (`grep PURE_ECN rp.log` stays empty while `doca_flow_ecn` reports rising CE
> counts): the receiver's HW **CNP generation** may be priority-scoped. Add a traffic class to steer
> traffic onto an ECN-enabled priority — e.g. `--tclass=104` (DSCP 26 → TC3) on both `ib_write_bw`
> ends — even though `doca_flow_ecn` already marks CE regardless of queue.

