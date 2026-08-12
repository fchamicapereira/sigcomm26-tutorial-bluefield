# Changes made to `doca-2`, to be applied to the other tutorial versions

Written 2026-08-12. `doca-2/doca-flow/` was reworked to carry both the solution and a participant
exercise. Some of the work landed in every tree, some only in `doca-2`. This is the record of what
went where, and how to repeat it.

**Status: the port is done.** The [`doca-3` unification](doca-3-unification.md) landed first, so
`doca-3.2/` is gone and `doca-2/` and `doca-3.4/` are the only two trees. Everything below has now
been applied to `doca-3.4/doca-flow/` as well; the sections are kept as the record of *what* was
changed and *why*, and as the recipe for any future tree. See
[What `doca-3.4` required differently](#what-doca-34-required-differently) for the deltas that are
forced by the DOCA version.

## Already applied everywhere

| change | where |
| --- | --- |
| `crash_if_unsuccessful` → `doca_check` | `doca-2`, `doca-3.2`, `doca-3.4` — all 348 call sites (`doca-3.2` has since been removed) |
| clang-format `ColumnLimit: 140` → `100` | all 24 `.c`/`.h` files in the repo |

Both were mechanical. The rename kept the `__attribute__((format(printf, 2, 3)))` on the helper,
which is what type-checks the format string in those 348 messages — worth re-confirming after any
future rename, by deliberately breaking one call and checking the warning still fires.

## The work, and how it was ported

### 1. Drop the file header comment block

The 17-line `/* ... */` banner at the top of `doca_flow_ecn_pcap.c` is gone; the file now starts at
`#include <doca_argp.h>`. The topology diagram it carried still lives in
`doca_flow_ecn_pcap.README.md`. Same treatment applied to both `doca-3.4` files.

`doca-3.4` had no `doca_flow_ecn_pcap.README.md`, so dropping its banner would have deleted the
only copy of that program's topology diagram and of the "how this differs from 2.x" notes. The
README was written for it (modelled on doca-2's, plus a table of the version deltas) before the
banner came out. **Check the README exists before deleting a banner in any future tree.**

### 2. Restructure `main`

`main` was ~150 lines of mixed setup, pipeline and runtime loop; it is now 25 lines of named
phases. The point is that the DOCA Flow work is a single call — `build_pipeline(port, &cfg, &pl)` —
sitting between bring-up and the runtime loop, so it is obvious what a participant edits.

Extracted, in file order: `setup_logging`, `parse_args`, `open_capture_pcap`, `close_capture_pcap`,
`build_pipeline`, `install_signal_handlers`, `log_startup`, `run_capture_loop`.

Two structs carry what used to be loose locals in `main`, which is what let the runtime loop move
out without growing a long parameter list:

```c
struct pipeline {                            /* what build_pipeline() reports back */
  struct doca_flow_pipe_entry *ce_entry;     /* MARK_CAPTURE's entry — CE-marked packet count */
  struct doca_flow_pipe_entry *pass_entry;   /* PASS_CAPTURE's entry — unmarked packet count */
  uint16_t sample_mask;                      /* 0 unless --percent is strictly between 0 and 100 */
};

struct capture_sink {                        /* the pcap file and its running totals */
  pcap_t *pd;
  pcap_dumper_t *dumper;
  uint64_t written, mirrored, sample_ctr, truncated;
};
```

`log_startup` calls `enable_key_toggle()` because what it returns decides what the startup line
says — keep those together if this is rearranged again.

### 3. Comment style

Two rules, both adopted after the column limit dropped to 100:

- **No trailing comments — every comment goes on its own line, above what it describes.** The
  motivation is concrete: at 100 columns a trailing comment pushes the line over, and clang-format
  then breaks the *code* rather than the comment, producing things like

  ```c
  static volatile bool g_capture_writing =
      false; /* starts OFF; SPACE or SIGUSR1 toggles it at runtime */
  ```

- **`//` everywhere — including multi-line comments.** No `/* ... */` blocks at all; a multi-line
  comment is just consecutive `//` lines. `#endif` labels become `#endif  // NAME`.

Exactly one idiom is exempt, and it is a hard constraint rather than a preference:

| exempt | example | why |
| --- | --- | --- |
| argument names at call sites | `create_fwd_pipe(port, 1, /*with_counter=*/true)` | mid-expression — `//` would comment out the rest of the line, including the closing paren |

Applied to all of `doca-2/doca-flow/` and all of `doca-3.4/doca-flow/`: `doca_flow_ecn_pcap.c`,
`doca_flow_nop.c`, `doca_flow_compat.h`, and the template regenerated from the solution. **Not**
applied to either `doca-pcc-ecn/` tree.

After conversion the only `/* ... */` left in either tree is the exempt call-site idiom — two
occurrences, both in `doca_flow_nop.c`'s `main`. That is the check to run after any future pass:

```bash
grep -n '/\*' doca-*/doca-flow/*.c doca-*/doca-flow/*.h   # expect only /*with_counter=*/
```

`doca_flow_compat.h` was the one file where converting it risked *widening* a divergence:
`meson.build` claims it is identical in every `doca-*/` directory "on purpose", and the copies also
differed in `#include` ordering. With `doca-3.2/` gone there were only two left, so doca-2's
converted version was copied verbatim over `doca-3.4`'s and the two are now byte-identical — the
claim in `meson.build` is true again. Keep it that way: copy, do not re-convert independently.

Note the macro `DOCA_TUT_MIRROR_SET_ORIG_FWD` is dead code in `doca-3.4` (3.4 has no mirror at all).
It stays only to keep the file identical across trees, which is the stated policy.

The conversion is scriptable, in two passes: first move trailing comments onto their own line (the
converter below), then fold every `/* ... */` block into consecutive `//` lines. Always verify by
stripping comments from before and after and comparing — a `sed` one-liner is not good enough here,
because it mangles string literals containing `//` or `/*`, so use a real tokenizer:

```python
# strip.py — prints True if two files are identical once comments and whitespace are removed
import sys, pathlib
def strip(path):
    s, out, i, n = pathlib.Path(path).read_text(), [], 0, 0
    n = len(s)
    while i < n:
        c = s[i]
        if c in '"\'':                                  # literal: copy verbatim
            q = c; out.append(c); i += 1
            while i < n:
                out.append(s[i])
                if s[i] == '\\': out.append(s[i+1]); i += 2; continue
                if s[i] == q: i += 1; break
                i += 1
            continue
        if s.startswith("//", i):
            while i < n and s[i] != "\n": i += 1
            continue
        if s.startswith("/*", i):
            j = s.find("*/", i+2); i = (j+2) if j != -1 else n
            continue
        out.append(c); i += 1
    return "".join("".join(out).split())
print(strip(sys.argv[1]) == strip(sys.argv[2]))
```

```python
import re, pathlib
p = pathlib.Path("FILE.c")
out, in_block, flagged = [], False, []
standalone = re.compile(r'^(\s*)/\*\s*(.*?)\s*\*/\s*$')
trailing   = re.compile(r'^(\s*)(.*\S)\s+/\*\s*(.*?)\s*\*/\s*$')
for i, line in enumerate(p.read_text().split("\n"), 1):
    if in_block:
        out.append(line)
        if "*/" in line: in_block = False
        continue
    if "/*" in line and "*/" not in line:          # opens a block comment
        in_block = True; out.append(line); continue
    m = standalone.match(line)
    if m:
        out.append("%s// %s" % (m.group(1), m.group(2))); continue
    m = trailing.match(line)                        # only matches comments at end-of-line,
    if m:                                           # so /*arg=*/value mid-expression is safe
        indent, code, comment = m.groups()
        if code.strip().startswith("#endif") or code.strip() in ("}", "{", "};"):
            flagged.append((i, line.strip())); out.append(line); continue
        out.append("%s// %s" % (indent, comment.rstrip()))
        out.append("%s%s" % (indent, code)); continue
    out.append(line)
p.write_text("\n".join(out))
print("left for a human:", flagged or "nothing")
```

Anything the script flags is a comment attached to a brace, which needs a human — the one case in
`doca_flow_ecn_pcap.c` was `} /* per-entry value */` closing an `if (mark)` block, and the comment
belonged above the `if`, not above the `}`.

### 4. The participant template

`doca_flow_template.c` is `doca_flow_ecn_pcap.c` with five function bodies emptied:
`create_to_cpu_pipe`, `bind_capture_mirror`, `create_forward_to_sf_pipe`, `create_sampling_pipe`,
`create_root_pipe`. `create_passthrough_pipe` stays implemented as the worked example, and
`build_pipeline` stays implemented so the topology is given.

Also added: a `flow_template` executable in `doca-flow/meson.build`, and
`doca_flow_template.README.md` (draft guidance — superseded once a proper participant guide
exists, and no longer referenced from the code).

#### Invariants that make it work

These were each arrived at the hard way. Breaking any of them makes the template worse in a way
that is not obvious until someone uses it.

- **The template is byte-identical to the solution except the five bodies.** Same file header, same
  `DOCA_LOG_REGISTER` tag, same `doca_argp_init` program name. Consequence: `--help` and the log
  lines read `doca_flow_ecn_pcap` in both binaries. That is deliberate — it keeps
  `diff doca_flow_template.c doca_flow_ecn_pcap.c` down to the participant's own work.
- **No comment ever differs.** Guidance goes in the guide, not in the source, or the diff fills up
  with comment churn instead of code.
- **`build_pipeline` stays implemented.** With it present and the five stubs returning `NULL`,
  there are no DOCA calls to fail: the unmodified template *compiles and runs*, reporting
  `CE marked: 0` and forwarding nothing. Participants get a working program at minute zero and fill
  in one pipe at a time. Empty the assembly too and it dies during setup instead.
- **Each stub body is two lines — a comment plus an explicit `return`.** A lone comment, or a
  comment with a trailing `return NULL;`, is short enough that clang-format collapses the whole
  function onto one line (Google style sets `AllowShortFunctionsOnASingleLine: All`). The explicit
  `return;` in the two `void` stubs exists only to prevent that.
- **Regenerate rather than hand-edit.** After any change to the solution, re-derive the template so
  the two cannot drift.

#### Regeneration recipe

Run from the version's `doca-flow/` directory. Re-derives the template from the current solution,
so it also serves as the port to another tree — only the function-name list changes.

```bash
cp doca_flow_ecn_pcap.c doca_flow_template.c && python3 - <<'PY'
import pathlib
p = pathlib.Path("doca_flow_template.c")
lines = p.read_text().split("\n")
for name, n, ret in [("create_to_cpu_pipe", 1, "  return NULL;"),
                     ("bind_capture_mirror", 2, "  return;"),
                     ("create_forward_to_sf_pipe", 3, "  return NULL;"),
                     ("create_sampling_pipe", 4, "  return NULL;"),
                     ("create_root_pipe", 5, "  return;")]:
    body = ["  /* TODO %d -- your code here. */" % n, ret]
    start = next(i for i, l in enumerate(lines) if l.startswith("static") and (name + "(") in l)
    o = start
    while not lines[o].rstrip().endswith("{"):
        o += 1
    c = o + 1
    while lines[c] != "}":
        c += 1
    lines[o + 1:c] = body
p.write_text("\n".join(lines))
PY
clang-format -i doca_flow_template.c
diff -u doca_flow_ecn_pcap.c doca_flow_template.c   # expect only the five bodies
```

For `doca-3.4` the capture path is a **hash-flooding pipe**, not a shared mirror, so the function
list differs — `create_flood_pipe` replaces `bind_capture_mirror` as TODO 2, and there is no
`MIRROR_ID` binding to write. The exercise is otherwise the same shape, and the substitution in the
recipe above is the only change needed:

```python
for name, n, ret in [("create_to_cpu_pipe", 1, "  return NULL;"),
                     ("create_flood_pipe", 2, "  return NULL;"),      # was bind_capture_mirror
                     ("create_forward_to_sf_pipe", 3, "  return NULL;"),
                     ("create_sampling_pipe", 4, "  return NULL;"),
                     ("create_root_pipe", 5, "  return;")]:
```

Note TODO 2 returns `NULL` here rather than being `void`, so `doca-3.4` has one `void` stub where
`doca-2` has two. The "two lines per body" invariant still holds either way.

One gotcha seen while reformatting: clang-format never re-joins adjacent string literals, so if the
solution and template were split at different points by an earlier column limit, they stay
different. Regenerating the template from the freshly formatted solution fixes it; reformatting the
two files independently does not.

## What `doca-3.4` required differently

Everything else was ported verbatim — same comments, same function order, same structure. These are
the only divergences, and each is forced by the DOCA version. `doca_flow_ecn_pcap.README.md` carries
the same table for tutorial readers.

| | `doca-2` | `doca-3.4` |
| --- | --- | --- |
| copy to the pcap | shared mirror via `monitor.shared_mirror_id` | `DOCA_FLOW_PIPE_HASH` + `..._ALGORITHM_FLOODING` |
| SF representor | DPDK port index as a devargs string | `doca_dev_rep` from `doca_devinfo_rep_create_list` |
| port config | `doca_flow_port_cfg_set_devargs` | `..._set_port_id` + `..._set_actions_mem_size` |
| entry install | `doca_flow_pipe_add_entry`, `action_idx` in `doca_flow_actions` | `doca_flow_pipe_basic_add_entry`, explicit `action_idx` arg + `DOCA_FLOW_ENTRY_FLAGS_*` |
| RSS forward | flat `fwd.rss_queues` / `num_of_queues` / `rss_outer_flags` | nested `fwd.rss.queues_array` / `nr_queues` / `outer_flags` |
| source port match | `parser_meta.port_meta` (`uint32`) | `parser_meta.port_id` (`uint16`) |
| mode args | `switch,hws,isolated,disable_switch_rss` + `rte_flow_isolate()` | `switch,hws`, no `rte_flow_isolate()` |
| probe args | `…,repr_matching_en=0,representor=sf%u` | `…,representor=sf%u` |
| EAL allowlist | one dummy `-a pci:00:00.0`, argc cap 62 | also `-a auxiliary:`, argc cap 60 |

Two consequences worth remembering:

- **`build_pipeline` builds `PASSTHROUGH` first in 3.4.** Both flooding entries must share one fwd
  *type* (`FWD_PIPE`), so entry 0 reaches the SF *through* `PASSTHROUGH` rather than via `FWD_PORT`.
  That makes `PASSTHROUGH` a dependency of `FLOOD`, so it has to exist before it. In `doca-2` the
  mirror imposes no such ordering and it is built after the capture path.
- **`doca-3.4/doca_flow_nop.c` regained `--sf-num`.** It had drifted: no `--sf-num` flag, no
  `find_sf_representor_port_id`, hardcoded ring sizes, `create_port_demux_pipe` instead of
  `create_root_pipe`. All of that came back from `doca-2`. Its `open_sf_representor` now selects the
  representor whose SF index equals `--sf-num` instead of taking the first one with any SF index —
  identical behaviour on a `setup_roce_loopback.sh` DPU (one SF, sfnum 0), but **this specific
  change is untested on hardware.**

## Verification status

| | builds | runs | end-to-end |
| --- | --- | --- | --- |
| `doca-2` solution | yes, 0 warnings | yes | **passes** — 92.59 → 0.08 Gb/s, 7314/7314 CE |
| `doca-2` template | yes, 0 warnings | yes — counters at 0, blackholes traffic (see below) | n/a |
| `doca-3.4` solution | yes, 0 warnings | not run | never |
| `doca-3.4` template | yes, 0 warnings | not run | never |
| `doca-3.4` nop | yes, 0 warnings | **not run since the port** | never |

`bf3-uwaterloo-1` is the only DOCA 3.4 machine in the fleet, and it was in use, so the 3.4 build was
verified by compiling only — `doca-3.4/meson.build` plus `doca-3.4/doca-flow/` copied to a scratch
directory on that host and built there, leaving its checkout untouched. All three targets linked
with zero warnings. `doca-pcc-ecn` was excluded (meson skips a missing app directory with a
warning); it is unchanged by this work, but that also means **dpacc was not re-run**.

To reproduce, or to build the whole tree once the machine is free:

```bash
export PKG_CONFIG_PATH=/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig
cd doca-3.4 && meson setup build && ninja -C build
```

That `PKG_CONFIG_PATH` is required on a native (non-container) build: `doca-common`, `doca-argp` and
`doca-flow` resolve from the default path, but `doca-dpdk-bridge` and `libdpdk` do not, and meson
merely *warns* and skips the whole `doca-flow` app rather than failing. A build that reports
`Build targets in project: 0` has silently built nothing — check that line, not just the exit code.

`doca-2` was re-tested end to end after every change above, including the column-limit reformat
that rewrote 114 lines of `doca-2/doca-pcc-ecn/device/algo/` — the DPA congestion controller itself.
That one mattered: a clean dpacc build proves the algo compiles, not that it still collapses
throughput. It does. The numbers are unchanged from before the rework began (92.60 → 0.08,
7457/7457 CE then; the CE totals differ only because each run captures a different window, and both
are 100%).

Re-run after any further change with:

```bash
./admin/fleet.py test-tutorial bf3-ulisbon-1
```

### Still to do on `doca-3.4`

Nothing in `doca-3.4` has been run since the port — only compiled. When `bf3-uwaterloo-1` is free:

- Run the solution end to end and fill in the table
  (`./admin/fleet.py test-tutorial bf3-uwaterloo-1`).
- Confirm the flooding pipe actually captures. The counter line reads `flooded:` where 2.x read
  `mirrored:`, and it has never been observed to move — the whole mirror→flood substitution is
  unverified on hardware.
- Re-check `doca_flow_nop`, which changed most in the port (see the `--sf-num` note above).
- Build `doca-pcc-ecn` too, so dpacc runs; the compile-only check above skipped it.

### The template blackholes traffic, by design

Measured, with a no-app control to attribute it correctly:

| configuration | `ping` ns1→ns0 | `ib_write_bw` |
| --- | --- | --- |
| no DOCA app at all | — | 92.60 Gb/s (kernel/OVS path) |
| **unmodified template** | **100% loss** | **never connects** |
| `doca_flow_nop` | 0% loss | 92.60 Gb/s |

`doca_flow_port_start` takes ownership of PF0's eSwitch whether or not any pipe exists, so a
template with `create_root_pipe` stubbed out drops everything. Killing it restores the link
immediately. This is why `doca_flow_nop` must stay: it is the only worked example of a *complete
minimal* pipeline, which blank stubs cannot demonstrate.

Note the control result — the kernel path already does 92.60 Gb/s, the same figure the full app
reaches. On this testbed "traffic flows at line rate" is therefore **not** evidence that a DOCA app
is doing anything. Always check the app's own counters as well.
