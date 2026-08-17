#!/usr/bin/env python3
"""Drive the SIGCOMM'26 BlueField-3 tutorial fleet from your laptop.

This is transport and aggregation only, never logic: every verb ships a self-contained bash
script to each machine over stdin (`ssh <host> bash -s -- <args>`), runs them in parallel, and
renders one table of results. The scripts in local_scripts/ are the real work, and each one is
independently runnable by hand — which is the fallback for any machine this tool cannot reach.

Machines are named as Tailscale knows them, so there is no ssh_config and no addresses to track.
Auth is publickey first, falling back to the tutorial account's password, so a machine you never
ran ssh-copy-id against still works.

Standard library only, on purpose: clone the repo and run it, no venv, no pip.

    ./admin/fleet.py sync                     # every machine in the inventory
    ./admin/fleet.py sync bf3-ulisbon-1       # just these
    ./admin/fleet.py sync --force             # discard local modifications on the targets
    ./admin/fleet.py sync-participants        # install the participant repo, minus its .git
    ./admin/fleet.py doca                     # which DOCA version is each machine running
    ./admin/fleet.py firmware                 # which NIC firmware is each machine running
    ./admin/fleet.py deps                     # what is each machine missing to build the exercises
    ./admin/fleet.py deps --install           # install it
    ./admin/fleet.py pcc-ready                # are the PCC firmware knobs live, staged, or unset
    ./admin/fleet.py links                    # port PCIe names, link state, speed, loopback mode
    ./admin/fleet.py test-tutorial HOST       # run both exercises end to end and report pass/fail
    ./admin/fleet.py test-path-steering HOST  # run the dual-receiver steering experiment
    ./admin/fleet.py -h
"""

import argparse
import concurrent.futures
import contextlib
import dataclasses
import os
import pathlib
import shlex
import subprocess
import sys
import tempfile
import time
from collections.abc import Iterator

ADMIN_DIR = pathlib.Path(__file__).resolve().parent
INVENTORY = ADMIN_DIR / "machines.txt"
RESULTS_DIR = ADMIN_DIR / "results"
# The target-side scripts live in their own directory: they are a different kind of thing to
# this file — each runs on a machine, by hand or over ssh, and knows nothing about the fleet.
SCRIPTS_DIR = ADMIN_DIR / "local_scripts"

# Cloned over HTTPS rather than SSH: the repo is public, and putting deploy keys on a dozen
# shared lab boxes with a published password would be a bad trade.
REPO_URL = "https://github.com/fchamicapereira/sigcomm26-tutorial-bluefield.git"
TUTORIAL_USER = "s26t"

# What the participants actually work in: the cut-down tree that
# admin/update_participants_repo_on_github.py generates and pushes. Separate repo, separate verb
# (sync-participants), and installed differently -- as a plain directory with .git deleted, because
# its history holds every earlier state of the exercise. Same HTTPS reasoning as above.
PARTICIPANTS_REPO_URL = "https://github.com/fchamicapereira/sigcomm26-tutorial-bluefield-participants.git"

# Where sync_participants.sh puts it, repeated here only so the --force warning can name the exact
# directory it is about to delete -- spelled as a real path, because someone reading that warning
# may well want to go and look at it before answering. Derived the same way the script does
# (/home/$TUSER/<repo basename>); the script computes its own and remains the authority, so if the
# two ever disagree the cost is a misleading warning, not a wrong path on a machine.
PARTICIPANTS_DEST = f"/home/{TUTORIAL_USER}/" + PARTICIPANTS_REPO_URL.rsplit("/", 1)[-1].removesuffix(".git")

# Set by local_scripts/setup_tutorial_user.sh and handed to the participants anyway, so there is
# nothing here to protect: hardcoding it means a fresh laptop can drive a fresh fleet with no setup.
# Keys are still tried first, so machines where you ran ssh-copy-id never see this.
PASSWORD = "sigcomm26tutorial"

SSH_OPTS = [
    # No BatchMode: it would switch password auth off entirely, which is exactly the machines we
    # cannot afford to lose. One prompt only — a second one means the password is wrong, and
    # retrying it just walks the account into the sshd lockout.
    "-o",
    "NumberOfPasswordPrompts=1",
    "-o",
    "ConnectTimeout=10",
    # accept-new, not 'no': a machine whose key genuinely changed should still stop us, but a
    # first-time connection must not need an interactive prompt we cannot answer unattended.
    "-o",
    "StrictHostKeyChecking=accept-new",
]

