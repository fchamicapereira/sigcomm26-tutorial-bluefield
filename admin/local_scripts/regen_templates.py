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
     writes -- building the pipe, and adding its entries -- are cut to TODO stubs. Every struct the
     participant needs is still declared for them, so what is missing is the DOCA calls and the
     fields they take, not the scaffolding around them:

         TODO 1   create_root_pipe            sort wire traffic from SF traffic  (Part D.1)
         TODO 2   create_forward_to_sf_pipe   forward, count, and CE-mark        (Part D.2)
         TODO 3   create_sampling_pipe        mark 1 packet in N                 (Part D.3)

     The regions are found by anchoring on code lines inside each function (see CUTS), so the
     solution needs no tooling markers, and the anchors are deliberately version-agnostic -- no
     port_meta/port_id, no add_entry-versus-basic_add_entry -- so one set covers both trees.

  2. `build_pipeline` is replaced with a version wired as a NO-OP FORWARDER: it calls
     create_root_pipe_nop() and nothing else, with the real pipeline present but commented out.
     Running the untouched template therefore forwards traffic at line rate, which is where Part D
     starts. create_root_pipe_nop() itself is copied through verbatim, minus the
     __attribute__((unused)) the solution needs to keep -Wall quiet about it.

Everything else -- including `create_passthrough_pipe`, the worked example -- is identical, so
`diff` between the two files shows the exercise and nothing else.

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

# The exercise. One entry per cut region: (function, first-line anchor, last-line anchor, stub).
# Both anchors are matched INSIDE the named function, so needles as common as `install_status` stay
# unambiguous. Later regions are cut before earlier ones so the earlier anchors do not move.
CUTS = [
    # ---- TODO 1: create_root_pipe -----------------------------------------------------------
    ("create_root_pipe", "A full mask on the ingress port", "doca_flow_pipe_create(cfg,",
     ["  // The match and its mask, a hit forward and a miss forward, and a handle for the new pipe",
      "  // are declared for you -- fill in their fields and create the pipe.",
      "  struct doca_flow_match match = {0}, match_mask = {0};",
      "  struct doca_flow_fwd fwd_hit = {0};",
      "  struct doca_flow_fwd fwd_miss = {0};",
      "  struct doca_flow_pipe *pipe = NULL;",
      "",
      "  // TODO 1a -- build the pipe. Match on the ingress port (parser_meta) with a full mask,",
      "  // set fwd_hit.type = CHANGEABLE so each entry brings its own destination and",
      "  // fwd_miss.type = DROP, set the match on the cfg, then doca_flow_pipe_create(...).",
      "  // create_root_pipe_nop() above does all of this already -- start from it."]),
    ("create_root_pipe", "struct entry_batch_status install_status", '"PORT_DEMUX: install"',
     ["  // The batch status, an entry handle, and reusable match/forward scratch structs are",
      "  // declared for you -- fill in and install the pipe's two entries.",
      "  struct entry_batch_status install_status = {0};",
      "  struct doca_flow_pipe_entry *entry;",
      "  struct doca_flow_match entry_match = {0};",
      "  struct doca_flow_fwd entry_fwd = {0};",
      "",
      "  // TODO 1b -- add and install the two entries. Wire (PF_PORT_ID) -> wire_target, which is",
      "  // a PIPE rather than a port and is the ONE thing that differs from the no-op above;",
      "  // receiver SF (SF_REP_PORT_ID) -> PF_PORT_ID. Then doca_flow_entries_process(...) and",
      "  // check the batch status."]),
    # ---- TODO 2: create_forward_to_sf_pipe --------------------------------------------------
    ("create_forward_to_sf_pipe", "A field set in the template but zeroed in the mask",
     "doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe)",
     ["  // The match and its mask, an action template and the one-element array the cfg wants it",
      "  // in, a monitor, the two forwards and a handle for the new pipe are declared for you.",
      "  struct doca_flow_match match = {0}, match_mask = {0};",
      "  struct doca_flow_actions action_template = {0}, *action_templates[1] = {&action_template};",
      "  struct doca_flow_monitor monitor = {0};",
      "  struct doca_flow_fwd fwd_hit = {0};",
      "  struct doca_flow_fwd fwd_miss = {0};",
      "  struct doca_flow_pipe *pipe = NULL;",
      "",
      "  // TODO 2a -- build the pipe:",
      "  //   match     IPv4, whatever the DSCP/ECN byte holds (0xFF in the match, 0x00 in the mask)",
      "  //   actions   only when `mark`: declare that entries may rewrite outer.ip4.dscp_ecn",
      "  //   monitor   a non-shared counter, which is what the CE marked: report reads",
      "  //   forwards  hits to SF_REP_PORT_ID, misses to miss_pipe",
      "  // then set them on the cfg and doca_flow_pipe_create(...).",
      "  // create_passthrough_pipe() above is the same pipe without the counter or the action."]),
    ("create_forward_to_sf_pipe", "What the template above allowed to be written",
     '"%s install", name)',
     ["  // The entry's actions and the batch status are declared for you.",
      "  struct doca_flow_actions entry_actions = {0};",
      "  struct entry_batch_status install_status = {0};",
      "",
      "  // TODO 2b -- add and install the one entry. When `mark`, its actions carry the value to",
      "  // write: 0x03 is both ECN bits set, CE (Congestion Experienced). Reset match's dscp_ecn",
      "  // to 0x00 first -- the same struct is reused as this entry's values. Hand the installed",
      "  // entry back through out_entry, then doca_flow_entries_process(...) and check the status."]),
    # ---- TODO 3: create_sampling_pipe -------------------------------------------------------
    ("create_sampling_pipe", "struct doca_flow_match match = {0}, match_mask = {0};",
     "doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe)",
     ["  // The match and its mask, the two forwards and a handle for the new pipe are declared",
      "  // for you.",
      "  struct doca_flow_match match = {0}, match_mask = {0};",
      "  struct doca_flow_fwd fwd_hit = {0};",
      "  struct doca_flow_fwd fwd_miss = {0};",
      "  struct doca_flow_pipe *pipe = NULL;",
      "",
      "  // TODO 3a -- build the pipe. Match parser_meta.random against 0 under `mask`, which is a",
      "  // power of two minus one, so exactly 1 packet in (mask + 1) hits. Hits forward to `hit`,",
      "  // misses to `miss` -- both are pipes, and both go on to the receiver."]),
    ("create_sampling_pipe", "The entry adds nothing to the template", '"RANDOM_SAMPLE: install"',
     ["  // The batch status and an entry handle are declared for you.",
      "  struct entry_batch_status install_status = {0};",
      "  struct doca_flow_pipe_entry *entry;",
      "",
      "  // TODO 3b -- add and install the one entry. It adds nothing to the template: no actions,",
      "  // no counter and no forward of its own, so both outcomes are decided by the pipe's own",
      "  // two forwards. Then doca_flow_entries_process(...) and check the status."]),
]

