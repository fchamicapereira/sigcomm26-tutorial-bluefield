#!/usr/bin/env python3
"""Rebuild the participant repository from this one.

Participants get a much smaller tree than this repo: the exercises, the guides, and the scripts that
drive traffic. Everything else here -- the fleet tooling, the Dockerfiles, the guide sources, the PCC
docs and tuning tools -- is ours and only gets in their way. So is doca_flow_nop: the flow exercise
ships with its own worked no-op forwarder inside the file being edited, so a second copy as a
separate program is redundant.

NO SOLUTIONS SHIP. Neither the finished flow program (doca_flow_solution.c, or the capture-capable
doca_flow_ecn_pcap.c, which contains the same three answers) nor the finished PCC controller
(rtt_template.c) is copied over. There are no doca-N-solutions/ directories any more; MANAGED still
lists them so that a checkout from before this change gets them deleted.

Two exercises ship: the DOCA Flow ECN/mirror pipeline (doca-flow/) and, for any version that has a
PCC template (device/algo/rtt_template_exercise.c), the DOCA PCC pure-ECN reaction-point controller
(doca-pcc-ecn/). A version without a PCC template ships flow only.

This script REGENERATES the managed parts of that repo; it does not merge. Every path it owns is
deleted and rewritten from this tree on each run, so editing those files in the participant repo is
pointless -- edit them here and re-run. Paths it does not own (a README written by hand over there,
say) are left alone.

Layout it produces:

    doca-2/                       flow exercise (from doca_flow_template.c) + pcc exercise (from
                                  doca-pcc-ecn/device/algo/rtt_template_exercise.c)
    doca-3/                       ditto for the DOCA 3 tree (pcc only if it has a template)
    tutorial-doca-flow.md         Part I hands-on guide (Markdown; flow filename rewritten)
    tutorial-doca-pcc.md          Part II hands-on guide (Markdown)
    guides/*.pdf                  the rendered guides only, no LaTeX (see GUIDES)
    scripts/                      run_server.sh, run_client.sh, benchmark.sh, check_ecn_bits...
    .vscode/                      IntelliSense paths for the DOCA/DPDK headers on the card
    .clang-format

The flow program is called doca_flow_ecn.c over there. "Template" is our word for it -- an artefact
of how the exercise is derived here, see admin/local_scripts/regen_templates.py -- and means nothing
to a participant, who simply has one program with three functions to write.

Usage (S=admin/update_participants_repo_on_github.py):

    $S                # populate the checkout, leave it uncommitted
    $S --dry-run      # say what would change, touch nothing
    $S --push         # commit and push to the participant remote

The checkout lives in .participants/ (gitignored) and is cloned on first use.

This is the LAPTOP end of the pipeline: it writes the participant repo on GitHub. The other end is
`admin/fleet.py sync-participants`, which installs that repo onto every card -- and deletes its
.git on the way, so nothing published here can be read back out of the history on a machine.
"""

import argparse
import filecmp
import pathlib
import re
import shutil
import subprocess
import sys

REMOTE = "git@github.com:fchamicapereira/sigcomm26-tutorial-bluefield-participants.git"

REPO = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_DEST = REPO / ".participants"

VERSIONS = ["doca-2", "doca-3"]

# Copied verbatim into scripts/, keeping the executable bit.
SCRIPTS = ["run_server.sh", "run_client.sh", "benchmark.sh", "check_ecn_bits_from_pcap.sh"]

# Rendered guides to hand over, by filename in guides/. The .md sources, template.tex, the logos
# and the figures stay here -- participants get the PDF and nothing else, which is why this is a
# list of built artefacts rather than a directory copy. Run `make -C guides` first: this script
# does NOT build them, and copying a stale PDF is silent.
GUIDES = ["tailscale.pdf", "doca-flow.pdf"]

# Participant-only files kept here as real files rather than generated inline, so they can be
# reviewed and edited directly. Mapped to their destination in the participant repo.
ASSETS = {"participants/c_cpp_properties.json": ".vscode/c_cpp_properties.json"}

