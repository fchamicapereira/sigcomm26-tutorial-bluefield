#!/usr/bin/env python3
"""Rebuild the participant repository from this one.

Participants get a much smaller tree than this repo: the exercise, the solutions to check it
against, the guide, and the scripts that drive traffic. Everything else here -- the fleet tooling,
the PCC exercise, the nop programs, the Dockerfiles, the porting notes, the guide sources -- is ours
and only gets in their way.

This script REGENERATES the managed parts of that repo; it does not merge. Every path it owns is
deleted and rewritten from this tree on each run, so editing those files in the participant repo is
pointless -- edit them here and re-run. Paths it does not own (a README written by hand over there,
say) are left alone.

Layout it produces:

    doca-2/                       the exercise, built from doca-2/doca-flow/doca_flow_template.c
    doca-2-solutions/             the same program complete, from doca_flow_ecn_pcap.c
    doca-3/ doca-3-solutions/     ditto for the DOCA 3 tree
    guides/tailscale.pdf          the rendered guide only, no LaTeX
    scripts/                      run_server.sh, run_client.sh, benchmark.sh, check_ecn_bits...
    .vscode/                      IntelliSense paths for the DOCA/DPDK headers on the card
    .clang-format

In both the exercise and the solution directory the program is called doca_flow_ecn_pcap.c. The
"template" name is an artefact of how the exercise is derived here (see
docs/porting-doca-2-changes.md) and means nothing to a participant, who simply has a program with
five pipes to write.

Usage:
    admin/sync_participants.py                  # populate the checkout, leave it uncommitted
    admin/sync_participants.py --dry-run        # say what would change, touch nothing
    admin/sync_participants.py --push           # commit and push to the participant remote

The checkout lives in .participants/ (gitignored) and is cloned on first use.
"""

import argparse
import filecmp
import pathlib
import shutil
import subprocess
import sys

REMOTE = "git@github.com:fchamicapereira/sigcomm26-tutorial-bluefield-participants.git"

REPO = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_DEST = REPO / ".participants"

VERSIONS = ["doca-2", "doca-3"]

# Copied verbatim into scripts/, keeping the executable bit.
SCRIPTS = ["run_server.sh", "run_client.sh", "benchmark.sh", "check_ecn_bits_from_pcap.sh"]

# Participant-only files kept here as real files rather than generated inline, so they can be
# reviewed and edited directly. Mapped to their destination in the participant repo.
ASSETS = {"participants/c_cpp_properties.json": ".vscode/c_cpp_properties.json"}

# Top-level directories this script owns. Anything else in the participant repo survives a run.
MANAGED = [f"{v}{suffix}" for v in VERSIONS for suffix in ("", "-solutions")] + [
    "guides",
    "scripts",
    ".vscode",
]
MANAGED_FILES = [".clang-format"]


def run(cmd, cwd=None, check=True):
    return subprocess.run(cmd, cwd=cwd, check=check, text=True, capture_output=True)


def trim_app_list(meson_src):
    """Drop doca-pcc-ecn from the top-level app_list: the PCC exercise has no template yet."""
    old = "app_list = [\n\t'doca-flow',\n\t'doca-pcc-ecn',\n]"
    new = "app_list = [\n\t'doca-flow',\n]"
    if old not in meson_src:
        sys.exit("meson.build: app_list is not in the expected shape -- adapt this script")
    return meson_src.replace(old, new)


def trim_flow_targets(meson_src):
    """Keep only the flow_ecn_pcap executable.

    flow_nop is a scratch program of ours, and flow_template does not exist over there -- the
    exercise IS doca_flow_ecn_pcap.c. Blocks are separated by blank lines, and each executable()
    comes with its own comment block, so dropping whole blocks removes the comments with them.
    """
    blocks = meson_src.split("\n\n")
    kept = [b for b in blocks if "flow_nop" not in b and "flow_template" not in b]
    if len(kept) != len(blocks) - 2:
        sys.exit("doca-flow/meson.build: expected one flow_nop and one flow_template block")
    out = "\n\n".join(kept)
    # The dropped template block was last, taking the file's trailing newline with it.
    return out if out.endswith("\n") else out + "\n"


def write(path, data, changes, dry_run, executable=False):
    """Write bytes/str, recording whether it differed from what was there."""
    payload = data.encode() if isinstance(data, str) else data
    existed = path.exists()
    same = existed and path.read_bytes() == payload
    if not same:
        changes.append(("update" if existed else "add", path))
    if dry_run:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    if executable:
        path.chmod(0o755)


