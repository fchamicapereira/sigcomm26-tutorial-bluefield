#!/usr/bin/env bash
#
# Launches the RoCE server + client together and shows a live ttyplot chart of the
# client's (sender's) throughput. Both are stopped together on exit (Ctrl-C).
# ttyplot is optional: resolved on PATH first (the fleet installs it to /usr/local/bin via
# admin/local_scripts/install_deps.sh), then a repo-local build; absent that, plain throughput.
#
set -uo pipefail

SERVER_NETNS="ns0";  SERVER_DEV="mlx5_2"                       # receiver SF (PF0)
CLIENT_NETNS="ns1";  CLIENT_DEV="mlx5_3";  SERVER_IP="10.0.0.1" # sender SF (PF1)
GID="1"                # -x: RoCEv2 GID index on both SFs
INTERVAL_SEC="1"       # -D: seconds between throughput reports

# Tells the tool to use the RDMA Connection Manager (RDMA_CM) to establish the initial connection
# between the client and the server, rather than the default out-of-band TCP connection.
# REQUIRED for the DOCA PCC exercise.
# It binds the QP to algo slot 0; optional for the ECN-only test.
# Set to "" to disable.
USE_RDMA_CM="1"

# Two independent buffering hazards sit between ib_write_bw and ttyplot; either one alone leaves
# the chart stuck on "waiting for data from stdin" until a ~4 KB buffer fills (then it jumps in
# one burst):
#   1. ib_write_bw block-buffers stdout when it's a pipe (not a TTY) -> `stdbuf -oL` below.
#   2. mawk block-buffers its INPUT. `-W interactive` makes mawk read line-by-line and write
#      unbuffered. (awk's fflush() only affects OUTPUT, so it is not enough on its own.)
# Which awk you get depends on where you run: the DPU natively has gawk, which already streams
# and prints a warning if handed -W interactive; the tutorial container (Ubuntu 22.04 minimal,
# see the Dockerfile) has mawk, which needs the flag. So detect and pass it only to mawk.
AWK=(awk)
if [ "$(basename "$(readlink -f "$(command -v awk)")")" = "mawk" ]; then
  AWK=(awk -W interactive)
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# A live chart is nice but optional. Locate ttyplot in this order:
#   1. on PATH -- the permanent install. Build it once (admin/local_scripts/setup_ttyplot.sh) and
#      `sudo install -m0755 ttyplot/ttyplot /usr/local/bin/`; then every repo/checkout finds it.
#   2. a source build in the repo -- <repo-root>/ttyplot (setup_ttyplot.sh's default) or right beside
#      this script, which is where the Dockerfiles place it.
# If ttyplot is nowhere, fall back to plain per-second throughput lines, so this script still
# launches both ends with one command everywhere.
TTYPLOT="$(command -v ttyplot || true)"
if [ -z "$TTYPLOT" ]; then
  for cand in "$SCRIPT_DIR/../ttyplot/ttyplot" "$SCRIPT_DIR/ttyplot/ttyplot"; do
    [ -x "$cand" ] && { TTYPLOT="$cand"; break; }
  done
fi
HAVE_TTYPLOT=0
if [ ! -t 1 ]; then
  # ttyplot is a full-screen ncurses program: with stdout redirected to a file or a pipe it dies on
  # "Error opening terminal", taking the reader out from under the client and leaving this script
  # spinning on a pipe nobody is holding. Plain lines are what a redirected run wants anyway.
  echo "Note: stdout is not a terminal -- showing plain throughput instead of a live chart." >&2
elif [ -n "$TTYPLOT" ] && [ -x "$TTYPLOT" ]; then
  HAVE_TTYPLOT=1
else
  echo "Note: ttyplot not found (not on PATH or in the repo) -- showing plain throughput instead of a live chart." >&2
fi

