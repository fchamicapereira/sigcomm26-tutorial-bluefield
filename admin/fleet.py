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
    ./admin/fleet.py doca                     # which DOCA version is each machine running
    ./admin/fleet.py firmware                 # which NIC firmware is each machine running
    ./admin/fleet.py deps                     # what is each machine missing to build the exercises
    ./admin/fleet.py deps --install           # install it
    ./admin/fleet.py pcc-ready                # are the PCC firmware knobs live, staged, or unset
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
BRANCH = "main"
TUTORIAL_USER = "s26t"

# Set by local_scripts/setup_tutorial_user.sh and handed to the participants anyway, so there is
# nothing here to protect: hardcoding it means a fresh laptop can drive a fresh fleet with no setup.
# Keys are still tried first, so machines where you ran ssh-copy-id never see this.
PASSWORD = "sigcomm26tutorial"

SSH_OPTS = [
    # No BatchMode: it would switch password auth off entirely, which is exactly the machines we
    # cannot afford to lose. One prompt only — a second one means the password is wrong, and
    # retrying it just walks the account into the sshd lockout.
    "-o", "NumberOfPasswordPrompts=1",
    "-o", "ConnectTimeout=10",
    # accept-new, not 'no': a machine whose key genuinely changed should still stop us, but a
    # first-time connection must not need an interactive prompt we cannot answer unattended.
    "-o", "StrictHostKeyChecking=accept-new",
]

DEFAULT_JOBS = 8
DEFAULT_TIMEOUT = 300
# apt on a machine that has none of this is minutes, not seconds, and every machine shares the
# uplink; the default would time out a first run.
INSTALL_TIMEOUT = 1800


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
        if self.ok:
            return self.data.get("subject", "")
        for line in reversed(self.output.splitlines()):
            line = line.strip()
            if line:
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
        sys.exit(
            f"not in {path.name}: {', '.join(unknown)}\n"
            f"known machines: {', '.join(by_name) or '(none)'}"
        )
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
        helper.write_text(
            "#!/bin/sh\n"
            'case "$1" in *[Pp]assphrase*) exit 1 ;; esac\n'
            f"cat -- {shlex.quote(str(secret))}\n"
        )
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
        "ssh", *SSH_OPTS,
        f"{machine.user}@{machine.name}",
        "bash", "-s", "--",
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
    with ssh_environment() as env, \
            concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        futures = {
            pool.submit(run_script, m, script, script_args, timeout, dry_run, env): m
            for m in machines
        }
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
    widths = [
        max(len(headers[i]), *(len(row[i]) for row in rows)) if rows else len(headers[i])
        for i in range(len(columns))
    ]

    def line(cells: list[str]) -> str:
        return "  ".join(c.ljust(w) for c, w in zip(cells, widths)).rstrip()

    print()
    print(line(headers))
    print(line(["-" * w for w in widths]))
    for row in rows:
        print(line(row))


def summarize(results: list[Result]) -> int:
    failed = [r for r in results if not r.ok]
    print()
    if failed:
        print(f"{len(results) - len(failed)}/{len(results)} ok — "
              f"failures: {', '.join(r.machine.name for r in failed)}")
        print(f"logs: {RESULTS_DIR}/<machine>.<script>.log")
        return 1
    print(f"{len(results)}/{len(results)} ok")
    return 0


def cmd_sync(args: argparse.Namespace) -> int:
    machines = load_inventory(INVENTORY, args.machines)
    script_args = ["--repo", REPO_URL, "--branch", BRANCH, "--user", TUTORIAL_USER]
    if args.force:
        script_args.append("--force")

    print(f"sync: {BRANCH} -> {len(machines)} machine(s)"
          f"{' (--force: local changes will be discarded)' if args.force else ''}")

    results = run_fleet(
        machines, SCRIPTS_DIR / "sync.sh", script_args,
        args.jobs, args.timeout or DEFAULT_TIMEOUT, args.dry_run,
    )
    if args.dry_run:
        return 0

    render_table(results, [
        ("HOST", "@host"),
        ("STATUS", "@status"),
        ("ACTION", "action"),
        ("SHA", "sha"),
        ("BRANCH", "branch"),
        ("NOTE", "@note"),
    ])
    return summarize(results)


def warn_if_mixed(results: list[Result], key: str, label: str) -> None:
    """The fleet is only usable if one set of instructions fits all of it — say so when it isn't."""
    seen = {r.data[key] for r in results if r.ok and key in r.data}
    if len(seen) > 1:
        print(f"\nnote: {label} is not uniform across the fleet — {', '.join(sorted(seen))}")


def cmd_doca(args: argparse.Namespace) -> int:
    machines = load_inventory(INVENTORY, args.machines)

    print(f"doca: {len(machines)} machine(s)")

    results = run_fleet(
        machines, SCRIPTS_DIR / "print_doca_version.sh", ["--emit"],
        args.jobs, args.timeout or DEFAULT_TIMEOUT, args.dry_run,
    )
    if args.dry_run:
        return 0

    render_table(results, [
        ("HOST", "@host"),
        ("STATUS", "@status"),
        ("DOCA", "doca"),
        ("RAW", "raw"),
        ("SOURCE", "source"),
        ("NOTE", "@note"),
    ])
    warn_if_mixed(results, "doca", "DOCA")
    return summarize(results)