DEFAULT_JOBS = 8
DEFAULT_TIMEOUT = 300
# apt on a machine that has none of this is minutes, not seconds, and every machine shares the
# uplink; the default would time out a first run.
INSTALL_TIMEOUT = 1800
# test-tutorial spends about a minute in deliberate sleeps (traffic warm-up, two averaging windows,
# the capture window, the post-PCC settle) plus setup, on top of building both exercises from
# scratch. Measured end to end at 100s on a DOCA 2.9 machine.
#
# The build is not the expensive part and does not need to be planned around: a from-scratch
# DOCA 3.4 build takes 6.5s (5.8s of meson setup, which is where build_device_code.sh runs dpacc
# over the DPA sources, and 0.7s of ninja) on a machine that was already busy. So this is not an
# estimate of anything — it is the point at which a run is declared hung and killed, and what it
# has to be large enough for is the sleeps, not the compiler.
TEST_TIMEOUT = 300


@dataclasses.dataclass(frozen=True)
class Machine:
    name: str
    user: str
    comment: str = ""


@dataclasses.dataclass
class Result:
    machine: Machine
    returncode: int | None  # None => timed out before ssh returned
    output: str
    data: dict[str, str]
    elapsed: float

    @property
    def status(self) -> str:
        if self.returncode is None:
            return "TIMEOUT"
        if self.returncode == 0:
            return "ok"
        if self.returncode == 255:
            return "UNREACHABLE"  # ssh itself failed: down, DNS, auth, host key
        return "FAIL"

    @property
    def ok(self) -> bool:
        return self.returncode == 0

    def note(self) -> str:
        """One line of context for the summary table."""
        # A script that emitted a subject has already said the useful thing, whether it succeeded
        # or not — a failing one has usually put its reason there.
        if subject := self.data.get("subject"):
            return subject
        if self.ok:
            return ""
        # Otherwise the last thing it said. Skip the '@@' lines: those are for parse_emitted, and
        # showing one raw in the table is noise, not context.
        for line in reversed(self.output.splitlines()):
            line = line.strip()
            if line and not line.startswith("@@"):
                return line
        return ""


def load_inventory(path: pathlib.Path, wanted: list[str]) -> list[Machine]:
    if not path.exists():
        sys.exit(f"inventory not found: {path}")

    machines: list[Machine] = []
    for raw in path.read_text().splitlines():
        body, _, comment = raw.partition("#")
        fields = body.split()
        if not fields:
            continue
        name = fields[0]
        user = fields[1] if len(fields) > 1 else TUTORIAL_USER
        machines.append(Machine(name, user, comment.strip()))

    if not wanted:
        return machines

    by_name = {m.name: m for m in machines}
    unknown = [n for n in wanted if n not in by_name]
    if unknown:
        sys.exit(f"not in {path.name}: {', '.join(unknown)}\n" f"known machines: {', '.join(by_name) or '(none)'}")
    return [by_name[n] for n in wanted]


def parse_emitted(stdout: str) -> dict[str, str]:
    """Collect the '@@key=value' lines the target-side scripts print."""
    data: dict[str, str] = {}
    for line in stdout.splitlines():
        if line.startswith("@@") and "=" in line:
            key, _, value = line[2:].partition("=")
            data[key.strip()] = value.strip()
    return data


@contextlib.contextmanager
def ssh_environment() -> Iterator[dict[str, str]]:
    """Yield the environment ssh runs under, able to answer a password prompt unattended.

    ssh only ever takes a password from a terminal or from an SSH_ASKPASS helper — never from
    stdin, which here is the script we are piping in. So write a throwaway helper into a private
    directory and point ssh at it; SSH_ASKPASS_REQUIRE=force makes ssh use it even though we do
    have a terminal. Publickey auth is untouched and still tried first.
    """
    with tempfile.TemporaryDirectory(prefix="fleet-askpass-") as tmp:
        secret = pathlib.Path(tmp, "password")
        secret.write_text(PASSWORD)
        secret.chmod(0o600)

        # A local key's passphrase must not be answered with the fleet password: refuse anything
        # that is not a password prompt and let ssh move on to the next auth method.
        helper = pathlib.Path(tmp, "askpass")
        helper.write_text("#!/bin/sh\n" 'case "$1" in *[Pp]assphrase*) exit 1 ;; esac\n' f"cat -- {shlex.quote(str(secret))}\n")
        helper.chmod(0o700)

        env = os.environ.copy()
        env["SSH_ASKPASS"] = str(helper)
        env["SSH_ASKPASS_REQUIRE"] = "force"
        yield env


def run_script(
    machine: Machine,
    script: pathlib.Path,
    script_args: list[str],
    timeout: int,
    dry_run: bool,
    env: dict[str, str],
) -> Result:
    cmd = [
        "ssh",
        *SSH_OPTS,
        f"{machine.user}@{machine.name}",
        "bash",
        "-s",
        "--",
        *[shlex.quote(a) for a in script_args],
    ]

    if dry_run:
        print(f"{machine.name}: {' '.join(cmd)} < {script.name}")
        return Result(machine, 0, "", {}, 0.0)

    started = time.monotonic()
    try:
        proc = subprocess.run(
            cmd,
            input=script.read_text(),
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env,
        )
        returncode: int | None = proc.returncode
        stdout, stderr = proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as exc:
        returncode = None
        stdout = exc.stdout.decode() if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        stderr = exc.stderr.decode() if isinstance(exc.stderr, bytes) else (exc.stderr or "")
        stderr += f"\ntimed out after {timeout}s"

    elapsed = time.monotonic() - started
    output = stdout + stderr

    # Per script, not just per machine: a verb that makes several passes would otherwise leave
    # only the last one's log behind.
    RESULTS_DIR.mkdir(exist_ok=True)
    (RESULTS_DIR / f"{machine.name}.{script.stem}.log").write_text(output)

    return Result(machine, returncode, output, parse_emitted(stdout), elapsed)