# A DOCA Flow program OWNS the NIC's eSwitch: while its pipeline is not forwarding, no packet gets
# across. That is not an edge case during the exercise — it is most of it, since every half-written
# pipeline drops traffic, as does the moment between commenting out the no-op root pipe and
# finishing your own.
#
# ib_write_bw does not ride that out. Its RC queue pair exhausts its transport retries in about a
# second and the client aborts for good:
#     Completion with error at client / Failed status 12: wr_id 0 syndrom 0x81
#     Error occurred while running infinitely! aborting ...
# and it does NOT come back when forwarding does. The server process survives but its queue pair is
# equally dead, so a fresh client cannot attach to it either:
#     Unexpected CM event / Unable to perform rdma_client function
# Recovering means restarting BOTH ends, which is why this script supervises them rather than
# launching them once. A broken pipeline shows up as the chart going flat, and it fills in again by
# itself once you fix it — nothing to restart by hand.
RESTART_DELAY_SEC="2"
RESTART_LOG="/tmp/benchmark-restarts.log"

# The [i] keeps the pattern from matching the `sudo pkill …` process that carries it in its own argv.
# Match by argv, not $!: sudo runs with use_pty on this box, so a backgrounded `sudo … &` job's PID
# is sudo's own pty-monitor process, not ib_write_bw itself — killing/waiting on that PID doesn't
# reliably reach or reap the real command.
#
# SIGINT alone is not enough: a server parked in --run_infinitely waiting for a client ignores it and
# survives, and every survivor keeps its share of the link, so the next run reads half the throughput
# for no visible reason. Escalate to SIGKILL for anything still standing.
stop_both() {
  local pattern sig
  for sig in INT KILL; do
    for pattern in "[i]b_write_bw -d $SERVER_DEV" "[i]b_write_bw -d $CLIENT_DEV"; do
      sudo pkill -"$sig" -f "$pattern" 2>/dev/null
    done
    [ "$sig" = INT ] && sleep 1
  done
}

# Set on the way out so the supervision loop knows a Ctrl-C is not a broken pipeline. It runs in a
# subshell (the left-hand side of the pipe below), so it cannot see a variable — hence a file.
STOP_FILE="$(mktemp -u /tmp/benchmark-stop.XXXXXX)"

# WHY Ctrl-C NEEDS HELP HERE. These cards set `Defaults use_pty` in /etc/sudoers, so sudo runs its
# command on a pseudo-terminal and, to pass keystrokes through to it, puts OUR terminal into raw
# mode — ISIG included — for as long as it runs. With ISIG off the line discipline never turns ^C
# into SIGINT: the keystroke arrives as a plain byte that nothing in this pipeline reads. bash is
# therefore never signalled, the EXIT trap below never fires, and both ib_write_bw ends keep going
# until the terminal itself is closed.
#
# Measured on a tutorial card: `stty -a` reads `-isig` for the whole run — with ttyplot AND without
# it — and two Ctrl-Cs in a row leave the process tree byte-for-byte identical. (ttyplot is not the
# culprit, though it looks like one: on its own it leaves ISIG alone and quits on `q` or ^C.)
#
# So take ISIG back, and keep taking it back: every restart in supervise() spawns a fresh pair of
# sudos, and each one raw-modes the terminal again, so a one-shot at startup would hold only until
# the first restart. Once a second is plenty — the only window it leaves is the moment just after a
# restart, when there is barely anything running to stop.
#
# ONLY ISIG is touched. sudo wants the rest of raw mode to forward keystrokes to the command, and
# ib_write_bw has no use for a keyboard, so taking this one flag back costs nothing.
TTY_SAVED=""
ISIG_KEEPER=""
start_isig_keeper() {
  [ -t 0 ] || return 0                       # not a terminal: nothing to fix, nothing to restore
  TTY_SAVED="$(stty -g </dev/tty 2>/dev/null)" || { TTY_SAVED=""; return 0; }
  # EVERY stty here reads /dev/tty explicitly rather than inheriting stdin. With job control off --
  # which is the case in any script -- bash redirects an asynchronous command's stdin from
  # /dev/null, so a backgrounded `stty isig` would silently be configuring /dev/null and give up on
  # the first pass. Cost me a full test run to find; the loop looked alive and did nothing.
  #
  # It runs in this script's process group, so its tcsetattr counts as a foreground one and does not
  # raise SIGTTOU, and it dies to the same Ctrl-C it exists to enable.
  while :; do
    stty isig </dev/tty 2>/dev/null || exit 0   # terminal gone: stop rather than spin
    sleep 1
  done &
  ISIG_KEEPER=$!
}

