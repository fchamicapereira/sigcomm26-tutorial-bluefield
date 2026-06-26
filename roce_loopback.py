#!/usr/bin/env python3
"""
RoCEv2 cross-port traffic generator for BlueField-3 ECN / PCC tutorial.

Sender uses mlx5_3 (enp3s0f1s0 / p1, 10.0.0.2).
Receiver uses mlx5_2 (enp3s0f0s0 / p0, 10.0.0.1).
A DAC cable connects p0 <-> p1 so packets physically cross ports and DOCA Flow
on PF0 intercepts the ingress traffic to mark ECN bits.

Traffic path (data):
  mlx5_3 TX  ->  PF1 eSwitch  ->  p1 TX  ->  [DAC cable]  ->  p0 RX
                                                                      |
  mlx5_2 RX  <-  en3f0pf0sf0  <-  DOCA Flow ECT->CE  <-  PF0 eSwitch

ACK path (return):
  mlx5_2 TX  ->  DOCA Flow EGRESS  ->  p0 TX  ->  [DAC cable]  ->  p1 RX
                                                                          |
  mlx5_3 RX  <-  en3f1pf1sf0  <-  PF1 eSwitch DEFAULT  <-  p1 RX

Prerequisites (run once after doca_flow_ecn has started):
  # Receiver side — DPDK probe of PF0 clears enp3s0f0s0's GID; re-assign each run:
  sudo ip addr add 10.0.0.1/24 dev enp3s0f0s0
  sudo ip neigh add 10.0.0.2 lladdr 02:26:3d:d7:e0:4b dev enp3s0f0s0 nud permanent
  # Sender side — PF1 GID survives DPDK probe; only ARP needed:
  sudo ip addr add 10.0.0.2/24 dev enp3s0f1s0
  sudo ip neigh add 10.0.0.1 lladdr 02:c9:c8:ff:a6:24 dev enp3s0f1s0 nud permanent

The sender's packets carry ECT(0) in the IPv4 TOS field. DOCA Flow marks them CE.
The receiver's ibverbs stack sees CE-marked packets -> DOCA PCC generates CNP.

Usage:
    sudo python3 roce_loopback.py [--msgs N] [--size B] [--tos 0x02]
"""

import argparse
import time

from pyverbs.device import Context
from pyverbs.pd import PD
from pyverbs.cq import CQ
from pyverbs.mr import MR
from pyverbs.qp import QP, QPInitAttr, QPAttr, QPCap
from pyverbs.addr import AHAttr, GlobalRoute
from pyverbs.wr import SendWR, SGE, RecvWR
import pyverbs.enums as e

SENDER_DEV   = "mlx5_3"   # SF on p1 (enp3s0f1s0, 10.0.0.2) — traffic leaves here
RECEIVER_DEV = "mlx5_2"   # SF on p0 (enp3s0f0s0, 10.0.0.1) — receives CE-marked traffic
PORT         = 1
GID_IDX      = 1           # IPv4-mapped GID index


def make_qp(pd, cq, depth) -> QP:
    cap  = QPCap(max_send_wr=depth, max_recv_wr=depth, max_send_sge=1, max_recv_sge=1)
    init = QPInitAttr(qp_type=e.IBV_QPT_RC, scq=cq, rcq=cq, cap=cap)
    qp   = QP(pd, init)

    attr = QPAttr()
    attr.qp_state        = e.IBV_QPS_INIT
    attr.pkey_index      = 0
    attr.port_num        = PORT
    attr.qp_access_flags = (e.IBV_ACCESS_LOCAL_WRITE |
                             e.IBV_ACCESS_REMOTE_WRITE |
                             e.IBV_ACCESS_REMOTE_READ)
    qp.to_init(attr)
    return qp


def bring_to_rtr(qp, dest_qp_num, dest_gid, traffic_class):
    gr = GlobalRoute(dgid=dest_gid, sgid_index=GID_IDX, traffic_class=traffic_class)
    ah = AHAttr(gr=gr, is_global=1, dlid=0, sl=0, src_path_bits=0, port_num=PORT)

    attr = QPAttr()
    attr.qp_state           = e.IBV_QPS_RTR
    attr.path_mtu           = e.IBV_MTU_1024
    attr.dest_qp_num        = dest_qp_num
    attr.rq_psn             = 0
    attr.max_dest_rd_atomic = 1
    attr.min_rnr_timer      = 12
    attr.ah_attr            = ah
    qp.to_rtr(attr)


def bring_to_rts(qp):
    attr = QPAttr()
    attr.qp_state      = e.IBV_QPS_RTS
    attr.sq_psn        = 0
    attr.timeout       = 14
    attr.retry_cnt     = 7
    attr.rnr_retry     = 7
    attr.max_rd_atomic = 1
    qp.to_rts(attr)


