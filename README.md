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

The two physical ports **p0 and p1 are wired directly to each other with a single 100G DAC
cable** (loopback). Everything below therefore happens inside one DPU: packets that leave p1
physically re-enter at p0.

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

[`setup_roce_loopback.sh`](setup_roce_loopback.sh) creates these bridges and ports idempotently
(`ovs-vsctl --may-exist ...`), so it's safe to re-run and will re-establish this layout even from
a box where OVS was never configured.

### Scalable Functions (SFs) — the RoCE endpoints

Two SFs, one per PF, carry the actual RoCE traffic. Each SF's netdev is moved into its own
**network namespace** so that the RoCE traffic is forced out onto the wire (crossing the DAC cable)
instead of being delivered locally by the host kernel:

| SF (aux dev) | RDMA dev | Netdev (in ns) | Namespace | IP | MAC | Role |
|---|---|---|---|---|---|---|
| `mlx5_core.sf.2` (on PF0) | `mlx5_2` | `enp3s0f0s0` | `ns0` | `10.0.0.1` | `02:c9:c8:ff:a6:24` | **Receiver / server (NP)** |
| `mlx5_core.sf.3` (on PF1) | `mlx5_3` | `enp3s0f1s0` | `ns1` | `10.0.0.2` | `02:26:3d:d7:e0:4b` | **Sender / client (RP)** |

Run [`setup_roce_loopback.sh`](setup_roce_loopback.sh) to build this whole layout: it ensures the
OVS bridges above exist, reserves hugepages, creates `ns0`/`ns1`, moves each SF's RDMA device and
netdev into its namespace, assigns the IPs, and pins the sender's neighbor for the receiver
directly to `mlx5_2`'s real MAC (read dynamically from `enp3s0f0s0`, not hardcoded). **None of
this survives a reboot or power-cycle, so re-run it after every boot:**

```bash
./setup_roce_loopback.sh
```

> **Why namespaces.** They stop the *Linux kernel* from delivering `10.0.0.1 ↔ 10.0.0.2` locally
> — both IPs sit on this one host, so without isolation the kernel short-circuits them and RoCE
> never touches the wire. Each SF in its own netns forces the traffic out. The packet then
> crosses the DAC because **p0 and p1 are independent switchdev eSwitches** — PF1 has no vport
> for `mlx5_2`, so `ovsbr2` (see above) floods it as unknown-unicast out p1 → DAC → p0
> *regardless of the destination MAC*. (There is no cross-PF eSwitch shortcut to defeat.)
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
  tools needed to build both parts **natively on the Arm** — no DOCA devel container is required.
- **`perftest` (`ib_write_bw`) and `mlxconfig`/`mlxfwreset` (MFT)** are installed for driving RoCE
  traffic and for the firmware NV-config step above.
- Hugepages are reserved (DPDK/DPA programs — the `doca-flow` programs and `doca_pcc` — need them). This is
  done for you by [`setup_roce_loopback.sh`](setup_roce_loopback.sh)
  (`dpdk-hugepages.py --reserve 4G`); it does not persist across reboots, so re-run
  `setup_roce_loopback.sh` after every boot/power-cycle.

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