def copy(src, dst, changes, dry_run):
    existed = dst.exists()
    same = existed and filecmp.cmp(src, dst, shallow=False)
    if not same:
        changes.append(("update" if existed else "add", dst))
    if dry_run:
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def build_version(dest, version, solutions, changes, dry_run):
    """Populate one doca-N/ or doca-N-solutions/ directory."""
    src = REPO / version
    out = dest / (version + ("-solutions" if solutions else ""))
    flow_src = src / "doca-flow"
    flow_out = out / "doca-flow"

    write(out / "meson.build", trim_app_list((src / "meson.build").read_text()), changes, dry_run)
    write(
        flow_out / "meson.build",
        trim_flow_targets((flow_src / "meson.build").read_text()),
        changes,
        dry_run,
    )
    copy(flow_src / "dependencies" / "meson.build", flow_out / "dependencies" / "meson.build",
         changes, dry_run)
    copy(flow_src / "doca_flow_compat.h", flow_out / "doca_flow_compat.h", changes, dry_run)

    # The one file that differs between the two directories.
    program = "doca_flow_ecn_pcap.c" if solutions else "doca_flow_template.c"
    copy(flow_src / program, flow_out / "doca_flow_ecn_pcap.c", changes, dry_run)


def ensure_checkout(dest, dry_run):
    if (dest / ".git").is_dir():
        if not dry_run:
            run(["git", "fetch", "--prune", "origin"], cwd=dest, check=False)
        return
    if dry_run:
        print(f"would clone {REMOTE} into {dest}")
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"cloning {REMOTE} -> {dest}")
    run(["git", "clone", REMOTE, str(dest)])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dest", type=pathlib.Path, default=DEFAULT_DEST,
                    help=f"participant checkout (default: {DEFAULT_DEST})")
    ap.add_argument("--dry-run", action="store_true", help="report changes, write nothing")
    ap.add_argument("--commit", action="store_true", help="commit the result")
    ap.add_argument("--push", action="store_true", help="commit and push to the participant remote")
    ap.add_argument("-m", "--message", default=None, help="commit message")
    args = ap.parse_args()

    dest = args.dest.resolve()
    ensure_checkout(dest, args.dry_run)

    changes = []

    # Regenerate rather than merge: drop everything this script owns, then rebuild it. Otherwise a
    # file renamed or deleted here lingers over there forever.
    removed = []
    for name in MANAGED:
        path = dest / name
        if path.is_dir():
            removed.append(path)
            if not args.dry_run:
                shutil.rmtree(path)

    for version in VERSIONS:
        for solutions in (False, True):
            build_version(dest, version, solutions, changes, args.dry_run)

    copy(REPO / "guides" / "tailscale.pdf", dest / "guides" / "tailscale.pdf", changes,
         args.dry_run)
    for script in SCRIPTS:
        src = REPO / script
        dst = dest / "scripts" / script
        copy(src, dst, changes, args.dry_run)
        if not args.dry_run:
            dst.chmod(src.stat().st_mode)
    for name in MANAGED_FILES:
        copy(REPO / name, dest / name, changes, args.dry_run)
    for src_rel, dst_rel in ASSETS.items():
        copy(REPO / "admin" / src_rel, dest / dst_rel, changes, args.dry_run)

    for kind, path in changes:
        print(f"  {kind:6} {path.relative_to(dest)}")
    print(f"{len(changes)} file(s) written, {len(removed)} managed director(ies) rebuilt")

    if args.dry_run:
        return 0

    status = run(["git", "status", "--porcelain"], cwd=dest).stdout
    if not status.strip():
        print("participant repo is already up to date")
        return 0
    print("\n" + status.rstrip())

    if not (args.commit or args.push):
        print(f"\nnot committed. Review with: git -C {dest} diff")
        return 0

    sha = run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO).stdout.strip()
    message = args.message or f"Sync from tutorial repo at {sha}"
    run(["git", "add", "-A"], cwd=dest)
    run(["git", "commit", "-m", message], cwd=dest)
    print(f"committed: {message}")

    if args.push:
        # An empty remote has no branch to track yet; push the local branch by name either way.
        branch = run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=dest).stdout.strip()
        run(["git", "push", "-u", "origin", branch], cwd=dest)
        print(f"pushed {branch} to {REMOTE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
