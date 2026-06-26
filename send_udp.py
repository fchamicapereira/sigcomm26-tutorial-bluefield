#!/usr/bin/env python3

from scapy.all import *

pkts = [
    Ether(dst='ff:ff:ff:ff:ff:ff', src='f0:fb:7f:e2:e2:76') /
        IP(src='10.0.0.1', dst='10.0.0.2', tos=0x01) /
        UDP(sport=1234, dport=4791) / Raw(b'ECT1'),
    Ether(dst='ff:ff:ff:ff:ff:ff', src='f0:fb:7f:e2:e2:76') /
        IP(src='10.0.0.1', dst='10.0.0.2', tos=0x02) /
        UDP(sport=1234, dport=4791) / Raw(b'ECT0'),
]

sendp(pkts * 100, iface='p0', inter=0.01)