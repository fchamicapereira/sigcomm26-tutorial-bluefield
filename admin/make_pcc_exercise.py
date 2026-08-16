#!/usr/bin/env python3
"""Derive a PCC exercise scaffold from a finished pure-ECN controller.

The exercise (device/algo/rtt_template_exercise.c) is algo/rtt_template.c with exactly two things
removed -- the CNP multiplicative decrease and the TX additive increase -- each replaced by a TODO
block, plus a rewritten header. It builds and runs as shipped but does no rate control until a
participant fills the two TODOs. update_participants_repo_on_github.py ships it, renamed to
rtt_template.c, as the participant exercise.

This tool encodes that transform so every version's exercise is produced the same way, and so the
exercise can be regenerated after the controller changes. The substitutions below must each match
exactly once in the source; a miss is a hard error rather than a silent no-op.

    admin/make_pcc_exercise.py doca-2                 # write doca-2/.../rtt_template_exercise.c
    admin/make_pcc_exercise.py doca-2 --check         # verify the on-disk exercise matches
    admin/make_pcc_exercise.py doca-3 --out /tmp/x.c  # write elsewhere (dry check)
"""

import argparse
import difflib
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent


def algo_path(version, name):
    return REPO / version / "doca-pcc-ecn" / "device" / "algo" / name


# Each entry is (old, new): a verbatim slice of the finished controller and what replaces it in the
# exercise. These bodies are identical across versions (only the dormant RTT handler differs), so
# one set of substitutions serves every version.
SUBS = [
    # Header: describe the exercise instead of the finished controller.
    (
        """ *  Pure-ECN (DCQCN-style) congestion controller  --  tutorial add-on
 *  Rate is driven ONLY by CNP (multiplicative decrease) + TX (additive increase).
 *  The RTT-based rate update is disabled in rtt_template_handle_roce_rtt() below.
 *  Tune ECN_CNP_DEC_FACTOR: 800..995  =>  x0.800 .. x0.995 per CNP (x0.90 is the sweet spot).""",
        """ *  EXERCISE — build the pure-ECN (DCQCN-style) congestion controller yourself.
 *
 *  This is a copy of algo/rtt_template.c with the two rate reactions removed. As shipped it
 *  compiles and runs, but does NO congestion control -- the send rate never changes. Your job is
 *  to add the two reactions, marked TODO 1 and TODO 2 below:
 *     TODO 1  (in rtt_template_handle_roce_cnp)  cut the rate when a CNP arrives   (decrease)
 *     TODO 2  (in rtt_template_handle_roce_tx)   raise the rate when it is quiet   (increase)
 *  The RTT handler is already decoupled (RTT is measured but does not steer the rate), which is
 *  what makes this a "pure-ECN" controller. Fill in the two TODOs, rebuild, and the rate starts
 *  reacting. `diff` against algo/rtt_template.c for the finished version.
 *
 *  ECN_CNP_DEC_FACTOR is the per-CNP cut factor (fxp16); you use it in TODO 1.
 *  Tune it later: 800..995  =>  x0.800 .. x0.995 per CNP (x0.90 is the sweet spot).""",
    ),
    # TODO 2: additive increase, in rtt_template_handle_roce_tx().
    (
        """  /* Pure-ECN: gated additive increase (recover when CNPs stop) */
  {
    static uint32_t g_tx_inc = 0;
    if ((++g_tx_inc % 1000) == 0) {
      cur_rate += (AI >> 2);
      if (cur_rate > RATE_MAX) cur_rate = RATE_MAX;
    }
  }""",
        """  /* === TODO 2: additive increase ===============================================================
   * A TX event means packets are flowing. When congestion has eased, the rate should drift back up
   * -- gently, so you don't overshoot and immediately re-trigger congestion.
   *
   * Fill in: only every ~1000th call, add a small step to `cur_rate` (AI >> 2, i.e. about 1.25% of
   * line rate) and cap it at RATE_MAX. A `static` counter plus `if ((++counter % 1000) == 0) { ... }`
   * is the usual pattern. Leave this empty and the rate can only ever fall, never recover.
   * ============================================================================================ */
  /* your code here */
""",
    ),
    # TODO 1: multiplicative decrease, in rtt_template_handle_roce_cnp().
    (
        """  /* Pure-ECN: multiplicative decrease per CNP, floored at MIN_RATE */
  cur_rate = doca_pcc_dev_fxp_mult(ECN_CNP_DEC_FACTOR, cur_rate);
  if (cur_rate < MIN_RATE) cur_rate = MIN_RATE;""",
        """  /* === TODO 1: multiplicative decrease =========================================================
   * A CNP means the network is congested. Cut the send rate by a fixed factor, and never let it
   * fall below the floor.
   *
   * Fill in: set  cur_rate = cur_rate x ECN_CNP_DEC_FACTOR  using doca_pcc_dev_fxp_mult(), which
   * multiplies an fxp16 factor by an fxp20 rate; then clamp cur_rate up to at least MIN_RATE.
   * Leave this empty and the controller never reacts -- the rate just sits at full speed and the
   * PURE_ECN line below prints the same rate forever.
   * ============================================================================================ */
  /* your code here */
""",
    ),
    # The observe printf's comment: it is no longer "optional" once TODO 1 is the exercise.
    (
        "    static uint32_t g_cnp = 0; /* optional: observe the loop engaging */",
        "    static uint32_t g_cnp = 0; /* observe the loop: prints the rate as CNPs arrive */",
    ),
]


def make_exercise(src_text):
    out = src_text
    for i, (old, new) in enumerate(SUBS, 1):
        n = out.count(old)
        if n != 1:
            sys.exit(f"substitution {i} matched {n} times, expected 1 -- controller changed shape?")
        out = out.replace(old, new)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("version", help="e.g. doca-2, doca-3")
    ap.add_argument("--out", type=pathlib.Path, help="write here instead of next to the source")
    ap.add_argument("--check", action="store_true",
                    help="compare against the on-disk exercise; nonzero exit if it differs")
    args = ap.parse_args()

    src = algo_path(args.version, "rtt_template.c")
    if not src.is_file():
        sys.exit(f"no controller at {src}")
    generated = make_exercise(src.read_text())

    if args.check:
        existing = algo_path(args.version, "rtt_template_exercise.c")
        if not existing.is_file():
            sys.exit(f"no exercise at {existing} to check against")
        if existing.read_text() == generated:
            print(f"{args.version}: exercise matches the transform")
            return 0
        diff = difflib.unified_diff(generated.splitlines(True), existing.read_text().splitlines(True),
                                    "generated", "on-disk")
        sys.stdout.writelines(diff)
        sys.exit(f"{args.version}: on-disk exercise differs from the transform")

    dst = args.out or algo_path(args.version, "rtt_template_exercise.c")
    dst.write_text(generated)
    print(f"wrote {dst} ({generated.count('TODO')} TODO markers)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
