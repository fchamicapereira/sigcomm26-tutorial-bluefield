# `doca_flow_template` — superseded by the participant guide

The exercise built from [`doca_flow_template.c`](doca_flow_template.c) is documented in
**[`guides/doca-flow.md`](../../guides/doca-flow.md)** (rendered to `guides/doca-flow.pdf`, which is
what participants are handed). That guide covers both trees, `doca-2` and `doca-3`.

This file used to carry a draft of that guidance. It was removed rather than kept in step, because
it had already drifted: it described **five** TODOs including `create_to_cpu_pipe`, which the
template now gives away as a worked example, and referred to local variable names that no longer
exist in the template. Two sets of instructions for one exercise is worse than one.

For the record of how the template is derived from the solution — and the recipe for regenerating it
after any change — see [`docs/porting-doca-2-changes.md`](../../docs/porting-doca-2-changes.md).
