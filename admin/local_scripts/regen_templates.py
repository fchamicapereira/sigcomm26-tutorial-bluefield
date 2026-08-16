#!/usr/bin/env python3
"""Re-derive every participant template from its solution.

    doca-N/doca-flow/doca_flow_ecn_pcap.c   (solution, the source of truth)
        -> doca-N/doca-flow/doca_flow_template.c   (exercise)

The template differs from the solution in exactly two ways, and this script is the only thing that
should ever produce it -- hand-editing is how the two drift apart:

  1. Three function bodies are emptied to a `TODO n` stub. These are the exercise:

         doca-2                       doca-3
         bind_capture_mirror          create_flood_pipe        TODO 1  (the capture copy)
         create_forward_to_sf_pipe    create_forward_to_sf_pipe TODO 2  (the CE marking)
         create_sampling_pipe         create_sampling_pipe      TODO 3  (the 1-in-N split)

  2. `build_pipeline` is replaced with a version wired as a NO-OP forwarder, with the ECN pipeline
     present but commented out. That is what makes the untouched template behave exactly like
     doca_flow_nop: run it and traffic flows at line rate, unmarked. The participant's first act is
     to comment out one line and uncomment the block under it.

Everything else -- including `create_passthrough_pipe`, `create_to_cpu_pipe` and `create_root_pipe`,
which are the worked examples -- is copied verbatim, so `diff` between the two files shows the
exercise and nothing else.

Why `create_root_pipe` is given away rather than being a TODO: the no-op configuration needs it (it
is what points wire ingress at the passthrough pipe), and a template that cannot forward at all is a
worse starting point than one that forwards plainly. This was the opposite of the earlier design,
where the untouched template blackholed every packet and participants had to be told that was
intentional.

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

# Function -> TODO number. The numbers are the order the GUIDE asks for them, which is not the order
# they appear in the file: marking first (it is the point of the exercise and gives a visible result
# on its own), then the capture copy, then the optional sampler. Renumbering here rather than
# reordering the file keeps the template a clean diff against the solution.
STUBS = {
    "doca-2": [("create_forward_to_sf_pipe", 1, "  return NULL;"),
               ("bind_capture_mirror", 2, "  return;"),
               ("create_sampling_pipe", 3, "  return NULL;")],
    "doca-3": [("create_forward_to_sf_pipe", 1, "  return NULL;"),
               ("create_flood_pipe", 2, "  return NULL;"),
               ("create_sampling_pipe", 3, "  return NULL;")],
}

# The no-op build_pipeline, shared between trees except for the capture wiring in the commented
# block. {capture} is substituted per tree.
BUILD_PIPELINE = '''// Build the PF0 pipeline, and report back the handles the rest of the program needs. This is the
// whole of the DOCA Flow work: everything before it is device and library setup, everything after
// it is the runtime loop.
//
// AS SHIPPED this builds a plain forwarder and nothing else -- the same data path doca_flow_nop
// provides. Wire ingress goes straight to the receiver SF untouched, whatever comes back from the
// SF goes straight out to the wire, and traffic runs at line rate with no marking and no capture.
//
// THE EXERCISE is to turn that into the ECN pipeline: comment out the one line of no-op wiring
// below, uncomment the block under it, and implement the three TODOs it calls. Which pipes that
// block builds depends on the options -- the RSS pipe and the mirror only when --pcap asked for a
// capture, MARK_CAPTURE only when --percent is above zero, and RANDOM_SAMPLE only when --percent is
// strictly between the two extremes.
//
// Until you uncomment it, the compiler reports the functions it would have called as "defined but
// not used". That is expected, and those warnings are how you know you have not wired them up yet.
static void build_pipeline(struct doca_flow_port *port, const struct app_config *cfg,
                           struct pipeline *out) {{
  // Both configurations need this pipe. In the no-op it IS the data path; in the ECN pipeline it is
  // where the two marking pipes send whatever they do not match.
  struct doca_flow_pipe *passthrough = create_passthrough_pipe(port);

  // ---------------- NO-OP CONFIGURATION: comment out this line for the exercise. --------------
  create_root_pipe(port, passthrough);

  // ---------------- ECN CONFIGURATION: uncomment everything below. -----------------------------
  //
  // bool capture = (cfg->pcap_path != NULL);
  //
{capture}  //
  // // wire-ingress entry point per --percent
  // struct doca_flow_pipe *wire_target;
  // if (cfg->random_percent >= 100.0)
  //   // mark+capture all
  //   wire_target = mark_cap;
  // else if (cfg->random_percent <= 0.0)
  //   // capture all, mark none
  //   wire_target = pass_cap;
  // else {{
  //   out->sample_mask = get_random_mask(cfg->random_percent);
  //   wire_target = create_sampling_pipe(port, mark_cap, pass_cap, out->sample_mask);
  // }}
  //
  // create_root_pipe(port, wire_target);
}}'''

CAPTURE = {
    "doca-2": '''  // if (capture) {
  //   struct doca_flow_pipe *cpu = create_to_cpu_pipe(port);
  //   bind_capture_mirror(port, cpu);
  // }
  //
  // // PASS_CAPTURE (no mark) and MARK_CAPTURE (CE-mark); both mirror to pcap only when capturing.
  // struct doca_flow_pipe *pass_cap =
  //     create_forward_to_sf_pipe(port, false, capture, passthrough, &out->pass_entry);
  // struct doca_flow_pipe *mark_cap = NULL;
  // if (cfg->random_percent > 0.0)
  //   mark_cap = create_forward_to_sf_pipe(port, true, capture, passthrough, &out->ce_entry);
''',
    # 3.x builds PASSTHROUGH before FLOOD, because FLOOD's ordered entry forwards to it.
    "doca-3": '''  // struct doca_flow_pipe *flood = NULL;
  // if (capture) {
  //   struct doca_flow_pipe *cpu = create_to_cpu_pipe(port);
  //   flood = create_flood_pipe(port, passthrough, cpu);
  // }
  //
  // // PASS_CAPTURE (no mark) and MARK_CAPTURE (CE-mark); both fan out to the pcap when capturing.
  // struct doca_flow_pipe *pass_cap =
  //     create_forward_to_sf_pipe(port, false, flood, passthrough, &out->pass_entry);
  // struct doca_flow_pipe *mark_cap = NULL;
  // if (cfg->random_percent > 0.0)
  //   mark_cap = create_forward_to_sf_pipe(port, true, flood, passthrough, &out->ce_entry);
''',
}


def find_fn(lines, name):
    """(first line, closing-brace line) of a top-level function definition."""
    start = next(i for i, l in enumerate(lines)
                 if l.startswith("static") and (name + "(") in l)
    o = start
    while not lines[o].rstrip().endswith("{"):
        o += 1
    c = o + 1
    while lines[c] != "}":
        c += 1
    return start, c


def derive(tree):
    """Produce the template text for one tree from its solution."""
    src = REPO / tree / "doca-flow" / "doca_flow_ecn_pcap.c"
    lines = src.read_text().split("\n")

    # Empty the exercise bodies. Each stub is two lines on purpose: a lone comment is short enough
    # that clang-format collapses the whole function onto one line.
    for name, n, ret in STUBS[tree]:
        o, c = find_fn(lines, name)
        while not lines[o].rstrip().endswith("{"):
            o += 1
        lines[o + 1:c] = ["  // TODO %d -- your code here." % n, ret]

    # Swap build_pipeline, comment block included.
    b0, b1 = find_fn(lines, "build_pipeline")
    while b0 > 0 and lines[b0 - 1].startswith("//"):
        b0 -= 1
    lines[b0:b1 + 1] = BUILD_PIPELINE.format(capture=CAPTURE[tree]).split("\n")

    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="verify the templates match their solutions; write nothing")
    args = ap.parse_args()

    stale = []
    for tree in STUBS:
        dst = REPO / tree / "doca-flow" / "doca_flow_template.c"
        want = derive(tree)

        if args.check:
            if dst.read_text() != want:
                stale.append(dst.relative_to(REPO))
                print(f"  STALE  {dst.relative_to(REPO)}")
            else:
                print(f"  ok     {dst.relative_to(REPO)}")
            continue

        dst.write_text(want)
        # clang-format last, so the template matches the repo style even if the substituted
        # build_pipeline was wrapped differently from the .clang-format rules.
        subprocess.run(["clang-format", "-i", str(dst)], check=False)
        print(f"  wrote  {dst.relative_to(REPO)}")

    if stale:
        print(f"\n{len(stale)} template(s) out of date -- run this script without --check",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
