#!/usr/bin/env python3
"""Re-derive every participant template from its solution.

    doca-N/doca-flow/doca_flow_solution.c   (solution, the source of truth)
        -> doca-N/doca-flow/doca_flow_template.c   (exercise)

Note the source: doca_flow_solution.c, NOT doca_flow_ecn_pcap.c. The pcap program is the fuller
one -- it adds a hardware copy of the traffic to a capture file, through a shared mirror on 2.x and
a flooding pipe on 3.x -- and none of that is part of the exercise. It is kept as a demo and is
maintained by hand; nothing here reads it.

The template differs from the solution in exactly two ways, and this script is the only thing that
should ever produce it -- hand-editing is how the two drift apart:

  1. Three functions keep their cfg boilerplate and teardown, but the two regions the participant
     writes -- building the pipe, and adding its entries -- are DELETED. Every struct the
     participant needs is still declared for them, and so is the `TODO Na` recipe that sits above
     it, so what is missing is the DOCA calls and the fields they take, not the scaffolding around
     them:

         TODO 1   create_root_pipe            sort wire traffic from SF traffic  (Step 4.1)
         TODO 2   create_forward_to_sf_pipe   forward, count, and CE-mark        (Step 4.2)
         TODO 3   create_sampling_pipe        mark 1 packet in N                 (Step 4.3)

     The regions are found by anchoring on the TODO marker and on a code line below it (see CUTS),
     and the end anchors are deliberately version-agnostic -- no port_meta/port_id, no
     add_entry-versus-basic_add_entry -- so one set covers both trees.

  2. `build_pipeline` is replaced with a version wired as a NO-OP FORWARDER: it calls
     create_root_pipe_nop() and nothing else, with the real pipeline present but commented out.
     Running the untouched template therefore forwards traffic at line rate, which is where Step 4
     starts. create_root_pipe_nop() itself is copied through verbatim, minus the
     __attribute__((unused)) the solution needs to keep -Wall quiet about it.

Everything else -- including `create_passthrough_pipe`, the worked example -- is identical, so
`diff` between the two files shows the exercise and nothing else.

THE TODO RECIPES LIVE IN THE SOLUTION. They are ordinary comments there, sitting directly above the
code that satisfies them, and are copied into the template like every other surviving line. That is
what keeps the two files honest: apart from `build_pipeline` this script ONLY deletes, so

    diff doca_flow_template.c doca_flow_solution.c

is pure additions -- exactly the code a participant has to write, with no reworded-comment churn to
read past. It also means each recipe and its answer are edited side by side, in one file, which is
the only arrangement that keeps them in step.

Three things follow, and they are the rules for editing a solution:

  - A TODO block must be ONE unbroken run of `//` lines, followed by a BLANK line, followed by the
    code it describes. The blank is the marker this script keys on: it deletes from there down to
    the region's end anchor. A TODO block run together with its code deletes nothing.
  - Anything between the TODO block and the end anchor is DROPPED from the template, comments
    included. That is the right home for prose that only makes sense once the code exists.
  - There are no per-tree placeholders. Each tree's solution spells out its own DOCA generation
    (port_meta vs port_id, add_entry vs basic_add_entry, the entry flags), because the recipe now
    lives in the file it describes rather than in one shared copy here.

Keep the TODO text short and point at the guide: Step 4 spells each one out in full, and
duplicating it in the source is a second copy to keep in step.

Usage:
    admin/local_scripts/regen_templates.py              # rewrite the templates
    admin/local_scripts/regen_templates.py --check      # verify they are up to date, change nothing

`--check` is the useful one in CI or before a release: it fails if a solution has moved on and its
template was not regenerated.
"""

import argparse
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent.parent

TREES = ["doca-2", "doca-3"]