CLEANED_UP=""
cleanup() {
  [ -n "$CLEANED_UP" ] && return
  CLEANED_UP=1
  : > "$STOP_FILE"
  [ -n "$ISIG_KEEPER" ] && kill "$ISIG_KEEPER" 2>/dev/null
  echo "Stopping server and client..."
  stop_both
  sleep 1
  rm -f "$STOP_FILE"
  # Hand the terminal back as we found it. sudo restores its own idea of the modes when it exits,
  # but a run killed mid-flight may leave no sudo around to do it, and a shell left in raw mode is
  # the second-worst thing to hand a participant after a benchmark that will not stop.
  [ -n "$TTY_SAVED" ] && stty "$TTY_SAVED" </dev/tty 2>/dev/null
  return 0
}
# EXIT alone is enough: bash runs it on normal completion AND after an uncaught INT/TERM.
trap cleanup EXIT

start_server() {
  sudo ip netns exec "$SERVER_NETNS" \
      ib_write_bw \
      -d "$SERVER_DEV" \
      -x "$GID" \
      -F \
      ${USE_RDMA_CM:+-R} \
      --report_gbits \
      --run_infinitely \
      -D "$INTERVAL_SEC" \
      > /dev/null 2>&1 &
}

run_client() {
  sudo ip netns exec "$CLIENT_NETNS" \
      stdbuf -oL ib_write_bw \
      -d "$CLIENT_DEV" \
      -x "$GID" \
      -F \
      ${USE_RDMA_CM:+-R} \
      "$SERVER_IP" \
      --report_gbits \
      --run_infinitely \
      -D "$INTERVAL_SEC"
}

# Notices never go to stdout: that stream is the client's throughput, and the chart reads it. With
# ttyplot drawing, stderr would land on top of the chart too, so there they go to the log alone.
note() {
  printf '%s %s\n' "$(date '+%H:%M:%S')" "$1" >> "$RESTART_LOG"
  [ "$HAVE_TTYPLOT" = 1 ] || echo "$1" >&2
}

# Keep a client running for as long as this script lives, restarting the pair whenever the pipeline
# takes it down. Everything reaching stdout is the client's own output, so the reader below is
# unchanged.
supervise() {
  # This runs as the left-hand side of a pipeline, i.e. a subshell, which does NOT inherit the EXIT
  # trap. Exit on Ctrl-C rather than treating it as one more broken pipeline and restarting.
  trap 'exit 130' INT TERM

  local attempt=0
  while [ ! -e "$STOP_FILE" ]; do
    attempt=$((attempt + 1))
    if [ "$attempt" -gt 1 ]; then
      note "client stopped — is the pipeline forwarding? restarting both ends (attempt $attempt)"
      # A blank line is ignored by the reader's filter, but if the reader is GONE this write raises
      # SIGPIPE and takes the loop down with it. Without it, a Ctrl-C that kills the chart first
      # would leave us restarting the pair forever against a pipe nobody is holding.
      echo
    fi

    stop_both            # never leave half a pair behind: both queue pairs have to be fresh
    sleep 1
    start_server
    sleep 2              # let the server reach "waiting for client"
    run_client           # blocks until the pipeline breaks it, or until Ctrl-C

    [ -e "$STOP_FILE" ] && break
    sleep "$RESTART_DELAY_SEC"
  done
}

echo "Starting server and client (restarts are automatic; see $RESTART_LOG)..."
start_isig_keeper
if [ "$HAVE_TTYPLOT" = 1 ]; then
  supervise \
    | "${AWK[@]}" '$1 == "65536" && NF == 5 { print $4; fflush(); }' \
    | "$TTYPLOT" -t "RoCE throughput (client -> server)" -u "Gb/s"
else
  supervise
fi
