# Guide figures

Figures used by the guides one level up that are **not** generated from this repository. Anything
built from a `.dot` in `../../docs/` is referenced straight from there instead (`../docs/*.png`), so it
stays in step with its source.

## Reproduced from NVIDIA

| File | Source | Used by |
|---|---|---|
| `nvidia-doca-flow-pipes.png` | NVIDIA, [DOCA Flow programming guide](https://networking-docs.nvidia.com/doca/archive/3-4-0/doca-flow), "Architecture" | `doca-flow.md`, Step 2 |
| `nvidia-switch-mode.png` | NVIDIA, [DOCA Flow programming guide](https://networking-docs.nvidia.com/doca/archive/2-9-0/doca-flow), "Domains in Switch Mode" | `doca-flow.md`, Step 4 |

Only these two are covered by [Re-fetching](#re-fetching) and [Attribution](#attribution) below.

## Drawn for the path-steering guide

| File | Used by |
|---|---|
| `simple-multi-path-topology.png` | `pcc-path-steering.md`, Step 1.1 — the single-QP multi-path scenario that does not work |
| `paired-multi-path-topology.png` | `pcc-path-steering.md`, Step 1.2 — the two-QP arrangement that does |
| `end-to-end-data-path-dual-flow.png` | `pcc-path-steering.md`, Step 1.3 — the dual-flow data path mapped onto one card |

These three arrived with the path-steering exercise (commit `a33370c`, "freeze and move pcc path
steering") as finished PNGs. **No generator source for them exists in this repository** — they are not
`.dot` renders and there is no editable original checked in, so they cannot be regenerated here; edit
them as images or redraw. The first two are set at 25% width via inline `<img>` tags rather than
Pandoc attributes, which is why they are the only figures in the guides written as raw HTML.

## Re-fetching

Both are attachments on the pages above; the hash in the URL is content-addressed and changes when
NVIDIA re-exports the image, so re-derive the link from the page rather than reusing the one below
if it ever 404s.

```bash
base=https://networking-docs.nvidia.com/doca/__attachments
curl -o nvidia-doca-flow-pipes.png \
  "$base/a_9b510b82725faa7e2499e5651df304dd9e736cf992b722728909464c7d6565b0/architecture-diagram.png"
curl -o nvidia-switch-mode.png \
  "$base/a_3b02639eaec1bcdba3ee151bb7a937bc5daa0d49c794b9926e7b583c50e4e09c/switch-mode-diagram.png"
```

## Attribution

These are NVIDIA's own figures, reproduced to explain NVIDIA's own product to people about to use
it, and each is captioned with a link back to the page it came from. Keep the caption credit on any
figure added here — it is the whole basis on which we are using them. If a figure ever needs to
carry more than that (a specific licence line, say), put it in the caption too rather than only
here, since the PDF is what gets handed out and this file is not.

`nvidia-switch-mode.png` is only 424x279. That is the resolution NVIDIA publishes; it is set small
in the guide so it does not go soft. Do not upscale it.