def run_fleet(
    machines: list[Machine],
    script: pathlib.Path,
    script_args: list[str],
    jobs: int,
    timeout: int,
    dry_run: bool,
) -> list[Result]:
    if not script.exists():
        sys.exit(f"target-side script not found: {script}")

    results: list[Result] = []
    with ssh_environment() as env, concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        futures = {pool.submit(run_script, m, script, script_args, timeout, dry_run, env): m for m in machines}
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            results.append(result)
            if not dry_run:
                print(f"  {result.status:<12} {result.machine.name}  ({result.elapsed:.1f}s)")

    order = {m.name: i for i, m in enumerate(machines)}
    results.sort(key=lambda r: order[r.machine.name])
    return results


def render_table(results: list[Result], columns: list[tuple[str, str]]) -> None:
    """columns: (header, key) pairs; key is looked up in Result.data, plus the specials below."""

    def cell(result: Result, key: str) -> str:
        if key == "@host":
            return result.machine.name
        if key == "@status":
            return result.status
        if key == "@note":
            return result.note()
        return result.data.get(key, "-")

    headers = [header for header, _ in columns]
    rows = [[cell(r, key) for _, key in columns] for r in results]
    widths = [max(len(headers[i]), *(len(row[i]) for row in rows)) if rows else len(headers[i]) for i in range(len(columns))]

    def line(cells: list[str]) -> str:
        return "  ".join(c.ljust(w) for c, w in zip(cells, widths)).rstrip()

    print()
    print(line(headers))
    print(line(["-" * w for w in widths]))
    for row in rows:
        print(line(row))


def warn_if_mixed(results: list[Result], key: str, label: str) -> None:
    """Say so when the fleet does not agree on a value the exercises depend on.

    A column of differing strings is easy to skim past; a fleet that is not uniform on firmware or
    DOCA cannot be handed one set of instructions, which is worth a sentence of its own.
    """
    values: dict[str, list[str]] = {}
    for result in results:
        if not result.ok:
            continue
        values.setdefault(result.data.get(key, "-"), []).append(result.machine.name)
    if len(values) <= 1:
        return
    print(f"\nnote: the fleet is on mixed {label} —")
    for value, hosts in sorted(values.items()):
        print(f"  {value}: {', '.join(hosts)}")


def summarize(results: list[Result]) -> int:
    failed = [r for r in results if not r.ok]
    print()
    if failed:
        print(f"{len(results) - len(failed)}/{len(results)} ok — " f"failures: {', '.join(r.machine.name for r in failed)}")
        print(f"logs: {RESULTS_DIR}/<machine>.<script>.log")
        return 1
    print(f"{len(results)}/{len(results)} ok")
    return 0


def _ansi(code: str) -> str:
    """An ANSI escape, or nothing when stdout is not a terminal or NO_COLOR is set."""
    if not sys.stdout.isatty() or os.environ.get("NO_COLOR"):
        return ""
    return f"\033[{code}m"


def confirm_destructive(headline: str, detail: list[str], machines: list[Machine], phrase: str = "yes, I understand") -> bool:
    """Print a warning in red and make the operator type `phrase` out in full before going on.

    Deliberately not a y/n prompt, and deliberately a sentence rather than a word. This exists for
    an action that destroys work nobody can get back, and a reflexive keystroke — or a reflexive
    "yes" — is the thing it is guarding against. Typing a phrase takes long enough to notice what
    the screen says.

    Matching is case-insensitive and does not care how much whitespace is between the words: the
    point is that the operator typed a sentence, not that they typed it perfectly.

    There is no way past this. With no terminal to ask at it refuses, rather than falling back to
    a flag that would mean consent: a --yes would end up in somebody's shell history or a wrapper
    script, and then the sentence guards nothing.
    """
    red, bold, off = _ansi("31"), _ansi("1"), _ansi("0")

    print(f"\n{red}{bold}!!! {headline}{off}")
    for line in detail:
        print(f"{red}    {line}{off}" if line else "")
    print(f"\n    {len(machines)} machine(s): {', '.join(m.name for m in machines)}")

    if not sys.stdin.isatty():
        print(f"\n{red}No terminal to confirm at, and there is no flag that skips this.{off}")
        print(f"{red}Re-run it by hand and type the sentence.{off}")
        return False

    try:
        answer = input(f"\nType {bold}{phrase}{off} to continue, anything else to abort: ")
    except (EOFError, KeyboardInterrupt):
        print()
        return False
    return " ".join(answer.lower().split()) == " ".join(phrase.lower().split())