# The exercise. One entry per deleted region: (function, TODO marker, last line to delete).
#
# The region runs from the blank line that closes the TODO block down to the end anchor INCLUSIVE.
# Both anchors are matched INSIDE the named function, so needles as common as `install_status` stay
# unambiguous, and the end anchor is searched from BELOW the TODO block -- which is what lets a
# recipe quote the very call that ends its region (`doca_flow_pipe_create(cfg,` appears in the text
# of TODO 1a) without matching itself.
#
# The recipes themselves live in the solutions, directly above the code they describe. Read the
# module docstring before moving or reflowing one -- the blank line under each block is load-bearing.
CUTS = [
    ("create_root_pipe", "TODO 1a", "doca_flow_pipe_create(cfg,"),
    ("create_root_pipe", "TODO 1b", '"PORT_DEMUX: install"'),
    ("create_forward_to_sf_pipe", "TODO 2a",
     "doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe)"),
    ("create_forward_to_sf_pipe", "TODO 2b", '"%s install", name)'),
    ("create_sampling_pipe", "TODO 3a", "doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe)"),
    ("create_sampling_pipe", "TODO 3b", '"RANDOM_SAMPLE: install"'),
]

# The no-op build_pipeline. Identical in both trees: with the capture path gone from the solution
# there is nothing version-specific left in here.
BUILD_PIPELINE = '''// Build the PF0 pipeline, and report back the handles the rest of the program needs. This is the
// whole of the DOCA Flow work: everything before it is device and library setup, everything after
// it is the runtime loop.
//
// Which pipes exist depends on --percent: MARK only when it is above zero, and RANDOM_SAMPLE only
// when it is strictly between the two extremes (at 0 or 100 the wire feeds one forwarding pipe
// directly, with no sampling stage to pay for).
static void build_pipeline(struct doca_flow_port *port, const struct app_config *cfg,
                           struct pipeline *out) {
  // ---------------- NO-OP CONFIGURATION: comment out this line in Step 4.1. --------------------
  // As shipped this line IS the whole pipeline: one root pipe moving packets between the wire and
  // the receiver SF, so the program runs at line rate with nothing marked and nothing counted.
  // Until you uncomment the block below, the compiler reports the functions it would have called
  // as "defined but not used" -- expected, and how you know what is still unwired.
  create_root_pipe_nop(port);

  // ---------------- YOUR PIPELINE ---------------------------------------------------------------
  // Step 4.1: uncomment the two lines tagged [1] at the end.
  // Step 4.2 and Step 4.3: uncomment the rest of the block as well.
  //
  // struct doca_flow_pipe *wire_target = create_passthrough_pipe(port);  // [1]
  //
  // // PASS forwards and counts; MARK also rewrites the ECN bits to CE. Anything they do not
  // // match misses into PASSTHROUGH and reaches the SF anyway, so the match only decides what is
  // // counted and marked, never what gets through.
  // struct doca_flow_pipe *pass =
  //     create_forward_to_sf_pipe(port, false, wire_target, &out->pass_entry);
  // struct doca_flow_pipe *mark = NULL;
  // if (cfg->random_percent > 0.0)
  //   mark = create_forward_to_sf_pipe(port, true, wire_target, &out->ce_entry);
  //
  // // Where wire traffic actually enters, per --percent.
  // if (cfg->random_percent >= 100.0)
  //   // mark everything
  //   wire_target = mark;
  // else if (cfg->random_percent <= 0.0)
  //   // mark nothing
  //   wire_target = pass;
  // else {
  //   out->sample_mask = get_random_mask(cfg->random_percent);
  //   wire_target = create_sampling_pipe(port, mark, pass, out->sample_mask);
  // }
  //
  // create_root_pipe(port, wire_target);  // [1]
}'''


def find_fn(lines, name):
    """(first line, closing-brace line) of a top-level function definition."""
    starts = [i for i, l in enumerate(lines) if l.startswith("static") and (name + "(") in l]
    if len(starts) != 1:
        sys.exit(f"{name}: found {len(starts)} definitions, want exactly 1")
    start = starts[0]
    o = start
    while not lines[o].rstrip().endswith("{"):
        o += 1
    c = o + 1
    while lines[c] != "}":
        c += 1
    return start, c


