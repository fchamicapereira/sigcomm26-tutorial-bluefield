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

COMMENTS MUST NOT DIVERGE. `diff solution template` is read by us to check the exercise, and a
comment that has merely been REWORDED looks exactly like the two files drifting apart. So the stubs
below keep the solution's own prose verbatim wherever the code it describes survives the cut, and
`build_pipeline`'s header is word-for-word the solution's. The only comment differences the diff
should ever show are:

  - a comment deleted together with the code it describes, and
  - the `TODO Na` markers, which by definition exist only in the template.

Anything else is a bug in this file. Keep the TODO text short and point at the guide: Part D spells
each one out in full, and duplicating it here is a second copy to keep in step.

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
# Per-tree spellings, substituted into the stubs below. The two DOCA generations disagree on the
# ingress-port field, its width, the add-entry call and the entry flags; everything else the
# participant types is identical, so one set of stubs covers both trees.
TOKENS = {
    "doca-2": {"PORT": "port_meta", "PORTMASK": "0xFFFFFFFF",
               "ADD": "doca_flow_pipe_add_entry", "ADDARGS": "&match",
               "NOWAIT": "DOCA_FLOW_NO_WAIT", "BATCH": "DOCA_FLOW_WAIT_FOR_BATCH",
               "ACTIDX": "\n  //      entry_actions.action_idx = 0;"},
    "doca-3": {"PORT": "port_id", "PORTMASK": "0xFFFF",
               "ADD": "doca_flow_pipe_basic_add_entry", "ADDARGS": "&match, 0",
               "NOWAIT": "DOCA_FLOW_ENTRY_FLAGS_NO_WAIT",
               "BATCH": "DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH", "ACTIDX": ""},
}

