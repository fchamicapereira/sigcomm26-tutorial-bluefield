# Guide figures

Figures used by the guides in this directory that are **not** generated from this repository.
Anything built from a `.dot` in `../../docs/` is referenced straight from there instead
(`../docs/*.png`), so it stays in step with its source.

| File | Source | Used by |
|---|---|---|
| `nvidia-doca-flow-pipes.png` | NVIDIA, [DOCA Flow programming guide](https://networking-docs.nvidia.com/doca/archive/3-4-0/doca-flow), "Architecture" | `doca-flow.md` §1 |
| `nvidia-switch-mode.png` | NVIDIA, [DOCA Flow programming guide](https://networking-docs.nvidia.com/doca/archive/2-9-0/doca-flow), "Domains in Switch Mode" | `doca-flow.md` §1 |

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