def run(n_msgs: int, msg_size: int, traffic_class: int):
    ctx_tx = Context(name=SENDER_DEV)
    ctx_rx = Context(name=RECEIVER_DEV)

    gid_tx = ctx_tx.query_gid(PORT, GID_IDX)
    gid_rx = ctx_rx.query_gid(PORT, GID_IDX)

    ZERO_GID = "0000:0000:0000:0000:0000:0000:0000:0000"
    if gid_rx.gid == ZERO_GID:
        raise RuntimeError(
            f"{RECEIVER_DEV} GID[{GID_IDX}] is all-zeros. "
            f"DPDK probe of PF0 clears enp3s0f0s0 GIDs — re-assign after doca_flow_ecn starts:\n"
            f"  sudo ip addr add 10.0.0.1/24 dev enp3s0f0s0\n"
            f"  sudo ip neigh add 10.0.0.2 lladdr 02:26:3d:d7:e0:4b dev enp3s0f0s0 nud permanent")
    if gid_tx.gid == ZERO_GID:
        raise RuntimeError(
            f"{SENDER_DEV} GID[{GID_IDX}] is all-zeros. "
            f"Assign IP to enp3s0f1s0 (PF1 GID survives DPDK probe, only needs an address):\n"
            f"  sudo ip addr add 10.0.0.2/24 dev enp3s0f1s0\n"
            f"  sudo ip neigh add 10.0.0.1 lladdr 02:c9:c8:ff:a6:24 dev enp3s0f1s0 nud permanent")

    print(f"Sender  : {SENDER_DEV}   GID[{GID_IDX}] = {gid_tx.gid}")
    print(f"Receiver: {RECEIVER_DEV}  GID[{GID_IDX}] = {gid_rx.gid}")
    print(f"TOS     : 0x{traffic_class:02x}  "
          f"({'ECT(0)' if traffic_class == 0x02 else 'ECT(1)' if traffic_class == 0x01 else 'other'})")
    print(f"Sending : {n_msgs} x {msg_size} B")
    print()

    pd_tx  = PD(ctx_tx)
    pd_rx  = PD(ctx_rx)
    cq_tx  = CQ(ctx_tx, n_msgs + 16)
    cq_rx  = CQ(ctx_rx, n_msgs + 16)

    qp_tx = make_qp(pd_tx, cq_tx, n_msgs)
    qp_rx = make_qp(pd_rx, cq_rx, n_msgs)

    # Cross-device connect: TX points at RX's GID/QPN, RX points at TX's GID/QPN
    bring_to_rtr(qp_tx, qp_rx.qp_num, gid_rx, traffic_class)
    bring_to_rtr(qp_rx, qp_tx.qp_num, gid_tx, 0)  # receiver side: no ECT needed
    bring_to_rts(qp_tx)
    bring_to_rts(qp_rx)

    send_mr = MR(pd_tx, msg_size, e.IBV_ACCESS_LOCAL_WRITE)
    recv_mr = MR(pd_rx, msg_size, e.IBV_ACCESS_LOCAL_WRITE)

    # Pre-post all receives
    for i in range(n_msgs):
        sge = SGE(recv_mr.buf, msg_size, recv_mr.lkey)
        qp_rx.post_recv(RecvWR(sg=[sge], num_sge=1, wr_id=i))

    # Send burst
    t0 = time.monotonic()
    for i in range(n_msgs):
        sge = SGE(send_mr.buf, msg_size, send_mr.lkey)
        qp_tx.post_send(SendWR(opcode=e.IBV_WR_SEND, sg=[sge], num_sge=1, wr_id=i,
                               send_flags=e.IBV_SEND_SIGNALED))

    # Poll TX completions
    tx_done = 0
    rx_done = 0
    deadline = time.monotonic() + 10.0
    while (tx_done < n_msgs or rx_done < n_msgs) and time.monotonic() < deadline:
        n, wcs = cq_tx.poll(n_msgs - tx_done)
        for wc in wcs:
            if wc.status != e.IBV_WC_SUCCESS:
                print(f"  TX WC error: {wc.status}")
        tx_done += n

        n, wcs = cq_rx.poll(n_msgs - rx_done)
        for wc in wcs:
            if wc.status != e.IBV_WC_SUCCESS:
                print(f"  RX WC error: {wc.status}")
        rx_done += n

    elapsed = time.monotonic() - t0
    print(f"TX completions: {tx_done} / {n_msgs}")
    print(f"RX completions: {rx_done} / {n_msgs}")
    print(f"Elapsed       : {elapsed*1000:.1f} ms")
    if elapsed > 0 and rx_done > 0:
        print(f"Throughput    : {rx_done * msg_size * 8 / elapsed / 1e6:.1f} Mbit/s")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--msgs", type=int,                   default=1000)
    p.add_argument("--size", type=int,                   default=1024)
    p.add_argument("--tos",  type=lambda x: int(x, 0),  default=0x02,
                   help="IP TOS for sender: 0x02=ECT(0), 0x01=ECT(1)")
    args = p.parse_args()
    run(args.msgs, args.size, args.tos)


if __name__ == "__main__":
    main()