# The exercise. One entry per cut region: (function, first-line anchor, last-line anchor, stub).
# Both anchors are matched INSIDE the named function, so needles as common as `install_status` stay
# unambiguous. Later regions are cut before earlier ones so the earlier anchors do not move.
#
# The TODO text is a RECIPE, not a puzzle: numbered steps naming the exact fields and calls. This is
# a 30-minute session and the point of it is to see a pipeline run, not to reverse-engineer an API
# from neighbouring functions. The understanding is meant to come from Part D and from reading the
# worked examples, not from guessing what to type.
CUTS = [
    # ---- TODO 1: create_root_pipe -----------------------------------------------------------
    ("create_root_pipe", "A full mask on the ingress port", "doca_flow_pipe_create(cfg,",
     ["  // A full mask on the ingress port: it is compared exactly, and each entry supplies the port it",
      "  // matches.",
      "  struct doca_flow_match match = {0}, match_mask = {0};",
      "  struct doca_flow_fwd fwd_hit = {0};",
      "  struct doca_flow_fwd fwd_miss = {0};",
      "  struct doca_flow_pipe *pipe = NULL;",
      "",
      "  // TODO 1a -- build the pipe. In order:",
      "  //   1. match.parser_meta.{PORT} = {PORTMASK};       -- this field participates",
      "  //   2. match_mask.parser_meta.{PORT} = {PORTMASK};  -- and is compared exactly",
      "  //   3. doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask)",
      "  //   4. fwd_hit.type = DOCA_FLOW_FWD_CHANGEABLE     -- each entry names its own target",
      "  //   5. fwd_miss.type = DOCA_FLOW_FWD_DROP",
      "  //   6. doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe)",
      "  // Wrap each call in DOCA_CHECK(\"PORT_DEMUX\", ...). create_root_pipe_nop() above builds",
      "  // exactly this pipe -- the two differ only in one entry, in 1b."]),
    ("create_root_pipe", "struct entry_batch_status install_status", '"PORT_DEMUX: install"',
     ["  struct entry_batch_status install_status = {0};",
      "  struct doca_flow_pipe_entry *entry;",
      "  struct doca_flow_match entry_match = {0};",
      "  struct doca_flow_fwd entry_fwd = {0};",
      "",
      "  // TODO 1b -- add one entry per direction, then install both. In order:",
      "  //   1. from the wire, on into your marking chain:",
      "  //        entry_match.parser_meta.{PORT} = PF_PORT_ID;",
      "  //        entry_fwd.type = DOCA_FLOW_FWD_PIPE;",
      "  //        entry_fwd.next_pipe = wire_target;   <-- a PIPE, not a port. THIS is the one",
      "  //                                                 line that differs from the no-op.",
      "  //        {ADD}(PIPE_QUEUE, pipe, &entry_match, NULL, NULL, &entry_fwd,",
      "  //                                 {BATCH}, &install_status, &entry)",
      "  //   2. back from the receiver SF, straight out of the uplink:",
      "  //        entry_match.parser_meta.{PORT} = SF_REP_PORT_ID;",
      "  //        memset(&entry_fwd, 0, sizeof(entry_fwd));   -- reused, so clear it first",
      "  //        entry_fwd.type = DOCA_FLOW_FWD_PORT;",
      "  //        entry_fwd.port_id = PF_PORT_ID;",
      "  //        the same add-entry call, but with {NOWAIT}",
      "  //   3. doca_flow_entries_process(port, PIPE_QUEUE, ENTRY_PROCESS_TIMEOUT_US, nb_entries)",
      "  //   4. fail unless install_status.failure is false and .nb_processed == nb_entries"]),
    # ---- TODO 2: create_forward_to_sf_pipe --------------------------------------------------
    ("create_forward_to_sf_pipe", "A field set in the template but zeroed in the mask",
     "doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe)",
     ["  // A field set in the template but zeroed in the mask is declared without being compared, which is",
      "  // how dscp_ecn is treated here: MARK has to catch packets that arrive already CE-marked as",
      "  // readily as fresh ones. l3_type has no mask entry, so it is compared exactly.",
      "  struct doca_flow_match match = {0}, match_mask = {0};",
      "  struct doca_flow_actions action_template = {0}, *action_templates[1] = {&action_template};",
      "  struct doca_flow_monitor monitor = {0};",
      "  struct doca_flow_fwd fwd_hit = {0};",
      "  struct doca_flow_fwd fwd_miss = {0};",
      "  struct doca_flow_pipe *pipe = NULL;",
      "",
      "  // TODO 2a -- build the pipe. In order:",
      "  //   1. the match, any IPv4 packet whatever its ECN bits:",
      "  //        match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;",
      "  //        match.outer.ip4.dscp_ecn = 0xFF;        -- the byte participates...",
      "  //        match_mask.outer.ip4.dscp_ecn = 0x00;   -- ...but is not compared",
      "  //        doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask)",
      "  //   2. the action, ONLY when `mark` is true:",
      "  //        action_template.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;",
      "  //        action_template.outer.ip4.dscp_ecn = 0xFF;   -- entries may rewrite this byte",
      "  //        doca_flow_pipe_cfg_set_actions(cfg, action_templates, NULL, NULL, 1)",
      "  //   3. the counter, which is what the CE marked: report reads:",
      "  //        monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;",
      "  //        doca_flow_pipe_cfg_set_monitor(cfg, &monitor)",
      "  //   4. the forwards:",
      "  //        fwd_hit.type = DOCA_FLOW_FWD_PORT;   fwd_hit.port_id = SF_REP_PORT_ID;",
      "  //        fwd_miss.type = DOCA_FLOW_FWD_PORT;  fwd_miss.port_id = SF_REP_PORT_ID;",
      "  //        -- hit and miss both go to the SF; the match only decides what gets",
      "  //           counted and marked, so nothing is dropped here",
      "  //   5. doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe)",
      "  // Wrap each call in DOCA_CHECK(name, ...)."]),
    ("create_forward_to_sf_pipe", "What the template above allowed to be written",
     '"%s install", name)',
     ["  struct doca_flow_actions entry_actions = {0};",
      "  struct entry_batch_status install_status = {0};",
      "",
      "  // TODO 2b -- add the one entry and install it. In order:",
      "  //   1. the value to write, ONLY when `mark` is true:",
      "  //      entry_actions.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;",
      "  //      entry_actions.outer.ip4.dscp_ecn = 0x03;   -- both ECN bits set: CE{ACTIDX}",
      "  //   2. match.outer.ip4.dscp_ecn = 0x00;   -- `match` is reused as this entry's values,",
      "  //                                            so drop the 0xFF placeholder from step 1 above",
      "  //   3. {ADD}(PIPE_QUEUE, pipe, {ADDARGS},",
      "  //                               mark ? &entry_actions : NULL, &monitor, NULL,",
      "  //                               {NOWAIT}, &install_status, out_entry)",
      "  //      -- out_entry, not &entry: the counter report queries the entry you hand back",
      "  //   4. doca_flow_entries_process(port, PIPE_QUEUE, ENTRY_PROCESS_TIMEOUT_US, nb_entries)",
      "  //   5. fail unless install_status.failure is false and .nb_processed == nb_entries"]),
    # ---- TODO 3: create_sampling_pipe -------------------------------------------------------
    ("create_sampling_pipe", "struct doca_flow_match match = {0}, match_mask = {0};",
     "doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe)",
     ["  struct doca_flow_match match = {0}, match_mask = {0};",
      "  struct doca_flow_fwd fwd_hit = {0};",
      "  struct doca_flow_fwd fwd_miss = {0};",
      "  struct doca_flow_pipe *pipe = NULL;",
      "",
      "  // TODO 3a -- build the pipe. In order:",
      "  //   1. match.parser_meta.random = 0;",
      "  //   2. match_mask.parser_meta.random = mask;   -- `mask` is a power of two minus one,",
      "  //                                                 so this hits 1 packet in (mask + 1)",
      "  //   3. doca_flow_pipe_cfg_set_match(cfg, &match, &match_mask)",
      "  //   4. fwd_hit.type = DOCA_FLOW_FWD_PIPE;   fwd_hit.next_pipe = hit;",
      "  //      fwd_miss.type = DOCA_FLOW_FWD_PIPE;  fwd_miss.next_pipe = miss;",
      "  //      -- both are pipes, and both go on to the receiver; a miss here means",
      "  //         `not selected for marking', not an error",
      "  //   5. doca_flow_pipe_create(cfg, &fwd_hit, &fwd_miss, &pipe)",
      "  // Wrap each call in DOCA_CHECK(\"RANDOM_SAMPLE\", ...)."]),
    ("create_sampling_pipe", "The entry adds nothing to the template", '"RANDOM_SAMPLE: install"',
     ["  // The entry adds nothing to the template: no actions, no counter, and no forward of its own, so",
      "  // both outcomes are decided by the pipe's own two forwards.",
      "  struct entry_batch_status install_status = {0};",
      "  struct doca_flow_pipe_entry *entry;",
      "",
      "  // TODO 3b -- add the one entry and install it. In order:",
      "  //   1. {ADD}(PIPE_QUEUE, pipe, {ADDARGS}, NULL, NULL, NULL,",
      "  //                               {NOWAIT}, &install_status, &entry)",
      "  //   2. doca_flow_entries_process(port, PIPE_QUEUE, ENTRY_PROCESS_TIMEOUT_US, nb_entries)",
      "  //   3. fail unless install_status.failure is false and .nb_processed == nb_entries"]),
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
  // ---------------- NO-OP CONFIGURATION: comment out this line in D.1. -------------------------
  // As shipped this line IS the whole pipeline: one root pipe moving packets between the wire and
  // the receiver SF, so the program runs at line rate with nothing marked and nothing counted.
  // Until you uncomment the block below, the compiler reports the functions it would have called
  // as "defined but not used" -- expected, and how you know what is still unwired.
  create_root_pipe_nop(port);

  // ---------------- YOUR PIPELINE ---------------------------------------------------------------
  // D.1: uncomment the two lines tagged [1] at the end.
  // D.2 and D.3: uncomment the rest of the block as well.
  //
  // struct doca_flow_pipe *wire_target = create_passthrough_pipe(port);  // [1]
  //
  // // PASS forwards and counts; MARK also rewrites the ECN bits to CE. Both send everything to
  // // the SF either way -- the match only decides what is counted and marked.
  // struct doca_flow_pipe *pass = create_forward_to_sf_pipe(port, false, &out->pass_entry);
  // struct doca_flow_pipe *mark = NULL;
  // if (cfg->random_percent > 0.0) mark = create_forward_to_sf_pipe(port, true, &out->ce_entry);
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
        # Plain replacement, NOT str.format: the stubs contain literal `= {0}` initialisers
        # that format() would read as positional fields.
        filled = "\n".join(stub)
        for key, value in TOKENS[tree].items():
            filled = filled.replace("{" + key + "}", value)
        cut_region(lines, fn, start, end, filled.split("\n"))

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
