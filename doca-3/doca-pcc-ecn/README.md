# doca-pcc-ecn (DOCA 3.x) — the pure-ECN congestion controller

The DOCA 3.x build of the pure-ECN (DCQCN-style) reaction-point controller. Same program, same two
executables, same exercise as the 2.x tree — **the full write-up is
[`doca-2/doca-pcc-ecn/README.md`](../../doca-2/doca-pcc-ecn/README.md)**, and the participant-facing
walkthrough is [`guides/doca-pcc.md`](../../guides/doca-pcc.md). This file records only what the 3.x
toolchain forced to be different.

Verified on DOCA 3.1 and 3.4.

## Build

```bash
cd doca-3
meson setup build
ninja -C build
# => build/doca-pcc-ecn/doca_pcc_ecn_rp           the finished controller
# => build/doca-pcc-ecn/doca_pcc_ecn_rp_template  the exercise (TODO 1 and TODO 2 unfilled)
```

Running it, the firmware prerequisites (`USER_PROGRAMMABLE_CC=1`, `DPA_AUTHENTICATION=0` as *Current*
values), the `-R`-is-mandatory rule and the tuning knobs are all identical — see the 2.x README.

## What differs from the 2.x tree

The host loader (`host/pcc_ecn_rp.c`) is **byte-for-byte identical** to the 2.x one. Everything below
is in the device half or the build.

| | `doca-2` | `doca-3` |
|---|---|---|
| RTT algo param | `RTT_TEMPLATE_BASE_RTT` / `BASE_RTT` | `RTT_TEMPLATE_BASE_RELATIVE_RTT` / `BASE_RELATIVE_RTT` (renamed in the 3.x device API) |
| DPA process attributes | not required | `device/dpa_app_attributes.yaml` → blob via `dpa-app-attributes2blob`, passed to `dpacc` as `--dpa-proc-attr` |
| host-side stubs | inside `dpacc`'s own archive | emitted as C into `--keep-dir`, compiled and archived separately |
| archive meson links | `dpacc`'s `.a` directly | `<build>/<app_name>/<app_name>.a`, the one the script builds |
| extra header | — | `device/pcc_common_dev.h` |

Two of those rows exist only from **DOCA 3.2 onwards**. DOCA 3.1 has neither
`dpa-app-attributes2blob` in its tools directory nor a `--dpa-proc-attr` flag on `dpacc`, and its
`dpacc` emits one complete archive exactly like the 2.x toolchain does.

`build_device_code.sh` therefore **picks its path from what is actually installed rather than from a
DOCA version string** — the same rule it already used to choose between untargeted and per-target
`libdoca_pcc_dev*.a`. Either way it leaves the archive meson links at
`${BUILD_DIR}/${APP_NAME}/${APP_NAME}.a`, so `meson.build` does not need to know which toolchain
produced it. It also selects the first `dpacc_mcpu` target on 3.1, whose compiler does not accept the
newer comma-separated multi-target form.

The `dpa_app_attributes.yaml` we ship declares the two features this controller needs:

```yaml
Feature_list:
  Congestion_Control_Flow_Context_Access: true
  Programmable_Congestion_Control: true
```

## The exercise

As in the 2.x tree, `device/algo/rtt_template_exercise.c` is **generated** — do not hand-edit it:

```bash
admin/make_pcc_exercise.py doca-3            # rewrite the exercise from the controller
admin/make_pcc_exercise.py doca-3 --check    # verify the two are in step
```

The two TODO bodies are identical across versions (only the dormant RTT handler differs), so one set
of substitutions in [`admin/make_pcc_exercise.py`](../../admin/make_pcc_exercise.py) serves both
trees.

`update_participants_repo_on_github.py` ships the exercise renamed to `rtt_template.c` and drops the
`_template` executable block from `meson.build`, so a participant sees one algorithm file and one
binary — never the answer key.
