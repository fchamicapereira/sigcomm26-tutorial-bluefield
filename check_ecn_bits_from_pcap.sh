#!/usr/bin/env bash
#
# Live-decode a pcap that a doca-flow program is still writing to, showing each packet's IP TOS
# byte so you can watch the ECN codepoint change as the marker runs.
#
# tcpdump has no follow mode — `tcpdump -r` reads to EOF and exits — so the file is streamed in
# through tail instead. `-v` is what makes tcpdump print the TOS field, which it renders as:
#
#     (nothing)        Not-ECT   — tcpdump omits `tos` entirely when the byte is zero
#     tos 0x1,ECT(1)   ECT(1)
#     tos 0x2,ECT(0)   ECT(0)
#     tos 0x3,CE       CE, i.e. the packet was marked
#
# Note the first line: an unmarked packet shows no `tos` at all rather than `tos 0x0`.

set -euo pipefail

PCAP_DEFAULT=out.pcap

usage() {
  cat <<EOF
Usage: $(basename "$0") [PCAP]

Follow a pcap file as it is written and print each packet with its ECN codepoint.

Arguments:
  PCAP        pcap file to read (default: ${PCAP_DEFAULT})

Options:
  -h, --help  show this help and exit

Examples:
  $(basename "$0")                 # follow ${PCAP_DEFAULT}
  $(basename "$0") capture.pcap    # follow capture.pcap

Runs until interrupted (Ctrl-C). The writer must flush whole packet records — tcpdump does this
with -U, libpcap code with pcap_dump_flush() — otherwise this stops on a half-written record with
"truncated dump file".
EOF
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
esac

if [ "$#" -gt 1 ]; then
  echo "ERROR: expected at most one argument, got $#." >&2
  echo >&2
  usage >&2
  exit 1
fi

PCAP="${1:-$PCAP_DEFAULT}"

if [ ! -e "$PCAP" ]; then
  echo "ERROR: '${PCAP}' does not exist." >&2
  echo "       Start the capture first, or pass the path explicitly (see --help)." >&2
  exit 1
fi

echo "== following ${PCAP} (Ctrl-C to stop) =="

# -c +0 starts at byte 0 so tcpdump receives the 24-byte pcap global header; a plain `tail -f`
# starts near the end of the file and tcpdump then rejects the stream as not a capture file.
# -l keeps tcpdump's output line-buffered, so packets appear as they arrive instead of in 4K blocks.
# -nn disables name resolution, and is NOT optional here: without it tcpdump does a reverse-DNS
# lookup for every address, and the RoCE endpoints (10.0.0.1/10.0.0.2) resolve nowhere. On a DPU
# with no working resolver each lookup blocks for seconds, so tcpdump prints the first packet and
# then appears to hang — including on Ctrl-C. Measured on one DPU: the same 1164-packet file took
# 40 s and showed 1 packet without -nn, and 20 ms showing all 1164 with it.
tail -c +0 -f "$PCAP" | tcpdump -r - -nn -v -l
