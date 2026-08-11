#!/usr/bin/env python3
"""Count ECN codepoints in a pcap written by doca_flow_ecn_pcap.

check_ecn_bits_from_pcap.sh answers "what is in this capture" for a human watching it scroll by.
This answers "how many of them were marked" for a script — one line of counts, and '@@key=value'
lines for admin/fleet.py.

Deliberately does not shell out to tcpdump: its output is prose meant for reading (an unmarked
packet prints no `tos` field at all rather than `tos 0x0`), so counting marked packets by grepping
it is guesswork. The pcap format is 24 bytes of global header and 16 per record, which is little
enough to just read.

Only IPv4 is counted. The ECN bits are the low two of the IPv4 TOS byte:

    0b00  Not-ECT   not ECN capable
    0b10  ECT(0)    ECN capable
    0b01  ECT(1)    ECN capable
    0b11  CE        congestion experienced — what the marker sets

Usage:
    ./pcap_ecn_stats.py capture.pcap             # one human-readable line
    ./pcap_ecn_stats.py --emit capture.pcap      # '@@key=value' lines
"""

import argparse
import pathlib
import struct
import sys

# The four byte orders/magics libpcap writes. The two 0xa1b2c3d4 spellings are microsecond
# timestamps, the 0xa1b23c4d ones nanosecond; the resolution does not matter here, only the
# endianness, because it decides how the per-record lengths are read.
MAGICS = {
    b"\xd4\xc3\xb2\xa1": "<",  # little-endian, microseconds
    b"\xa1\xb2\xc3\xd4": ">",  # big-endian,    microseconds
    b"\x4d\x3c\xb2\xa1": "<",  # little-endian, nanoseconds
    b"\xa1\xb2\x3c\x4d": ">",  # big-endian,    nanoseconds
}

LINKTYPE_ETHERNET = 1
ETHERTYPE_IPV4 = 0x0800
ETHERTYPE_VLAN = (0x8100, 0x88A8, 0x9100)  # 802.1Q and the QinQ spellings

ECN_NAMES = {0b00: "not_ect", 0b10: "ect0", 0b01: "ect1", 0b11: "ce"}


def ecn_of_frame(frame: bytes) -> int | None:
    """The ECN codepoint of an Ethernet frame, or None if it is not IPv4."""
    if len(frame) < 14:
        return None
    ethertype = struct.unpack_from("!H", frame, 12)[0]
    offset = 14
    # Walk however many VLAN tags are stacked on the front; each is 4 bytes carrying the next
    # ethertype. Our own capture path never tags, but a DPU cabled into a real fabric may see them.
    while ethertype in ETHERTYPE_VLAN:
        if len(frame) < offset + 4:
            return None
        ethertype = struct.unpack_from("!H", frame, offset + 2)[0]
        offset += 4
    if ethertype != ETHERTYPE_IPV4 or len(frame) < offset + 2:
        return None
    return frame[offset + 1] & 0b11  # the TOS byte's low two bits


def read_pcap(path: pathlib.Path) -> dict[str, int]:
    counts = {"packets": 0, "ipv4": 0, "not_ect": 0, "ect0": 0, "ect1": 0, "ce": 0, "truncated": 0}

    with path.open("rb") as f:
        header = f.read(24)
        if len(header) < 24:
            raise ValueError(f"{path}: too short to be a pcap ({len(header)} bytes) — nothing was captured")
        endian = MAGICS.get(header[:4])
        if endian is None:
            raise ValueError(f"{path}: not a pcap (magic {header[:4].hex()}); pcapng is not supported")
        linktype = struct.unpack_from(endian + "I", header, 20)[0]
        if linktype != LINKTYPE_ETHERNET:
            raise ValueError(f"{path}: link type {linktype}, expected Ethernet ({LINKTYPE_ETHERNET})")

        while True:
            record = f.read(16)
            if len(record) < 16:
                # A short read here is the writer being mid-record, not a corrupt file: this is
                # meant to be run against a pcap a live doca_flow_ecn_pcap is still appending to.
                if record:
                    counts["truncated"] += 1
                break
            caplen = struct.unpack_from(endian + "I", record, 8)[0]
            frame = f.read(caplen)
            if len(frame) < caplen:
                counts["truncated"] += 1
                break

            counts["packets"] += 1
            ecn = ecn_of_frame(frame)
            if ecn is None:
                continue
            counts["ipv4"] += 1
            counts[ECN_NAMES[ecn]] += 1

    return counts


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("pcap", type=pathlib.Path, help="pcap file to read")
    parser.add_argument("--emit", action="store_true", help="print '@@key=value' lines instead of the human-readable summary")
    args = parser.parse_args()

    try:
        counts = read_pcap(args.pcap)
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    if args.emit:
        for key, value in counts.items():
            print(f"@@pcap_{key}={value}")
    else:
        print(
            "packets {packets}  ipv4 {ipv4}  not-ect {not_ect}  ect0 {ect0}  "
            "ect1 {ect1}  ce {ce}  truncated {truncated}".format(**counts)
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