# The interactive hands-on guides (GitHub-flavoured Markdown, with <details> collapsibles and links
# into the source). They ship to the participant repo ROOT, unrendered, so GitHub shows the
# collapsibles and the doca-2/... source links resolve. The value says whether to apply the flow
# rename: the flow guide points at doca_flow_template.c, which build_version() renames to
# doca_flow_ecn.c for participants, so its links must be rewritten to match (the #L anchors are
# unaffected -- same file, same lines, only the name changes). HTML author-comments are stripped.
TUTORIAL_GUIDES = {
    "tutorial-doca-flow.md": True,
    "tutorial-doca-pcc.md": False,
}

# Top-level directories this script owns. Anything else in the participant repo survives a run.
#
# The doca-N-solutions/ names are still listed although nothing writes them any more: MANAGED is
# what gets deleted before each rebuild, so keeping them here is what removes the solution
# directories an earlier sync left behind. Drop them once no participant checkout has them.
MANAGED = [f"{v}{suffix}" for v in VERSIONS for suffix in ("", "-solutions")] + [
    "guides",
    "scripts",
    ".vscode",
]
MANAGED_FILES = [".clang-format"]


def run(cmd, cwd=None, check=True):
    return subprocess.run(cmd, cwd=cwd, check=check, text=True, capture_output=True)


def app_list_meson(meson_src, include_pcc):
    """The top-level app_list. doca-flow is always an exercise; doca-pcc-ecn is kept only for a
    version that has a PCC template (device/algo/rtt_template_exercise.c) and dropped otherwise --
    the finished controller on its own is not a participant exercise."""
    both = "app_list = [\n\t'doca-flow',\n\t'doca-pcc-ecn',\n]"
    flow_only = "app_list = [\n\t'doca-flow',\n]"
    if both not in meson_src:
        sys.exit("meson.build: app_list is not in the expected shape -- adapt this script")
    return meson_src if include_pcc else meson_src.replace(both, flow_only)


def trim_pcc_targets(meson_src):
    """Keep only the participant's doca_pcc_ecn_rp build.

    The maintainer-only bits -- the separate dpacc build of rtt_template_exercise.c and the
    doca_pcc_ecn_rp_template executable it feeds -- are dropped. Participants build the exercise as
    the ordinary doca_pcc_ecn_rp: their rtt_template.c IS the exercise. Blocks are separated by blank
    lines; every block that mentions the exercise/template goes."""
    blocks = meson_src.split("\n\n")
    kept = [b for b in blocks if "pcc_ecn_rp_template" not in b and "exercise" not in b]
    if len(kept) == len(blocks):
        sys.exit("doca-pcc-ecn/meson.build: no exercise/template block to drop -- adapt this script")
    out = "\n\n".join(kept)
    return out if out.endswith("\n") else out + "\n"


# doca-pcc-ecn paths that never ship to participants: the maintainer-only write-ups and tuning
# tools, plus the two algo sources. rtt_template.c is the finished controller and is skipped for
# that reason alone; the exercise's rtt_template.c is copied separately, from the scaffold.
PCC_SKIP = {
    "device/algo/rtt_template.c",
    "device/algo/rtt_template_exercise.c",
    "README.md",
    "doca_pcc_guide.md",
    "doca_pcc_findings.md",
    "tune_ecn.py",
    "plot_ecn_sweep.py",
    "doca_pcc_ecn_sweep.pdf",
    "doca_pcc_ecn_sweep.png",
}


def has_pcc_exercise(version):
    return (REPO / version / "doca-pcc-ecn" / "device" / "algo" / "rtt_template_exercise.c").is_file()


def build_pcc(dest, version, changes, dry_run):
    """Populate one version's doca-pcc-ecn/ -- the exercise, from the scaffold, named
    device/algo/rtt_template.c so the build just works. Copies the rest of the tree (host/, device/,
    build_device_code.sh, deps) verbatim, minus PCC_SKIP, and trims the meson to the single
    doca_pcc_ecn_rp target."""
    src = REPO / version / "doca-pcc-ecn"
    out = dest / version / "doca-pcc-ecn"
    for f in sorted(p for p in src.rglob("*") if p.is_file()):
        rel = f.relative_to(src).as_posix()
        if rel in PCC_SKIP:
            continue
        if rel == "meson.build":
            write(out / rel, trim_pcc_targets(f.read_text()), changes, dry_run)
        else:
            copy(f, out / rel, changes, dry_run)
    scaffold = src / "device" / "algo" / "rtt_template_exercise.c"
    copy(scaffold, out / "device" / "algo" / "rtt_template.c", changes, dry_run)


