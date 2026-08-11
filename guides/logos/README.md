# Institution logos

One file per `id` in `../tutorial.json`. The guides build without them — a missing logo renders as
a small framed box naming the file that is wanted, visible in the PDF — so this directory can be
filled in gradually. Those placeholders should not survive to publication.

| File | Institution |
|---|---|
| `inesc-id.pdf` | INESC-ID, Instituto Superior Técnico, University of Lisbon |
| `michigan.pdf` | University of Michigan |
| `waterloo.pdf` | University of Waterloo |
| `nvidia.pdf` | NVIDIA |
| `purdue.pdf` | Purdue University |
| `washington.pdf` | University of Washington |

## Format

**`.pdf` is strongly preferred, `.png` works.** The template tries `.pdf` first, then `.png`, then
gives up and draws the placeholder. Logos are set inline at about 2.1ex — roughly the height of a
capital letter — so a vector PDF stays crisp at any zoom while a small raster goes soft. If all you
can get is SVG, convert it once:

```bash
# rsvg-convert (brew install librsvg) — or Inkscape: inkscape in.svg --export-filename=out.pdf
rsvg-convert -f pdf -o michigan.pdf michigan.svg
```

Crop tightly to the mark. Whitespace baked into the file becomes visible padding next to someone's
name, and there is no way to trim it from the template.

Wordmarks generally read better than seals at this size: at 2.1ex a detailed crest turns to mush,
while a horizontal wordmark stays legible.

## Sourcing

Take these from each institution's own brand or identity page rather than from an image search.
Every one of these marks is a trademark, and the brand pages carry both the approved artwork and
the usage rules. Use of institutional logos to identify affiliation on academic material is
ordinary and expected, but the rules do vary — NVIDIA's in particular are worth reading, since it
is a company rather than a university.

If an institution's guidelines turn out not to permit this use, the honest fallback is to drop the
logo and let that person's affiliation read as text; tell the organizer from that institution
rather than working around it.
