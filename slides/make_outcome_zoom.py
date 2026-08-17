#!/usr/bin/env python3
"""Annotate the outcome screenshots: a magnifier popup, and arrows where a part needs them.

    ./make_outcome_zoom.py        write images/<part>-zoom.png for every job below

The raw captures are 3024px wide. On a 1280x720 stage they render about 830px across, which puts
terminal text at roughly five pixels per character -- the shape of the screen reads from the third
row, the numbers do not, and the numbers are the point:

  * Part 1: CE marked climbing against passthrough, holding at 12.5%.
  * Part 2: cnp climbing, and the throughput chart stepping down once ECN starts arriving.

So each job keeps the whole screen and puts a popup over a part of it that is not carrying
information, holding a few magnified lines. The lines the popup came from are ringed in the part's
colour and joined to it by dotted leaders, so it reads as a magnifier rather than as a second
unrelated picture. Part 2 also gets arrows onto the chart, because "the rate stepped down when the
CNPs started" is the whole story of Part 2 and nothing in the picture says it out loud.

Every coordinate here is in the capture's own pixel space and will not survive a retake at a
different terminal size. Regenerate after any retake, and re-check REGION and the arrows -- there
is a grid-overlay trick in the repo history for reading fresh coordinates off a screenshot.
"""

import pathlib
import sys

import fitz  # PyMuPDF

HERE = pathlib.Path(__file__).resolve().parent
IMAGES = HERE / "images"
FONT = HERE / "fonts" / "LinBiolinum_RB.otf"

# Brighter than the slides' own accents on purpose: these are drawn over a black terminal, where
# #2C7A52 and #6A4C93 go muddy. Same hue, enough luminance to read.
GREEN = (0.18, 0.62, 0.40)
PURPLE = (0.62, 0.49, 0.85)

JOBS = [
    dict(
        name="part1-doca-flow",
        accent=GREEN,
        # the "CE marked: N, passthrough: M (12.5% marked)" column, last four lines. Four, not ten:
        # the box has a fixed width, so fewer lines is a taller line, and four still shows a climb.
        # The left edge sits past the "[run_report_loop]" prefix, identical on every line.
        region=fitz.Rect(1524, 584, 2750, 760),
        popup=dict(left=102, top=1130, width=2820),   # over the chart, which is flat and loses nothing
        arrows=[],
    ),
    dict(
        name="part2-doca-pcc",
        accent=PURPLE,
        region=fitz.Rect(1525, 840, 2235, 1020),       # the first four PURE_ECN cnp/rate lines
        popup=dict(left=1560, top=1226, width=1440),   # smaller, and inside the right-hand pane
        arrows=[
            # into the tall bars on the left: full line rate, nothing reacting yet
            dict(text="no ECN yet", at=(676, 1004), tip=(320, 1180)),
            # into the low bars on the right: CNPs arriving, controller cutting the rate
            dict(text="ECN arriving — rate cut", at=(700, 1250), tip=(1080, 1400)),
        ],
    ),
]

PAD = 26            # inside the popup, around the magnified pixels
WHITE = (1, 1, 1)


def arrow(page, tail, tip, color, width=7, head=34):
    """A straight arrow. PyMuPDF draws lines but not heads, so the head is a filled triangle."""
    import math
    page.draw_line(fitz.Point(*tail), fitz.Point(*tip), color=color, width=width)
    ang = math.atan2(tip[1] - tail[1], tip[0] - tail[0])
    wings = [(tip[0] - head * math.cos(ang - a), tip[1] - head * math.sin(ang - a))
             for a in (0.42, -0.42)]
    page.draw_polyline([fitz.Point(*tip), fitz.Point(*wings[0]), fitz.Point(*wings[1]),
                        fitz.Point(*tip)], color=color, fill=color, width=1)


def compose(job):
    src_path = IMAGES / f"{job['name']}-outcome.png"
    out_path = IMAGES / f"{job['name']}-zoom.png"
    if not src_path.exists():
        sys.exit(f"{__file__}: {src_path} not found (it is the raw screen capture)")

    src = fitz.Pixmap(str(src_path))
    sw, sh = src.width, src.height
    accent, region = job["accent"], job["region"]

    doc = fitz.open()
    page = doc.new_page(width=sw, height=sh)
    page.insert_image(fitz.Rect(0, 0, sw, sh), filename=str(src_path))

    def cropped(rect):
        """SRC restricted to `rect`, as its own pixmap.

        This build of PyMuPDF has neither a clip= on insert_image nor a clipping Pixmap
        constructor, and set_cropbox only changes what a viewer shows -- the content still renders.
        So the crop happens on its own page, whose bounds discard everything outside it.
        """
        d = fitz.open()
        p = d.new_page(width=rect.width, height=rect.height)
        p.insert_image(fitz.Rect(-rect.x0, -rect.y0, -rect.x0 + sw, -rect.y0 + sh),
                       filename=str(src_path))
        return p.get_pixmap(dpi=72)

    # --- arrows onto the chart ------------------------------------------------------------------
    # 54px here is ~15px once the slide scales the image down; smaller and the labels are decoration.
    for a in job["arrows"]:
        page.insert_text(fitz.Point(*a["at"]), a["text"], fontsize=54, color=accent,
                         fontfile=str(FONT), fontname="LB")
        arrow(page, (a["at"][0] - 16, a["at"][1] + 22), a["tip"], accent)

    # --- ring the lines the popup magnifies -----------------------------------------------------
    page.draw_rect(region, color=accent, width=7)

    # --- the popup ------------------------------------------------------------------------------
    p = job["popup"]
    scale = (p["width"] - 2 * PAD) / region.width
    box = fitz.Rect(p["left"], p["top"],
                    p["left"] + p["width"], p["top"] + region.height * scale + 2 * PAD)
    inner = fitz.Rect(box.x0 + PAD, box.y0 + PAD, box.x1 - PAD, box.y1 - PAD)

    # drawn before the box, so the leaders run under it rather than across it
    for sx, bx in ((region.x0, box.x0), (region.x1, box.x1)):
        page.draw_line(fitz.Point(sx, region.y1), fitz.Point(bx, box.y0),
                       color=accent, width=6, dashes="[0.1 22] 0", lineCap=1)

    page.draw_rect(box, color=WHITE, fill=(0, 0, 0), width=14)   # white keeps it off the dark base
    page.insert_image(inner, pixmap=cropped(region))
    page.draw_rect(box, color=accent, width=7)

    page.get_pixmap(dpi=72).save(str(out_path))
    chars = 60 if job["arrows"] == [] else 34
    on_slide = 828 / sw * (p["width"] - 2 * PAD)
    print(f"  wrote images/{out_path.name}  (popup {scale:.2f}x, "
          f"{on_slide / chars:.1f}px per character on the slide)")


if __name__ == "__main__":
    for j in JOBS:
        compose(j)