def cut_region(lines, fn, todo_marker, end_needle):
    """Delete the answer below one TODO block: from the blank line that closes the block down to the
    first line containing `end_needle`, inclusive. The TODO block itself survives into the template.
    Scoping to function `fn` keeps common needles (like `install_status`) unambiguous. Mutates
    `lines` in place."""
    s, c = find_fn(lines, fn)
    try:
        t = next(i for i in range(s, c + 1) if todo_marker in lines[i])
    except StopIteration:
        sys.exit(f"{fn}: {todo_marker} not found -- the recipe lives in the solution now")

    # Walk off the end of the comment run. What follows must be the blank line that separates the
    # recipe from its answer; without it there is no way to tell where one stops and the other
    # starts, so say so rather than deleting the wrong thing.
    a = t
    while a <= c and lines[a].lstrip().startswith("//"):
        a += 1
    if a > c or lines[a].strip():
        got = lines[a] if a <= c else "<end of function>"
        sys.exit(f"{fn}: {todo_marker} is not followed by a blank line (got {got!r}). "
                 f"A TODO block must be one unbroken comment run, then a blank, then the code.")

    try:
        b = next(i for i in range(a, c + 1) if end_needle in lines[i])
    except StopIteration:
        sys.exit(f"{fn}: end anchor not found below {todo_marker} ({end_needle!r})")
    del lines[a:b + 1]


def replace_lines(lines, old, new):
    """Replace one exact run of lines. Fails loudly rather than silently doing nothing."""
    n = len(old)
    hits = [i for i in range(len(lines) - n + 1) if lines[i:i + n] == old]
    if len(hits) != 1:
        sys.exit(f"expected 1 match for {old[0]!r}, found {len(hits)}")
    lines[hits[0]:hits[0] + n] = new


def derive(tree):
    """Produce the template text for one tree from its solution."""
    src = REPO / tree / "doca-flow" / "doca_flow_solution.c"
    lines = src.read_text().split("\n")

    # Cut the exercise regions. Bottom-up within each function, so the earlier anchors do not move;
    # cut_region re-locates the function on every call anyway.
    for fn, todo_marker, end in reversed(CUTS):
        cut_region(lines, fn, todo_marker, end)

    # Swap build_pipeline, its comment block included.
    b0, b1 = find_fn(lines, "build_pipeline")
    while b0 > 0 and lines[b0 - 1].startswith("//"):
        b0 -= 1
    lines[b0:b1 + 1] = BUILD_PIPELINE.split("\n")

    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="verify the templates match their solutions; write nothing")
    args = ap.parse_args()

    stale = []
    for tree in TREES:
        dst = REPO / tree / "doca-flow" / "doca_flow_template.c"
        want = derive(tree)

        if args.check:
            if not dst.exists() or dst.read_text() != want:
                stale.append(dst.relative_to(REPO))
                print(f"  STALE  {dst.relative_to(REPO)}")
            else:
                print(f"  ok     {dst.relative_to(REPO)}")
            continue

        dst.write_text(want)
        # clang-format last, so the template matches the repo style even if the substituted
        # build_pipeline was wrapped differently from the .clang-format rules. Tolerate it being
        # absent (check=False only covers a non-zero exit, not a missing binary): the derived text
        # is already 2-space, <=100-col style, so the only cost is that any odd wrapping is left for
        # a machine that has clang-format to tidy before commit.
        try:
            subprocess.run(["clang-format", "-i", str(dst)], check=False)
        except FileNotFoundError:
            print("  note   clang-format not found; wrote unformatted (tidy before committing)")
        print(f"  wrote  {dst.relative_to(REPO)}")

    if stale:
        print(f"\n{len(stale)} template(s) out of date -- run this script without --check",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