# create_root_pipe_nop is live in the template, so it loses the attribute that keeps the solution's
# -Wall quiet, and the paragraph explaining why it is dead there.
NOP_HEAD_FROM = [
    "// Unused in this file, which is the finished program: build_pipeline() below calls "
    "create_root_pipe",
    "// instead, and the call to this one is left commented out where the template has it live.",
    "static void __attribute__((unused)) create_root_pipe_nop(struct doca_flow_port *port) {",
]
NOP_HEAD_TO = [
    "// build_pipeline() below calls this one as shipped, which is why the program forwards traffic",
    "// before you have written a line. Exercise 1 is to comment that call out and write",
    "// create_root_pipe() instead.",
    "static void create_root_pipe_nop(struct doca_flow_port *port) {",
]

# The no-op build_pipeline. Identical in both trees: with the capture path gone from the solution
# there is nothing version-specific left in here.
BUILD_PIPELINE = '''// Build the PF0 pipeline, and report back the handles the rest of the program needs. This is the
// whole of the DOCA Flow work: everything before it is device and library setup, everything after
// it is the runtime loop.
//
// AS SHIPPED this builds a no-op forwarder and nothing else. create_root_pipe_nop() installs one
// root pipe that moves packets between the wire and the receiver SF, so the program runs at line
// rate with nothing marked and nothing counted.
//
// THE EXERCISE turns that into an ECN-marking pipeline, in three steps:
//
//   Exercise 1   comment out create_root_pipe_nop() below, uncomment the two lines marked [1],
//                and write create_root_pipe()                                        -- TODO 1
//   Exercise 2   uncomment the rest of the block, and write create_forward_to_sf_pipe() -- TODO 2
//   Exercise 3   write create_sampling_pipe(), then run with --percent between 0 and 100 -- TODO 3
//
// Until you uncomment them, the compiler reports the functions they would have called as "defined
// but not used". That is expected, and those warnings are how you know what is still unwired.
static void build_pipeline(struct doca_flow_port *port, const struct app_config *cfg,
                           struct pipeline *out) {
  // ---------------- NO-OP CONFIGURATION: comment out this line in Exercise 1. ------------------
  create_root_pipe_nop(port);

  // ---------------- YOUR PIPELINE ---------------------------------------------------------------
  // Exercise 1: uncomment the two lines marked [1].
  // Exercises 2 and 3: uncomment the rest of the block as well.
  //
  // [1] struct doca_flow_pipe *wire_target = create_passthrough_pipe(port);
  //
  //     // PASS forwards and counts; MARK also rewrites the ECN bits to CE. wire_target is still
  //     // PASSTHROUGH at this point, so it is what both of them fall back to on a miss.
  //     struct doca_flow_pipe *pass =
  //         create_forward_to_sf_pipe(port, false, wire_target, &out->pass_entry);
  //     struct doca_flow_pipe *mark = NULL;
  //     if (cfg->random_percent > 0.0)
  //       mark = create_forward_to_sf_pipe(port, true, wire_target, &out->ce_entry);
  //
  //     // Where wire traffic actually enters, per --percent.
  //     if (cfg->random_percent >= 100.0)
  //       // mark everything
  //       wire_target = mark;
  //     else if (cfg->random_percent <= 0.0)
  //       // mark nothing
  //       wire_target = pass;
  //     else {
  //       out->sample_mask = get_random_mask(cfg->random_percent);
  //       wire_target = create_sampling_pipe(port, mark, pass, out->sample_mask);
  //     }
  //
  // [1] create_root_pipe(port, wire_target);
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


def cut_region(lines, fn, start_needle, end_needle, stub):
    """Replace lines[a..b] (inclusive) with `stub`, where a/b are the first lines inside function
    `fn` that contain start_needle / end_needle. Scoping to the function keeps common needles (like
    `install_status`) unambiguous. Mutates `lines` in place."""
    s, c = find_fn(lines, fn)
    try:
        a = next(i for i in range(s, c + 1) if start_needle in lines[i])
        b = next(i for i in range(a, c + 1) if end_needle in lines[i])
    except StopIteration:
        sys.exit(f"{fn}: anchor not found ({start_needle!r} .. {end_needle!r})")
    lines[a:b + 1] = stub


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
    for fn, start, end, stub in reversed(CUTS):
        cut_region(lines, fn, start, end, stub)

    replace_lines(lines, NOP_HEAD_FROM, NOP_HEAD_TO)

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
