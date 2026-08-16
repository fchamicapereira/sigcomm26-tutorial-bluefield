# `doca_flow_template` — the participant exercise

The exercise built from [`doca_flow_template.c`](doca_flow_template.c) is documented in
**[`guides/doca-flow.md`](../../guides/doca-flow.md)** (rendered to `guides/doca-flow.pdf`, which is
what participants are handed). That guide covers both trees, `doca-2` and `doca-3`, and is the only
description of the exercise. This file used to carry a second one; it drifted, and was removed.

**Do not hand-edit `doca_flow_template.c`.** It is generated from
[`doca_flow_solution.c`](doca_flow_solution.c) by
[`admin/local_scripts/regen_templates.py`](../../admin/local_scripts/regen_templates.py), whose
docstring records exactly what it cuts and why. Change the solution, then re-run the script; its
`--check` mode verifies the two are in step and is worth running before handing anything out.

The four flow programs in this directory:

| File                   | What it is                                                                     |
| ---------------------- | ------------------------------------------------------------------------------ |
| `doca_flow_solution.c` | ECN marking and random sampling with hardware counters. The answer key, and the file the template is derived from. |
| `doca_flow_template.c` | The same with three function bodies hollowed out to `TODO`s — the exercise. Generated. |
| `doca_flow_ecn_pcap.c` | The solution plus a hardware copy of the traffic to a pcap file: a shared mirror on 2.x, a flooding hash pipe on 3.x. Not part of the exercise, hand-maintained, and the only one that links libpcap. It ships to the participants' `-solutions/` directory so they can see a CE mark on the wire. |
| `doca_flow_nop.c`      | A standalone minimal forwarder, older than the rest. The exercise no longer needs it — the template carries its own `create_root_pipe_nop()` — and it does not ship to participants. |