# The participant's doca-flow/meson.build, written from here rather than trimmed out of ours.
#
# Ours carries four targets -- flow_nop, flow_ecn_pcap, flow_solution, flow_template -- and none of
# those names mean anything over there: the participant has ONE program, doca_flow_ecn.c, built from
# doca_flow_template.c. Deriving this by deleting blocks from ours was already fragile, and it would
# also have to rename the target. Writing it out is shorter and says what it means.
FLOW_MESON = """\
# doca_flow_compat.h shims a few DOCA 2.9 API renames (doca_flow_resource_query_entry,
# doca_flow_shared_resource_set_cfg) back onto their pre-2.9 names, so the same sources build
# unchanged against every DOCA release this repo targets. Force-included ahead of every .c file's
# own #include <doca_flow.h>; the header is a no-op on 2.9+ (it is guarded on DOCA_VERSION_*).
flow_c_args = base_c_args + ['-include', meson.current_source_dir() / 'doca_flow_compat.h']

# -Wno-unused-variable: the exercise declares every struct you need and leaves the DOCA calls that
# use them to you, so before you start there are around twenty variables that are declared and never
# read. -Wunused-function is deliberately left ON -- its "defined but not used" lines name exactly
# the functions you have not wired up yet.
executable(DOCA_PREFIX + 'flow_ecn',
\t['doca_flow_ecn.c'],
\tc_args : flow_c_args + ['-Wno-unused-variable'],
\tdependencies : app_dependencies,
\tinclude_directories : app_inc_dirs,
\tinstall: false)
"""

def flow_meson(meson_src):
    """The participant's doca-flow/meson.build. `meson_src` is ours, checked only to make sure the
    source this assumes still exists -- so renaming it here fails the sync instead of shipping a
    meson.build that names a file nobody has."""
    if "doca_flow_template.c" not in meson_src:
        sys.exit("doca-flow/meson.build no longer builds doca_flow_template.c -- adapt this script")
    return FLOW_MESON


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


def render_guide(text, flow_rename):
    """Prepare a tutorial guide for participants: drop the HTML author-comments (they are marked
    'delete before publishing') and, for the flow guide, rewrite the exercise filename so its source
    links resolve against the renamed participant file."""
    text = re.sub(r"<!--.*?-->", "", text, flags=re.DOTALL).lstrip("\n")
    if flow_rename:
        text = text.replace("doca_flow_template.c", "doca_flow_ecn.c")
        text = text.replace("doca_flow_solution.c", "doca_flow_ecn.c")
    return text


def build_version(dest, version, changes, dry_run):
    """Populate one doca-N/ directory."""
    src = REPO / version
    out = dest / version
    flow_src = src / "doca-flow"
    flow_out = out / "doca-flow"
    include_pcc = has_pcc_exercise(version)

    write(out / "meson.build",
          app_list_meson((src / "meson.build").read_text(), include_pcc), changes, dry_run)
    write(
        flow_out / "meson.build",
        flow_meson((flow_src / "meson.build").read_text()),
        changes,
        dry_run,
    )
    copy(flow_src / "dependencies" / "meson.build", flow_out / "dependencies" / "meson.build",
         changes, dry_run)
    copy(flow_src / "doca_flow_compat.h", flow_out / "doca_flow_compat.h", changes, dry_run)

    # The exercise. Called doca_flow_ecn.c over there: "template" is our word for it, and a
    # participant has just the one program.
    copy(flow_src / "doca_flow_template.c", flow_out / "doca_flow_ecn.c", changes, dry_run)

    if include_pcc:
        build_pcc(dest, version, changes, dry_run)


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
        build_version(dest, version, changes, args.dry_run)

    for guide in GUIDES:
        src = REPO / "guides" / guide
        if not src.exists():
            sys.exit(f"guides/{guide} has not been built -- run: make -C guides")
        copy(src, dest / "guides" / guide, changes, args.dry_run)
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
    for guide, flow_rename in TUTORIAL_GUIDES.items():
        write(dest / guide, render_guide((REPO / guide).read_text(), flow_rename),
              changes, args.dry_run)

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
