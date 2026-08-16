# Changes made to `doca-2`, to be applied to the other tutorial versions

Written 2026-08-12. `doca-2/doca-flow/` was reworked to carry both the solution and a participant
exercise. Some of the work landed in every tree, some only in `doca-2`. This is the record of what
went where, and how to repeat it.

**Status: the port is done.** The `doca-3` unification landed first, so
`doca-3.2/` is gone and `doca-2/` and `doca-3/` are the only two trees. Everything below has now
been applied to `doca-3/doca-flow/` as well; the sections are kept as the record of *what* was
changed and *why*, and as the recipe for any future tree. See
[What `doca-3` required differently](#what-doca-3-required-differently) for the deltas that are
forced by the DOCA version.

## Already applied everywhere

| change | where |
| --- | --- |
| `crash_if_unsuccessful` → `doca_check` | `doca-2`, `doca-3.2`, `doca-3` — all 348 call sites (`doca-3.2` has since been removed) |
| clang-format `ColumnLimit: 140` → `100` | all 24 `.c`/`.h` files in the repo |

Both were mechanical. The rename kept the `__attribute__((format(printf, 2, 3)))` on the helper,
which is what type-checks the format string in those 348 messages — worth re-confirming after any
future rename, by deliberately breaking one call and checking the warning still fires.

## The work, and how it was ported

### 1. Drop the file header comment block

The 17-line `/* ... */` banner at the top of `doca_flow_ecn_pcap.c` is gone; the file now starts at
`#include <doca_argp.h>`. The topology diagram it carried still lives in
`doca_flow_ecn_pcap.README.md`. Same treatment applied to both `doca-3` files.

`doca-3` had no `doca_flow_ecn_pcap.README.md`, so dropping its banner would have deleted the
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

Applied to all of `doca-2/doca-flow/` and all of `doca-3/doca-flow/`: `doca_flow_ecn_pcap.c`,
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
converted version was copied verbatim over `doca-3`'s and the two are now byte-identical — the
claim in `meson.build` is true again. Keep it that way: copy, do not re-convert independently.

Note the macro `DOCA_TUT_MIRROR_SET_ORIG_FWD` is dead code in `doca-3`: nothing there uses the
mirror. On 3.2+ it could not — the mirror is gone from the API — while 3.1 still has it and simply
does not use it, so that one source builds on every 3.x. The macro stays only to keep the file
identical across trees, which is the stated policy.

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

`doca_flow_template.c` is derived from `doca_flow_ecn_pcap.c` by
[`admin/local_scripts/regen_templates.py`](../admin/local_scripts/regen_templates.py). **Never
hand-edit it** -- run the script, which rebuilds both trees' templates and has a `--check` mode that
fails if a solution has moved on without one.

Two differences from the solution, and no others:

1. **Three bodies are emptied** to a `TODO n` stub. The numbers are the order the guide asks for
   them, not the order they appear in the file:

   | | `doca-2` | `doca-3` | |
   | --- | --- | --- | --- |
   | `TODO 1` | `create_forward_to_sf_pipe` | `create_forward_to_sf_pipe` | the CE marking |
   | `TODO 2` | `bind_capture_mirror` | `create_flood_pipe` | the capture copy |
   | `TODO 3` | `create_sampling_pipe` | `create_sampling_pipe` | the 1-in-N split |

2. **`build_pipeline` is replaced** with a version wired as a plain forwarder, with the ECN pipeline
   present but commented out.

`create_passthrough_pipe`, `create_to_cpu_pipe` and `create_root_pipe` are all given, as worked
examples.

#### The template runs as `doca_flow_nop`

This replaced an earlier design in which the untouched template **blackholed every packet**, because
`doca_flow_port_start` takes ownership of PF0's eSwitch whether or not any pipe exists and there was
no root pipe. That was defensible -- it taught "your program owns the data path" on the first run --
but it meant a participant's first experience was a program that broke the link, and the guide had
to spend a paragraph insisting this was intentional.

Now the template ships wired as a no-op forwarder: `create_root_pipe(port, passthrough)` and nothing
else. Run it and traffic flows at line rate, unmarked. The exercise begins by commenting out that
one line and uncommenting the ECN block under it.

That is why `create_root_pipe` had to stop being a TODO -- the no-op configuration needs it. It is
also why the old rationale for which functions could be given away no longer applies: it used to be
that `create_to_cpu_pipe` was the only safe one, since it takes just `port`.

**Expect unused-function warnings on a fresh template.** With the ECN block commented out,
`get_random_mask`, `create_to_cpu_pipe` and the three TODO functions are never referenced, and
`-Wall` reports each as "defined but not used". Five warnings, and they clear as the participant
uncomments. The guide tells them to expect this. **Not verified on a card** -- reasoned from
`-Wall` semantics, since meson's default `warning_level=1` adds it.

#### Invariants that make it work

These were each arrived at the hard way. Breaking any of them makes the template worse in a way
that is not obvious until someone uses it.

- **The template is byte-identical to the solution except the three bodies and `build_pipeline`.**
  Same file header, same `DOCA_LOG_REGISTER` tag, same `doca_argp_init` program name. Consequence:
  `--help` and the log lines read `doca_flow_ecn_pcap` in both binaries. That is deliberate -- it
  keeps `diff doca_flow_template.c doca_flow_ecn_pcap.c` down to the participant's own work plus one
  function they were told about.
- **No comment ever differs**, outside `build_pipeline`. Guidance goes in the guide, not the source,
  or the diff fills up with comment churn instead of code.
- **The unmodified template compiles, runs, and forwards.** Participants get a working program at
  minute zero. Empty `build_pipeline` too and it forwards nothing, which is where this started.
- **Each stub body is two lines -- a comment plus an explicit `return`.** A lone comment, or a
  comment with a trailing `return NULL;`, is short enough that clang-format collapses the whole
  function onto one line (Google style sets `AllowShortFunctionsOnASingleLine: All`). The explicit
  `return;` in a `void` stub exists only to prevent that.
- **Regenerate rather than hand-edit.** After any change to a solution, re-run the script.

#### Regeneration

```bash
admin/local_scripts/regen_templates.py            # rewrite both templates
admin/local_scripts/regen_templates.py --check    # verify, change nothing (use before a release)
```

The script holds the no-op `build_pipeline` text for both trees, the per-tree stub lists, and runs
`clang-format -i` afterwards. The one gotcha it inherits: clang-format never re-joins adjacent string
literals, so if a solution and its template were split at different points by an earlier column
limit they stay different. Regenerating from the freshly formatted solution fixes it; reformatting
the two files independently does not.


## What `doca-3` required differently

Everything else was ported verbatim — same comments, same function order, same structure. These are
the only divergences, and each is forced by the DOCA version. `doca_flow_ecn_pcap.README.md` carries
the same table for tutorial readers.

| | `doca-2` | `doca-3` |
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
- **`doca-3/doca_flow_nop.c` regained `--sf-num`.** It had drifted: no `--sf-num` flag, no
  `find_sf_representor_port_id`, hardcoded ring sizes, `create_port_demux_pipe` instead of
  `create_root_pipe`. All of that came back from `doca-2`. Its `open_sf_representor` now selects the
  representor whose SF index equals `--sf-num` instead of taking the first one with any SF index —
  identical behaviour on a `setup_roce_loopback.sh` DPU (one SF, sfnum 0), but **this specific
  change is untested on hardware.**

## `doca-3/` covers every DOCA 3.x

Added after the port, so the exercise could be tested on `bf3-uwashington-1`/`-2` while the only 3.4
machine was busy. The directory was called `doca-3.4/` until this landed and was renamed once it
served more than one release — it now stands in the same relationship to DOCA 3.x that `doca-2/`
does to 2.7 and 2.9, and `test_tutorial.sh` maps every `3.*` to it.

`run_container.sh` takes the directory suffix as its argument and discovers the available ones from
`doca-*/`, so the invocation is now `./run_container.sh 3`; `./run_container.sh 3.4` no longer
exists.

The capture path did **not** have to change: `DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_FLOODING`,
`doca_flow_pipe_cfg_set_hash_map_algorithm`, `DOCA_FLOW_FWD_HASH_PIPE` and
`doca_flow_pipe_hash_add_entry` all exist in 3.1, and `struct doca_flow_fwd` is identical between
the two releases. So is `parser_meta.port_id`, and so are `port_cfg_set_port_id`,
`..._set_actions_mem_size` and `..._set_dev_rep`. 3.1 still has the shared mirror, but flooding
works there, so both releases run the same code.

Three shims in `doca_flow_compat.h` cover the whole difference:

| | DOCA 3.1 | DOCA 3.2+ |
| --- | --- | --- |
| basic entry | `doca_flow_pipe_add_entry`, index in `doca_flow_actions.action_idx` | `doca_flow_pipe_basic_add_entry`, `action_idx` argument |
| hash entry | `doca_flow_pipe_hash_add_entry`, no `action_idx`, `flags` is an enum | same name, plus `action_idx`, `flags` is `uint32_t` |
| entry flags | `DOCA_FLOW_NO_WAIT` = 0, `DOCA_FLOW_WAIT_FOR_BATCH` = 1 | `DOCA_FLOW_ENTRY_FLAGS_NO_WAIT` = 1, `..._WAIT_FOR_BATCH` = 2 |

The flag **values** differ as well as the names, so the shim maps by name and never by number. The
hash-entry wrapper is defined under its own name and aliased with a `#define` afterwards, because
the 3.4 name collides with the real 3.1 function.

### The DPA toolchain, which was the harder half

`doca-pcc-ecn` needed a build change, not a code change. DOCA 3.2 introduced
`dpa-app-attributes2blob` and dpacc's `--dpa-proc-attr`; **3.1 has neither** — its dpacc does not
list `--dpa-proc-attr` at all. It behaves like the 2.x toolchain: one archive out of dpacc that
already exports `pcc_ecn_rp_app`, with no separate host-stub archiving step.

`build_device_code.sh` now picks its path from whether `dpa-app-attributes2blob` is present, the
same "check the filesystem, not a version string" rule the DPA-library choice already used. Both
paths leave the linkable archive at `${KEEP_DIR}/${APP_NAME}.a`, so `meson.build` does not have to
know which toolchain ran. The device sources themselves compile unmodified on 3.1.

A first attempt made `doca-pcc-ecn` *skip* itself on 3.1 instead. That was wrong and was reverted —
worth recording because the failure mode is not obvious: `run_command(check: true)` fails at
**configure** time, so an unbuildable PCC app takes `doca-flow/` down with it and nothing at all
builds on that box. If a future release does need skipping, skip it in `meson.build` with
`subdir_done()`, never by letting `run_command` fail.

## Verification status

| | builds | runs | end-to-end |
| --- | --- | --- | --- |
| `doca-2` solution | yes, 0 warnings | yes | **passes** — 92.59 → 0.08 Gb/s, 7314/7314 CE |
| `doca-2` template | yes, 0 warnings | yes — counters at 0, blackholes traffic (see below) | n/a |
| `doca-3` on DOCA 3.1 | yes, 0 warnings — all 4 targets incl. dpacc | yes | **passes** — 184.41 → 0.08 Gb/s, 15114/15114 CE |
| `doca-3` on DOCA 3.4 | yes, 0 warnings — all 4 targets incl. dpacc | not run | never |

Both builds were verified by copying the tree to a scratch directory on the target and building
there, leaving each machine's own checkout untouched: `bf3-uwaterloo-1` for 3.4 (it was in use) and
`bf3-uwashington-1` for 3.1. "All 4 targets" means `doca_flow_nop`, `doca_flow_ecn_pcap`,
`doca_flow_template` and `doca_pcc_ecn_rp` — so dpacc ran on both, down each of its two paths.

### The flooding capture path works

Run end to end on `bf3-uwashington-1` (DOCA 3.1), every phase passing:

```
packets flowing     CE marked 396125 -> 225509091
baseline throughput 184.41 Gb/s
pcap ecn marking    15114/15114 captured IPv4 packets are CE-marked
pcc controller      loaded on mlx5_1
rate collapse       184.41 -> 0.08 Gb/s
```

That settles the open question from the port: **the hash-flooding pipe really does deliver a copy
to the CPU queue**, and the copy is taken *after* the CE action, exactly as the 2.x shared mirror
did — 15114 of 15114 captured packets are CE-marked at `--percent 100`, with zero `not_ect`,
`ect0`, `ect1` or truncated. `flooded:` moves. The mirror→flood substitution is no longer
theoretical.

Note the baseline: **184.41 Gb/s**, where `doca-2` on `bf3-ulisbon-1` measures 92.60. That is the
testbed, not the port — a different link configuration on the uwashington boxes. Do not read the
two numbers against each other; compare each machine to its own no-app control.

This exercises the compat header's *shim* path (`doca_flow_pipe_add_entry`, pre-3.2 flags) and the
pre-3.2 dpacc path. The flooding code itself is identical on both releases, so 3.4 still needs a
run to prove the native `basic_add_entry` path and the blob-based DPA build — but nothing
version-specific about the capture design remains untested.

To reproduce, or to build the whole tree once the machine is free:

```bash
export PKG_CONFIG_PATH=/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig
cd doca-3 && meson setup build && ninja -C build
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

### Still to do on `doca-3`

- Run it end to end on 3.4 once `bf3-uwaterloo-1` is free
  (`./admin/fleet.py test-tutorial bf3-uwaterloo-1`). The 3.1 pass does not cover the native
  `doca_flow_pipe_basic_add_entry` path or the blob-based dpacc build; those are the only two
  things 3.4 does differently.
- Run `doca_flow_nop` by hand on either release. `test-tutorial` never launches it, so the file
  that changed most in the port is still the one with the least runtime coverage — in particular
  its `--sf-num` handling and the `open_sf_representor` SF-index match.
- Run the template by hand and confirm it still blackholes traffic by design (the `doca-2`
  behaviour below), so participants see the same thing on a 3.x box.

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