def current_checkout_branch() -> str:
    result = subprocess.run(
        ["git", "-C", str(ADMIN_DIR.parent), "symbolic-ref", "--quiet", "--short", "HEAD"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0 or not result.stdout.strip():
        sys.exit("the controller checkout has a detached HEAD; pass sync --branch explicitly")
    return result.stdout.strip()


def cmd_sync(args: argparse.Namespace) -> int:
    machines = load_inventory(INVENTORY, args.machines)
    branch = args.branch or current_checkout_branch()
    script_args = ["--repo", REPO_URL, "--branch", branch, "--user", TUTORIAL_USER]
    if args.force:
        script_args.append("--force")

    print(f"sync: {branch} -> {len(machines)} machine(s)" f"{' (--force: local changes will be discarded)' if args.force else ''}")

    results = run_fleet(
        machines,
        SCRIPTS_DIR / "sync.sh",
        script_args,
        args.jobs,
        args.timeout or DEFAULT_TIMEOUT,
        args.dry_run,
    )
    if args.dry_run:
        return 0

    render_table(
        results,
        [
            ("HOST", "@host"),
            ("STATUS", "@status"),
            ("ACTION", "action"),
            ("SHA", "sha"),
            ("BRANCH", "branch"),
            ("NOTE", "@note"),
        ],
    )
    return summarize(results)


def cmd_sync_participants(args: argparse.Namespace) -> int:
    machines = load_inventory(INVENTORY, args.machines)
    script_args = ["--repo", PARTICIPANTS_REPO_URL, "--branch", BRANCH, "--user", TUTORIAL_USER]
    if args.force:
        script_args.append("--force")

    # Without --force the script refuses any destination that already exists, so the only way to
    # lose someone's work is here. --dry-run connects to nothing, so there is nothing to confirm.
    if args.force and not args.dry_run:
        if not confirm_destructive(
            "sync-participants --force DELETES the participant repo on every machine below.",
            [
                f"On each one {PARTICIPANTS_DEST} is removed and replaced by a fresh clone.",
                "Everything a participant has done in that tree goes with it: edited exercises,",
                "build directories, captures, notes. There is no backup and no undo.",
                "",
                "If the session is running, assume somebody is mid-exercise on these machines",
                "and will lose it. Without --force nothing here is touched at all.",
            ],
            machines,
        ):
            print("aborted — nothing was touched")
            return 1

    print(
        f"sync-participants: {BRANCH} -> {len(machines)} machine(s)"
        f"{' (--force: whatever is in the destination will be DELETED)' if args.force else ''}"
    )

    results = run_fleet(
        machines,
        SCRIPTS_DIR / "sync_participants.sh",
        script_args,
        args.jobs,
        args.timeout or DEFAULT_TIMEOUT,
        args.dry_run,
    )
    if args.dry_run:
        return 0

    render_table(
        results,
        [
            ("HOST", "@host"),
            ("STATUS", "@status"),
            ("ACTION", "action"),
            ("SHA", "sha"),
            ("GIT REMOVED", "git_removed"),
            ("NOTE", "@note"),
        ],
    )
    return summarize(results)


def cmd_doca(args: argparse.Namespace) -> int:
    machines = load_inventory(INVENTORY, args.machines)

    print(f"doca: {len(machines)} machine(s)")

    results = run_fleet(
        machines,
        SCRIPTS_DIR / "print_doca_version.sh",
        ["--emit"],
        args.jobs,
        args.timeout or DEFAULT_TIMEOUT,
        args.dry_run,
    )
    if args.dry_run:
        return 0

    render_table(
        results,
        [
            ("HOST", "@host"),
            ("STATUS", "@status"),
            ("DOCA", "doca"),
            ("RAW", "raw"),
            ("SOURCE", "source"),
            ("NOTE", "@note"),
        ],
    )

    return summarize(results)


def cmd_pcc_ready(args: argparse.Namespace) -> int:
    machines = load_inventory(INVENTORY, args.machines)

    print(f"pcc-ready: {len(machines)} machine(s)")

    results = run_fleet(
        machines,
        SCRIPTS_DIR / "check_pcc_ready.sh",
        ["--emit"],
        args.jobs,
        args.timeout or DEFAULT_TIMEOUT,
        args.dry_run,
    )
    if args.dry_run:
        return 0

    render_table(
        results,
        [
            ("HOST", "@host"),
            ("STATUS", "@status"),
            ("USER_PROG_CC", "upcc"),
            ("DPA_AUTH", "dpa"),
            ("VERDICT", "verdict"),
            ("NOTE", "@note"),
        ],
    )

    # The two verdicts that are not 'ready' cost very different amounts of work, so they are
    # called out separately rather than lumped into one "not ready" count.
    for verdict, advice in (
        ("power-cycle", "already staged — power off, wait ~30s, power on (a reboot will not do)"),
        ("configure", "needs `mlxconfig set` first, then the power-cycle"),
    ):
        hosts = [r.machine.name for r in results if r.data.get("verdict") == verdict]
        if hosts:
            print(f"\n{verdict}: {', '.join(hosts)}\n  {advice}")

    return summarize(results)


def cmd_links(args: argparse.Namespace) -> int:
    machines = load_inventory(INVENTORY, args.machines)

    print(f"links: {len(machines)} machine(s)")

    results = run_fleet(
        machines,
        SCRIPTS_DIR / "print_link_status.sh",
        ["--emit"],
        args.jobs,
        args.timeout or DEFAULT_TIMEOUT,
        args.dry_run,
    )
    if args.dry_run:
        return 0

    render_table(
        results,
        [
            ("HOST", "@host"),
            ("STATUS", "@status"),
            ("PCIE", "pcie"),
            ("MST (/dev/mst/)", "mst"),
            ("LINKS", "summary"),
            ("LOOPBACK", "loopback"),
            ("NOTE", "@note"),
        ],
    )

    # A port in loopback passes every other check — ip link, ethtool and the counters all look
    # healthy — while nothing it sends reaches the wire. Say it out loud rather than leave it in
    # a column somebody skims past.
    looped = [f"{r.machine.name} ({r.data['loopback']})" for r in results if r.ok and r.data.get("loopback", "-") != "-"]
    if looped:
        print(f"\nnote: loopback is ACTIVE on {', '.join(looped)} — those ports are not on the wire")

    # The mst names are derived from the PCI device id, so they are reported even where the mst
    # kernel modules were never loaded — in which case the paths do not exist yet. Worth saying,
    # since `mlxconfig -d /dev/mst/...` would then fail on exactly the machines listed here.
    not_started = [r.machine.name for r in results if r.data.get("mst_started") == "no"]
    if not_started:
        print(f"\nnote: mst is not started on {', '.join(not_started)} — the MST names there are " f"derived from the PCI device id, and /dev/mst/ does not exist until `sudo mst start`")

    return summarize(results)


def cmd_deps(args: argparse.Namespace) -> int:
    machines = load_inventory(INVENTORY, args.machines)
    script_args = ["--emit"] + (["--install"] if args.install else [])

    print(f"deps: {len(machines)} machine(s)" f"{' (installing)' if args.install else ' (report only — pass --install to act)'}")

    results = run_fleet(
        machines,
        SCRIPTS_DIR / "install_deps.sh",
        script_args,
        args.jobs,
        args.timeout or INSTALL_TIMEOUT,
        args.dry_run,
    )
    if args.dry_run:
        return 0

    render_table(
        results,
        [
            ("HOST", "@host"),
            ("STATUS", "@status"),
            ("PRESENT", "present"),
            ("INSTALLED", "added") if args.install else ("MISSING", "needed"),
            ("MISSING TOOLS", "tools"),
            ("NOTE", "@note"),
        ],
    )

    # DOCA, dpacc, MFT and pyverbs are not ours to install; a machine short of one of them is
    # short of an exercise, so it has to be said out loud rather than left in a column.
    short = [r.machine.name for r in results if r.ok and r.data.get("tools", "-") != "-"]
    if short:
        print(f"\nnote: DOCA/BSP tooling is missing on {', '.join(short)} — see the MISSING " f"TOOLS column; those come with the DOCA install, not from apt")

    return summarize(results)


def cmd_firmware(args: argparse.Namespace) -> int:
    machines = load_inventory(INVENTORY, args.machines)

    print(f"firmware: {len(machines)} machine(s)")

    results = run_fleet(
        machines,
        SCRIPTS_DIR / "print_firmware_version.sh",
        ["--emit"],
        args.jobs,
        args.timeout or DEFAULT_TIMEOUT,
        args.dry_run,
    )
    if args.dry_run:
        return 0

    render_table(
        results,
        [
            ("HOST", "@host"),
            ("STATUS", "@status"),
            ("FIRMWARE", "fw"),
            ("DEVICE", "fw_device"),
            ("SOURCE", "fw_source"),
            ("NOTE", "@note"),
        ],
    )
    warn_if_mixed(results, "fw", "firmware")
    return summarize(results)


def cmd_test_tutorial(args: argparse.Namespace) -> int:
    machines = load_inventory(INVENTORY, args.machines)

    # This verb deletes every OVS bridge and SF on the target and drives its ports at line rate.
    # Naming a machine is consent; a bare `test-tutorial` doing that to the whole fleet is not.
    if not args.machines and not args.whole_fleet:
        sys.exit(
            "test-tutorial is destructive: it wipes every OVS bridge and SF on the target and\n"
            "kills any traffic running there. Name the machines to test, or pass --whole-fleet to\n"
            f"run it on all {len(machines)} machines in {INVENTORY.name}."
        )

    script_args = ["--emit"]
    if args.skip_build:
        script_args.append("--skip-build")
    if args.skip_pcc:
        script_args.append("--skip-pcc")
    if args.force:
        script_args.append("--force")

    # "at most", explicitly: an earlier version quoted the timeout on its own and read as though
    # every machine took that long, when the measured run is about two minutes.
    print(f"test-tutorial: {len(machines)} machine(s) — destructive; about 2 min each, at most " f"{TEST_TIMEOUT // 60} min before a machine is given up on")

    results = run_fleet(
        machines,
        SCRIPTS_DIR / "test_tutorial.sh",
        script_args,
        args.jobs,
        args.timeout or TEST_TIMEOUT,
        args.dry_run,
    )
    if args.dry_run:
        return 0

    render_table(
        results,
        [
            ("HOST", "@host"),
            ("STATUS", "@status"),
            ("DOCA", "doca"),
            ("BUILDS", "version"),
            ("CE/IPv4", "ecn"),
            ("BW BASE", "bw_base"),
            ("BW PCC", "bw_pcc"),
            ("PCC", "pcc"),
            ("VERDICT", "verdict"),
            ("NOTE", "@note"),
        ],
    )

    # The request's "if there's no available version for that host, say so": a machine on a DOCA
    # release no doca-*/ directory targets is not a broken machine, and it is not something the
    # next `sync` fixes either — somebody has to port the exercise or reimage the box.
    unsupported = [(r.machine.name, r.data.get("doca", "?")) for r in results if r.data.get("verdict") == "unsupported"]
    if unsupported:
        print("\nno tutorial version for:")
        for host, doca in unsupported:
            print(f"  {host} (DOCA {doca}) — nothing was changed there")
        print("  the mapping lives in local_scripts/test_tutorial.sh; doca-2/ covers every 2.x, " "doca-3/ every 3.x")

    # Somebody else's experiment was running there. These are shared lab machines, and the test
    # would have deleted their OVS bridges and SFs to make room for its own.
    busy = [r.machine.name for r in results if r.data.get("verdict") == "busy"]
    if busy:
        print(f"\nin use, not touched: {', '.join(busy)}")
        print("  another DOCA/DPDK workload is running there — see the log for what, and check " "with whoever owns it before passing --force")

    skipped = [(r.machine.name, r.data.get("pcc_skip_reason", "?")) for r in results if r.data.get("pcc") == "skipped"]
    if skipped and not args.skip_pcc:
        print("\npcc half not run on:")
        for host, reason in skipped:
            print(f"  {host} — {reason}")
        print("  see `pcc-ready` for what each machine needs")

    return summarize(results)


def cmd_test_path_steering(args: argparse.Namespace) -> int:
    machines = load_inventory(INVENTORY, args.machines)

    if not args.machines and not args.whole_fleet:
        sys.exit(
            "test-path-steering is destructive: it wipes every OVS bridge and SF on the target.\n"
            "Name the machines to test, or pass --whole-fleet to run it everywhere."
        )

    script_args = ["--emit"]
    if args.skip_build:
        script_args.append("--skip-build")
    if args.skip_setup:
        script_args.append("--skip-setup")
    if args.force:
        script_args.append("--force")

    mode = "reusing existing topology" if args.skip_setup else "destructive topology setup"
    print(
        f"test-path-steering: {len(machines)} machine(s) — {mode}; "
        f"at most {TEST_TIMEOUT // 60} min before a machine is given up on"
    )
    results = run_fleet(
        machines,
        SCRIPTS_DIR / "test_path_steering.sh",
        script_args,
        args.jobs,
        args.timeout or TEST_TIMEOUT,
        args.dry_run,
    )
    if args.dry_run:
        return 0

    render_table(
        results,
        [
            ("HOST", "@host"),
            ("STATUS", "@status"),
            ("DOCA", "doca"),
            ("FLOW0 GBPS", "path0_bw"),
            ("FLOW1 GBPS", "path1_bw"),
            ("FLOW0/FLOW1", "flow_ratio"),
            ("STEER P0/P1", "path_ratio"),
            ("CE/Gb P1/P0", "ce_ratio"),
            ("QPN MAP", "qpn_map"),
            ("PATH0/64", "applied_path0"),
            ("VERDICT", "verdict"),
            ("NOTE", "@note"),
        ],
    )

    unsupported = [
        (r.machine.name, r.data.get("doca", "?"))
        for r in results
        if r.data.get("verdict") == "unsupported"
    ]
    if unsupported:
        print("\npath steering is not ported for:")
        for host, doca in unsupported:
            print(f"  {host} (DOCA {doca})")

    busy = [r.machine.name for r in results if r.data.get("verdict") == "busy"]
    if busy:
        print(f"\nin use, not touched: {', '.join(busy)}")

    return summarize(results)


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="admin/fleet.py",
        description="Drive the SIGCOMM'26 BlueField-3 tutorial fleet.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"inventory: {INVENTORY}",
    )
    subparsers = parser.add_subparsers(dest="verb", required=True, metavar="VERB")

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument(
        "machines",
        nargs="*",
        metavar="MACHINE",
        help="machines to act on (default: every machine in the inventory)",
    )
    common.add_argument("-j", "--jobs", type=int, default=DEFAULT_JOBS, help=f"machines to work on in parallel (default: {DEFAULT_JOBS})")
    # Listed from the constants rather than spelled out, so this cannot go stale the next time one
    # of them moves — and so a verb whose timeout is just the default is not named here at all.
    overrides = ", ".join(f"{value} for {verb}" for verb, value in (("deps", INSTALL_TIMEOUT), ("test-tutorial", TEST_TIMEOUT)) if value != DEFAULT_TIMEOUT)
    common.add_argument(
        "--timeout",
        type=int,
        help=f"per-machine timeout in seconds (default: {DEFAULT_TIMEOUT}{', ' + overrides if overrides else ''})",
    )
    common.add_argument("--dry-run", action="store_true", help="print what would run, connect to nothing")

    sync = subparsers.add_parser(
        "sync",
        parents=[common],
        help="clone or update the tutorial repo on each machine",
        description=(
            f"Clone {REPO_URL} into ~{TUTORIAL_USER}, or fast-forward it if already present. "
            "Refuses to touch a tree with local modifications or on the wrong branch, since "
            "that is usually somebody's exercise work; --force discards it."
        ),
    )
    sync.add_argument(
        "--branch",
        help="branch to check out on targets (default: this controller checkout's current branch)",
    )
    sync.add_argument("--force", action="store_true", help="reset --hard to the selected origin branch, discarding local changes")
    sync.set_defaults(func=cmd_sync)

    sync_participants = subparsers.add_parser(
        "sync-participants",
        parents=[common],
        help="install the participant repo on each machine, without its git history",
        description=(
            f"Clone {PARTICIPANTS_REPO_URL} into ~{TUTORIAL_USER} and then DELETE its .git "
            "directory, so what a participant finds is a plain directory of files. That is the "
            "point of having a separate verb: the participant repo is regenerated by "
            "admin/update_participants_repo_on_github.py, so its history contains every earlier "
            "exercise -- including the period when solutions shipped alongside it -- and a "
            "checkout would hand all of it to anyone who ran `git log -p`. This removes it FROM "
            "THE MACHINE only; if that repo is public, the same history is one clone away from "
            "any laptop, and rewriting the remote is the only thing that changes. "
            "Because .git is gone there is no way to update the tree in place, only to replace "
            "it. So a destination that already exists is LEFT ALONE and reported as a failure for "
            "that machine -- the default can never destroy anything. --force is the only way to "
            "replace it, and it deletes whatever was there including a participant's work, so it "
            "stops and makes you type 'yes, I understand' in full first. There is no flag that "
            "skips that prompt and it cannot run unattended. Use the 'sync' verb for OUR repo, "
            "which stays a real checkout and can be fast-forwarded."
        ),
    )
    sync_participants.add_argument(
        "--force",
        action="store_true",
        help="replace the destination if it already exists, deleting whatever is in it (including a participant's work); asks you to confirm in full first",
    )
    sync_participants.set_defaults(func=cmd_sync_participants)

    doca = subparsers.add_parser(
        "doca",
        parents=[common],
        help="report the DOCA version installed on each machine",
        description=(
            "Run admin/local_scripts/print_doca_version.sh on each machine and tabulate the "
            "result. The MAJOR.MINOR version decides which doca-*/ directory a machine builds "
            "from (one directory may serve several minors), so "
            "this is the check for whether the fleet can be handed the same instructions. RAW "
            "and SOURCE show the string the version came from and what reported it."
        ),
    )
    doca.set_defaults(func=cmd_doca)

    firmware = subparsers.add_parser(
        "firmware",
        parents=[common],
        help="report the NIC firmware version of each machine",
        description=(
            "Run admin/local_scripts/print_firmware_version.sh on each machine and tabulate the "
            "result. This is the ConnectX/BlueField firmware — what mlxlink calls 'Firmware "
            "Version', not the "
            "BSP version from bfver. The DPA and PCC exercises depend on firmware NV-config "
            "knobs and on behaviour that differs between builds, so a fleet on mixed firmware "
            "will not behave uniformly."
        ),
    )
    firmware.set_defaults(func=cmd_firmware)

    links = subparsers.add_parser(
        "links",
        parents=[common],
        help="report each BlueField port's device names, link state, speed and loopback mode",
        description=(
            "Run admin/local_scripts/print_link_status.sh on each machine and tabulate mlxlink's "
            "Operational Info per uplink port: PCIe address, /dev/mst device name, link state, "
            "speed and loopback mode. "
            "Loopback is called out separately because a looped port looks healthy to ip link, "
            "ethtool and the port counters while nothing it sends reaches the wire. Reads only, "
            "and never starts mst — mlxlink is addressed by PCI address."
        ),
    )
    links.set_defaults(func=cmd_links)

    deps = subparsers.add_parser(
        "deps",
        parents=[common],
        help="report (or with --install, install) the packages the tutorial needs",
        description=(
            "Run admin/local_scripts/install_deps.sh on each machine: the build toolchain and "
            "libraries the doca-*/Dockerfile apt blocks install, plus what this repo needs, but "
            "natively on the Arm instead of into a container. Reports by default and touches "
            "nothing; --install acts, and is idempotent — a machine that already has everything "
            "is left alone. DOCA, dpacc, MFT and pyverbs are verified and reported, never "
            "installed: they come from the DOCA/BSP install and the distro's versions conflict "
            "with it."
        ),
    )
    deps.add_argument("--install", action="store_true", help="install the missing packages (default: report only)")
    deps.set_defaults(func=cmd_deps)

    pcc_ready = subparsers.add_parser(
        "pcc-ready",
        parents=[common],
        help="check the firmware NV-config knobs the PCC exercise needs",
        description=(
            "Run admin/local_scripts/check_pcc_ready.sh on each machine: are "
            "USER_PROGRAMMABLE_CC=1 and DPA_AUTHENTICATION=0 live? What counts is mlxconfig's "
            "Current column — `mlxconfig "
            "set` only stages a value, and it goes live when the DPU loses power. The verdict "
            "says which of the three states a machine is in: 'ready', 'power-cycle' (staged, so "
            "only a full power-off is left — a reboot does not do it), or 'configure' (mlxconfig "
            "has to run first). A cell like 0→1 means Current is 0 and a power-cycle makes it 1. "
            "Reads only; it never stages or resets anything."
        ),
    )
    pcc_ready.set_defaults(func=cmd_pcc_ready)

    test_tutorial = subparsers.add_parser(
        "test-tutorial",
        parents=[common],
        help="run both exercises end to end on each machine and report pass/fail",
        description=(
            "Run admin/local_scripts/test_tutorial.sh on each machine: set up the RoCE loopback, "
            "build the doca-*/ directory that machine's DOCA release needs, drive traffic across "
            "the two ports, check that DOCA Flow CE-marks it and captures it to a pcap, then load "
            "the DOCA PCC controller and check the throughput collapses. Where the other verbs "
            "report what a machine is, this reports whether it works. "
            "DESTRUCTIVE: it deletes every OVS bridge and SF on the target (via "
            "setup_roce_loopback.sh) and kills any traffic running there, so going fleet-wide "
            "has to be spelled out with --whole-fleet rather than falling out of naming no "
            "machines. A machine on a DOCA release no directory targets is reported as "
            "unsupported and left untouched. Takes about two minutes per machine, most of it "
            "deliberate — traffic has to settle and then be averaged over a window, twice — and "
            f"is given up on after {TEST_TIMEOUT // 60} minutes."
        ),
    )
    test_tutorial.add_argument("--skip-build", action="store_true", help="reuse the existing build directory instead of rebuilding from scratch")
    test_tutorial.add_argument("--skip-pcc", action="store_true", help="run the DOCA Flow half only")
    test_tutorial.add_argument("--force", action="store_true", help="run even where another DOCA/DPDK workload is already using the machine")
    test_tutorial.add_argument(
        "--whole-fleet",
        action="store_true",
        help="run on every machine in the inventory; required, because this verb is destructive and will not go fleet-wide just because no machines were named",
    )
    test_tutorial.set_defaults(func=cmd_test_tutorial)

    test_path_steering = subparsers.add_parser(
        "test-path-steering",
        parents=[common],
        help="run the dual-receiver PCC path-steering experiment end to end",
        description=(
            "Run admin/local_scripts/test_path_steering.sh on each target: create PF0 sf0 and "
            "sf4 receivers plus a PF1 sf0 sender, build the pcc-path-steering submodule, start "
            "ingress and embedded-egress steering, run simultaneous RDMA-CM traffic to "
            "10.0.0.1 and 10.0.0.11, then verify QPN mapping, sustained bandwidth, the 2:1 ECN "
            "signal and a dynamically applied path share. DESTRUCTIVE: every OVS bridge and SF "
            "on the target is replaced, so a machine name or --whole-fleet is mandatory."
        ),
    )
    test_path_steering.add_argument(
        "--skip-build", action="store_true", help="reuse pcc-path-steering/build"
    )
    test_path_steering.add_argument(
        "--skip-setup",
        action="store_true",
        help="reuse and validate the existing SF/namespace topology",
    )
    test_path_steering.add_argument(
        "--force", action="store_true", help="ignore an already-running DOCA/DPDK workload"
    )
    test_path_steering.add_argument(
        "--whole-fleet",
        action="store_true",
        help="run on every inventory machine; required when no machine is named",
    )
    test_path_steering.set_defaults(func=cmd_test_path_steering)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