def cmd_pcc_ready(args: argparse.Namespace) -> int:
    machines = load_inventory(INVENTORY, args.machines)

    print(f"pcc-ready: {len(machines)} machine(s)")

    results = run_fleet(
        machines, SCRIPTS_DIR / "check_pcc_ready.sh", ["--emit"],
        args.jobs, args.timeout or DEFAULT_TIMEOUT, args.dry_run,
    )
    if args.dry_run:
        return 0

    render_table(results, [
        ("HOST", "@host"),
        ("STATUS", "@status"),
        ("USER_PROG_CC", "upcc"),
        ("DPA_AUTH", "dpa"),
        ("VERDICT", "verdict"),
        ("NOTE", "@note"),
    ])

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


def cmd_deps(args: argparse.Namespace) -> int:
    machines = load_inventory(INVENTORY, args.machines)
    script_args = ["--emit"] + (["--install"] if args.install else [])

    print(f"deps: {len(machines)} machine(s)"
          f"{' (installing)' if args.install else ' (report only — pass --install to act)'}")

    results = run_fleet(
        machines, SCRIPTS_DIR / "install_deps.sh", script_args,
        args.jobs, args.timeout or INSTALL_TIMEOUT, args.dry_run,
    )
    if args.dry_run:
        return 0

    render_table(results, [
        ("HOST", "@host"),
        ("STATUS", "@status"),
        ("PRESENT", "present"),
        ("INSTALLED", "added") if args.install else ("MISSING", "needed"),
        ("MISSING TOOLS", "tools"),
        ("NOTE", "@note"),
    ])

    # DOCA, dpacc, MFT and pyverbs are not ours to install; a machine short of one of them is
    # short of an exercise, so it has to be said out loud rather than left in a column.
    short = [r.machine.name for r in results if r.ok and r.data.get("tools", "-") != "-"]
    if short:
        print(f"\nnote: DOCA/BSP tooling is missing on {', '.join(short)} — see the MISSING "
              f"TOOLS column; those come with the DOCA install, not from apt")

    return summarize(results)


def cmd_firmware(args: argparse.Namespace) -> int:
    machines = load_inventory(INVENTORY, args.machines)

    print(f"firmware: {len(machines)} machine(s)")

    results = run_fleet(
        machines, SCRIPTS_DIR / "print_firmware_version.sh", ["--emit"],
        args.jobs, args.timeout or DEFAULT_TIMEOUT, args.dry_run,
    )
    if args.dry_run:
        return 0

    render_table(results, [
        ("HOST", "@host"),
        ("STATUS", "@status"),
        ("FIRMWARE", "fw"),
        ("DEVICE", "fw_device"),
        ("SOURCE", "fw_source"),
        ("NOTE", "@note"),
    ])
    warn_if_mixed(results, "fw", "firmware")
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
        "machines", nargs="*", metavar="MACHINE",
        help="machines to act on (default: every machine in the inventory)",
    )
    common.add_argument("-j", "--jobs", type=int, default=DEFAULT_JOBS,
                        help=f"machines to work on in parallel (default: {DEFAULT_JOBS})")
    common.add_argument("--timeout", type=int,
                        help=f"per-machine timeout in seconds "
                             f"(default: {DEFAULT_TIMEOUT}, {INSTALL_TIMEOUT} for deps)")
    common.add_argument("--dry-run", action="store_true",
                        help="print what would run, connect to nothing")

    sync = subparsers.add_parser(
        "sync", parents=[common],
        help="clone or update the tutorial repo on each machine",
        description=(
            f"Clone {REPO_URL} into ~{TUTORIAL_USER}, or fast-forward it if already present. "
            "Refuses to touch a tree with local modifications or on the wrong branch, since "
            "that is usually somebody's exercise work; --force discards it."
        ),
    )
    sync.add_argument("--force", action="store_true",
                      help="reset --hard to origin/%s, discarding local changes" % BRANCH)
    sync.set_defaults(func=cmd_sync)

    doca = subparsers.add_parser(
        "doca", parents=[common],
        help="report the DOCA version installed on each machine",
        description=(
            "Run admin/local_scripts/print_doca_version.sh on each machine and tabulate the "
            "result. The MAJOR.MINOR version is the contract with the per-version source dirs, so "
            "this is the check for whether the fleet can be handed the same instructions. RAW "
            "and SOURCE show the string the version came from and what reported it."
        ),
    )
    doca.set_defaults(func=cmd_doca)

    firmware = subparsers.add_parser(
        "firmware", parents=[common],
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

    deps = subparsers.add_parser(
        "deps", parents=[common],
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
    deps.add_argument("--install", action="store_true",
                      help="install the missing packages (default: report only)")
    deps.set_defaults(func=cmd_deps)

    pcc_ready = subparsers.add_parser(
        "pcc-ready", parents=[common],
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

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
