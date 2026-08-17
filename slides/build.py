#!/usr/bin/env python3
"""Build the tutorial's slide deck as ONE self-contained HTML file.

    ./build.py            write hands-on.html
    ./build.py --open     ...and open it

Why a single file with everything inlined:

  * It has to work with no network. Conference wifi fails, and this is the deck that tells forty
    people where to go -- it cannot be the thing that is broken. No CDN, no reveal.js download, no
    external font or image request.
  * It has to look the same on someone else's laptop plugged into someone else's projector. The
    fonts are the guides' own (Biolinum, Libertine, Inconsolata, from TeX Live) embedded as data
    URIs, so there is no substitution to be surprised by five minutes before the session.
  * Logos ship as PDFs for LaTeX, which no browser will draw, so they are rasterised here with
    ghostscript and inlined too.

People, institutions and the conference name come from ../guides/tutorial.json -- the same file the
guides read, so a change to the roster lands in both without anyone remembering to sync them.

The deck itself is DECK, below: one entry per slide, plain HTML. Add slides there.
"""

import base64
import json
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
GUIDES = HERE.parent / "guides"
SHOTS = HERE / "images"
FONTDIR = HERE / "fonts"
OUT = HERE / "hands-on.html"

# Stand-in for a guide that does not exist yet. Slides may ship with it; the build counts them and
# says so at the end, so an unreplaced one is loud rather than something you meet on the projector.
TODO_MARK = "REPLACE-ME"

# One group per card. The names come from admin/machines.txt -- the fleet's own list, the same one
# admin/fleet.py drives -- so a card added or pulled changes the slide by itself and a group can
# never be sent to a machine that is not in the fleet.
MACHINES = HERE.parent / "admin" / "machines.txt"
TAILNET = "tail4f7fd9.ts.net"

# Shown on the slide, so everyone in the room can type it. Rotate it after the tutorial.
PASSWORD = "Smartnics-Roce-@-Sigcomm"

TITLE = "Hands on with SmartNICs"

# Fonts, by TeX Live file name. Only the faces the deck actually uses -- every extra one is a
# megabyte of base64 for nothing.
FONTS = {
    "Biolinum":    ("LinBiolinum_R.otf", 400, "normal"),
    "BiolinumB":   ("LinBiolinum_RB.otf", 700, "normal"),
    "LibertineIt": ("LinLibertine_RI.otf", 400, "italic"),
    "Inconsolata": ("Inconsolatazi4-Regular.otf", 400, "normal"),
}


def die(msg):
    sys.exit(f"build.py: {msg}")


def data_uri(blob, mime):
    return f"data:{mime};base64," + base64.b64encode(blob).decode("ascii")


def find_font(name):
    """The font file: the deck's own fonts/ first, TeX Live's copy second.

    fonts/ holds the same four faces CTAN ships, ~1.3 MB, so the deck builds on a laptop with no
    TeX Live -- which is most laptops, and apt wants 1.35 GB of texlive-fonts-extra for exactly
    these files. kpsewhich stays as the fallback so a machine that already has them uses its own.
    """
    local = FONTDIR / name
    if local.exists():
        return local
    try:
        p = subprocess.run(["kpsewhich", name], capture_output=True, text=True).stdout.strip()
    except FileNotFoundError:
        p = ""
    if not p:
        die(f"font not found: {name}. Put it in {FONTDIR.name}/ (it is on CTAN) or install TeX Live "
            "(libertine in collection-xetex, inconsolata in collection-fontsextra).")
    return pathlib.Path(p)


def font_faces():
    css = []
    for family, (fname, weight, style) in FONTS.items():
        uri = data_uri(find_font(fname).read_bytes(), "font/otf")
        css.append(f"@font-face{{font-family:'{family}';font-weight:{weight};"
                   f"font-style:{style};src:url({uri}) format('opentype');}}")
    return "\n".join(css)


def rasterise(pdf, out):
    """Vector logo to transparent PNG at 600 dpi.

    600 dpi with an alpha channel keeps the marks crisp when projected and keeps them sitting on
    the slide background rather than in white boxes. Two backends because neither is everywhere:
    ghostscript is what a TeX-shaped machine already has, PyMuPDF is a pip install needing no root.
    """
    if shutil.which("gs"):
        subprocess.run(["gs", "-q", "-dNOPAUSE", "-dBATCH", "-sDEVICE=pngalpha", "-r600",
                        "-dUseCropBox", f"-sOutputFile={out}", str(pdf)], check=True)
        return
    try:
        import fitz  # PyMuPDF
    except ImportError:
        die("converting the PDF logos needs either ghostscript (gs) or PyMuPDF (pip install pymupdf)")
    with fitz.open(pdf) as doc:
        doc[0].get_pixmap(dpi=600, alpha=True).save(out)


def logo_uri(ident):
    """A logo as an inlined PNG. The PDFs are vector art for LaTeX; browsers need a raster."""
    png = GUIDES / "logos" / f"{ident}.png"
    pdf = GUIDES / "logos" / f"{ident}.pdf"
    if png.exists():
        return data_uri(png.read_bytes(), "image/png")
    if not pdf.exists():
        return None
    out = HERE / f".{ident}.tmp.png"
    rasterise(pdf, out)
    uri = data_uri(out.read_bytes(), "image/png")
    out.unlink()
    return uri


def image_uri(path):
    """A screenshot or figure, inlined. Same no-network rule as everything else here."""
    mime = {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg"}.get(path.suffix.lower())
    if mime is None:
        die(f"unsupported image type: {path.name} (png or jpg)")
    return data_uri(path.read_bytes(), mime)


def shot(name):
    """The expected-outcome screenshot for a part, or a placeholder standing in its place.

    Missing on purpose is a normal state here: the outcome slides are written before the run that
    produces the screenshot. So a missing file prints a note and draws a labelled box rather than
    failing the build -- the deck stays presentable, and the gap is obvious on the slide instead of
    being a silent blank. Drop the file at the path below and rebuild; nothing else changes.
    """
    for ext in (".png", ".jpg", ".jpeg"):
        p = SHOTS / f"{name}{ext}"
        if p.exists():
            return f'<img class="shot" src="{image_uri(p)}" alt="">'
    print(f"  note: no screenshot yet for '{name}' -- drop one at "
          f"slides/images/{name}.png and rebuild")
    return (f'<div class="shot-missing"><span>screenshot<br>'
            f'<code>slides/images/{name}.png</code></span></div>')


meta = json.loads((GUIDES / "tutorial.json").read_text())
logos = {i["id"]: logo_uri(i["id"]) for i in meta["institutions"]}


def people_row(key):
    out = []
    for p in meta.get(key, []):
        marks = "".join(f'<img class="logo" src="{logos[a["id"]]}" alt="">'
                        for a in p["affiliations"] if logos.get(a["id"]))
        out.append(f'<span class="person">{p["name"]}{marks}</span>')
    return "".join(out)


def group_table():
    """Group N -> the card's Tailscale address, one row per machine in the fleet list.

    Two columns rather than twelve stacked rows: one column is a 620px-wide ribbon down the middle
    of an 1100px slide, and splitting it fills the frame and buys the vertical room the panel and
    the password need.

    Every address ends in the same .tail4f7fd9.ts.net/, so the host is set in the link colour and
    the shared tail is greyed: the eye lands on the part that differs, which is the only part
    anyone is looking for, and the address is still complete enough to type.
    """
    if not MACHINES.exists():
        die(f"{MACHINES} not found -- the group table is built from the fleet list")
    names = [n.strip() for n in MACHINES.read_text().splitlines() if n.strip()]
    if not names:
        die(f"{MACHINES} is empty -- no groups to put on the slide")

    half = (len(names) + 1) // 2
    rows = []
    for r in range(half):
        cells = []
        for col, i in enumerate((r, r + half)):
            gap = " gap" if col else ""
            if i >= len(names):
                cells.append(f'<td class="n{gap}"></td><td></td>')
                continue
            cells.append(
                f'<td class="n{gap}">{i + 1}</td>'
                f'<td class="u"><a href="https://{names[i]}.{TAILNET}/">'
                f'<b>{names[i]}</b><span class="sfx">.{TAILNET}/</span></a></td>')
        rows.append(f"<tr>{''.join(cells)}</tr>")

    # The header's group cells carry .n too, so the heading and the numbers under it can never
    # drift out of alignment -- one rule moves both.
    head = ('<tr class="head"><td class="n">Group</td><td>Node Address</td>'
            '<td class="n gap">Group</td><td>Node Address</td></tr>')
    return f'<div class="panel"><table class="groups">{head}{"".join(rows)}</table></div>'


def overview():
    """The two tasks as one picture: two boxes and the loop that joins them.

    The arrows are the point of the slide. Task 1 and Task 2 are not two unrelated exercises --
    Part 1 builds the congestion signal and Part 2 builds the reaction to it, and the reaction
    changes the traffic that gets marked. Drawing only the forward arrow would leave that as two
    boxes side by side; drawing the return closes the loop, which is what DCQCN actually is.

    Each box also says which engine it runs on, because that is the other half of the picture: the
    same card, two different programmable things -- the eSwitch pipeline and the DPA's cores.
    """
    def box(n, cls, name, kind, where):
        return (f'<div class="task {cls}"><div class="tnum">Part {n}</div>'
                f'<div class="tname">{name}</div>'
                f'<div class="tkind">{kind}</div>'
                f'<div class="twhere">{where}</div></div>')

    # Each arrow takes the colour of the box it leaves, which is what tells you the direction now
    # that the labels are gone. One marker per direction: orient="auto" turns the head to follow
    # its own line, so the return arrow needs nothing but its own colour.
    arrows = """
      <svg class="flow" viewBox="0 0 170 160" width="170" height="160" aria-hidden="true">
        <defs>
          <marker id="hd-f" markerWidth="9" markerHeight="7" refX="8.5" refY="3.5" orient="auto">
            <path d="M0,0 L9,3.5 L0,7 z" fill="#2C7A52"/>
          </marker>
          <marker id="hd-b" markerWidth="9" markerHeight="7" refX="8.5" refY="3.5" orient="auto">
            <path d="M0,0 L9,3.5 L0,7 z" fill="#6A4C93"/>
          </marker>
        </defs>
        <line x1="6" y1="58" x2="156" y2="58" stroke="#2C7A52" stroke-width="4"
              marker-end="url(#hd-f)"/>
        <line x1="164" y1="102" x2="14" y2="102" stroke="#6A4C93" stroke-width="4"
              marker-end="url(#hd-b)"/>
      </svg>
    """
    return ('<div class="tasks">'
            + box(1, "p1", "Mark ECN", "Packet Processing", "eSwitch &middot; hardware pipeline")
            + arrows
            + box(2, "p2", "Congestion Control", "Programmable Transport",
                  "DPA &middot; programmable cores")
            + "</div>")


def part(eyebrow, title, shot_name, guide, cls, zoom_shot=None):
    """The slides every hands-on part gets: where its guide is, then what success looks like.

    One helper rather than three near-copies. The parts differ only in their name and their guide,
    and a deck where Part 2 has drifted a font size away from Part 1 reads as a mistake from the
    third row.

    guide= is the file at the root of the checkout, and it is the whole line: the directory is the
    same for all three parts, so it was a shape people stopped reading after Part 1.

    cls= is the part's colour class, the same one its box wears in the overview diagram. It sets
    --accent for the slide, which the eyebrow, the filename and the outcome caption all read, so a
    part is the same colour everywhere it appears and one edit moves all of it.

    zoom_shot= adds a third slide. A 3024px capture shown whole answers "is this my screen?" and a
    magnified crop answers "are my numbers moving?"; they are different questions and one image
    cannot be sized for both, so a part that needs the second gets its own slide for it.
    """
    slides = [
        f"""
    <div class="centred {cls}">
      <div class="eyebrow">{eyebrow}</div>
      <h2>{title}</h2>
      <p class="lead">The guide on the node</p>
      <p class="gfile">{guide}</p>
    </div>
    """,
        # No h2 here on purpose. These screenshots are 3024px-wide terminals, and a 62px heading
        # costs the image ~110px of height -- which, at their 1.66 aspect, is ~180px of width. The
        # slide's whole job is "does your screen look like this?", so the caption folds into one
        # line and the picture gets everything else.
        f"""
    <div class="centred {cls}">
      <div class="shot-cap">{eyebrow} &middot; {title} &mdash; what you should see</div>
      {shot(shot_name)}
    </div>
    """,
    ]
    if zoom_shot:
        slides.append(f"""
    <div class="centred {cls}">
      <div class="shot-cap">{eyebrow} &middot; {title} &mdash; what you should see</div>
      {shot(zoom_shot)}
    </div>
    """)
    return slides


# --- the deck --------------------------------------------------------------------------------
# One string per slide. Slide 1 mirrors the guides' header block on purpose: same order (what it
# is, which conference, which tutorial, who runs it) and the same logo-beside-the-name treatment,
# so the projector and the PDF on someone's laptop read as one set.
DECK = [
    f"""
    <div class="title-slide">
      <h1>{TITLE}</h1>
      <div class="conference">{meta["conference"]}</div>
      <hr>
      <div class="tutorial-title">{meta["tutorial-title"]}</div>
      <div class="people">{people_row("organizers")}</div>
    </div>
    """,
    f"""
    <div class="centred">
      <h2>Let&rsquo;s get you onto a BlueField&#8209;3</h2>
      {group_table()}
      <p class="password">Password <code>{PASSWORD}</code></p>
    </div>
    """,
    f"""
    <div class="centred">
      {overview()}
      <p class="lead">Part 1 creates the congestion signal. Part 2 reacts to it.</p>
    </div>
    """,
    *part("Part 1", "Mark ECN", "part1-doca-flow-outcome", guide="doca-flow.pdf", cls="p1",
          zoom_shot="part1-doca-flow-zoom"),
    *part("Part 2", "Congestion Control", "part2-doca-pcc-outcome", guide="doca-pcc.pdf",
          cls="p2", zoom_shot="part2-doca-pcc-zoom"),
    *part("Bonus", "Steering", "bonus-steering-outcome", guide=TODO_MARK, cls="pb"),
]

CSS = """
*{margin:0;padding:0;box-sizing:border-box}
html,body{height:100%;background:#111;overflow:hidden}
/* A fixed 1280x720 stage scaled to the window: layout is then identical on every projector,
   which a percentage-based deck never quite is. */
#stage{position:absolute;top:50%;left:50%;width:1280px;height:720px;background:#fff;
       transform-origin:center center}
.slide{position:absolute;inset:0;display:none;padding:70px 90px}
.slide.on{display:flex;align-items:center;justify-content:center}
.title-slide,.centred{width:100%;text-align:center}

h1{font-family:'BiolinumB',sans-serif;font-weight:700;font-size:76px;letter-spacing:-.5px;
   color:#111;line-height:1.05}
.conference{font-family:'Biolinum',sans-serif;font-size:34px;color:#4A7AAB;margin-top:14px}
hr{border:0;border-top:1.5px solid #C8C8C8;width:58%;margin:34px auto}
.tutorial-title{font-family:'LibertineIt',Georgia,serif;font-style:italic;font-size:27px;
                color:#222}
.people{margin-top:44px;font-family:'Biolinum',sans-serif;font-size:25px;color:#111}
.person{white-space:nowrap;margin:0 15px;display:inline-block}
.logo{height:1.15em;vertical-align:-.19em;margin-left:.3em}

h2{font-family:'BiolinumB',sans-serif;font-weight:700;font-size:62px;color:#111;line-height:1.1}
.lead{font-family:'Biolinum',sans-serif;font-size:33px;color:#333;margin-top:40px}
/* The guide's filename, alone on the line now that the directory is gone -- so it is sized and
   coloured as the thing being pointed at rather than as a footnote under a path. */
.gfile{margin-top:20px;font-family:'Inconsolata',monospace;font-size:44px;color:var(--accent,#2C6394)}

/* The group table, in a panel so the addresses read as one object to find yourself in rather than
   loose text on a white field. inline-block keeps the panel exactly as wide as its contents and
   .centred does the rest. */
.panel{display:inline-block;margin-top:24px;padding:18px 30px;background:#F7F9FB;
       border:1px solid #E4E9EE;border-radius:12px}
.groups{border-collapse:collapse;font-size:25px}
/* Hairlines at the panel border's own weight: enough to carry the eye across a row and to part
   the two halves, not enough to read as a grid. The last row drops its rule so the table does not
   end on a line sitting just above the panel padding. */
.groups td{padding:9px 0;line-height:1.2;white-space:nowrap;border-bottom:1px solid #E8ECF0}
.groups tr:last-child td{border-bottom:0}
.groups .head td{font-family:'Biolinum',sans-serif;font-size:17px;letter-spacing:1.2px;
                 text-transform:uppercase;color:#111;padding-bottom:10px;
                 border-bottom:1px solid #D8DFE6}
.groups .n{font-family:'Biolinum',sans-serif;color:#111;text-align:center;padding-right:16px}
/* The 44px trough between the columns, split either side of the divider so it stays centred. */
.groups td:nth-child(2){padding-right:22px}
.groups .gap{padding-left:22px;border-left:1px solid #E8ECF0}
/* Not a link colour on the whole address: only the host differs, so only the host is emphasised. */
.groups .u a{font-family:'Inconsolata',monospace;text-decoration:none}
.groups .u b{font-weight:400;color:#2C6394}
.groups .u .sfx{color:#555}

.password{margin-top:26px;font-family:'Biolinum',sans-serif;font-size:30px;color:#555}
.password code{font-family:'Inconsolata',monospace;font-size:30px;color:#111;background:#F0F3F6;
                border:1px solid #E0E5EA;border-radius:7px;padding:4px 14px;margin-left:10px}

/* The two-task diagram. stretch, not center, so the boxes match height whatever their text does --
   one title wrapping to two lines must not leave the pair looking misaligned. */
.tasks{display:flex;align-items:stretch;justify-content:center}
/* A colour per part, carried through the box, its label and the arrow leaving it, so the two
   halves of the tutorial stay told apart wherever they appear. --accent is set by .p1/.p2 and
   read by everything inside. */
.task{display:flex;flex-direction:column;justify-content:center;width:455px;min-height:420px;
      padding:40px 34px;border:1px solid;border-radius:14px;text-align:center}
/* One accent per part, defined once and worn by its diagram box AND its two slides. */
.p1{--accent:#2C7A52}
.p2{--accent:#6A4C93}
.pb{--accent:#2C6394}
.task.p1{background:#ECF6F0;border-color:#C2E0CF}
.task.p2{background:#F2EEF8;border-color:#D6CBE9}
.tnum{font-family:'Biolinum',sans-serif;font-size:23px;letter-spacing:1.8px;
      text-transform:uppercase;color:var(--accent);margin-bottom:14px}
.tname{font-family:'BiolinumB',sans-serif;font-weight:700;font-size:42px;color:#111;line-height:1.1}
.tkind{font-family:'Biolinum',sans-serif;font-size:31px;color:var(--accent);margin-top:16px}
.twhere{font-family:'Biolinum',sans-serif;font-size:22px;color:#555;margin-top:12px}
.flow{flex:0 0 auto;align-self:center;margin:0 8px}

/* Part slides. The eyebrow carries which part you are in, so the h2 can be the plain name of the
   thing -- "DOCA Flow", not "Part 1: DOCA Flow" wrapped onto two lines. */
.eyebrow{font-family:'Biolinum',sans-serif;font-size:26px;letter-spacing:1.5px;
         text-transform:uppercase;color:var(--accent,#4A7AAB);margin-bottom:18px}
/* On an outcome slide the caption is not a kicker over a heading -- it IS the heading, the only
   text on the slide, so it is sized as one. 36px: the longest caption is "Part 2 - Congestion Control - what you should see", which at 40px
   ran to 101% of the line. 36 puts it at 91%, which leaves room for a part named longer still.
   Tracking drops to .5px because 1.5px is a small-text trick that looks loose this big. */
.shot-cap{font-family:'Biolinum',sans-serif;font-size:36px;letter-spacing:.5px;
          text-transform:uppercase;color:var(--accent,#4A7AAB);margin-bottom:18px}

/* 500px is the tallest that still clears the 40px caption and the 70px padding on a 720px stage --
   measured, not guessed. The screenshots are wide (1.66), so height is what binds: every pixel
   here is 1.66 of width. contain keeps the aspect ratio of whatever gets dropped in, and the
   placeholder below is the same height so the slide does not jump when the real one arrives. */
.shot{display:block;margin:14px auto 0;max-width:100%;max-height:500px;object-fit:contain;
      border:1px solid #DDD;border-radius:4px}
.shot-missing{display:flex;align-items:center;justify-content:center;width:70%;height:500px;
              margin:34px auto 0;border:2px dashed #CCC;border-radius:6px;
              font-family:'Biolinum',sans-serif;font-size:26px;color:#999;line-height:1.9}
.shot-missing code{font-family:'Inconsolata',monospace;font-size:23px;color:#777}

/* Deliberately dim and out of the way: useful while presenting, invisible from the third row. */
#num{position:fixed;right:14px;bottom:10px;font-family:'Biolinum',sans-serif;font-size:13px;
     color:#666}
"""

JS = """
const slides=[...document.querySelectorAll('.slide')];let i=0;
const num=document.getElementById('num');
function show(n){i=Math.max(0,Math.min(slides.length-1,n));
  slides.forEach((s,k)=>s.classList.toggle('on',k===i));
  num.textContent=(i+1)+' / '+slides.length;
  // The hash makes a slide linkable: hand someone "#2" and they land on it, and reloading
  // mid-talk puts you back where you were instead of at slide 1.
  history.replaceState(null,'','#'+(i+1));}
// Scale the fixed stage to whatever the projector gives us, keeping 16:9 and centring the letterbox.
function fit(){const s=Math.min(innerWidth/1280,innerHeight/720);
  document.getElementById('stage').style.transform=
    'translate(-50%,-50%) scale('+s+')';}
addEventListener('resize',fit);fit();
show((parseInt(location.hash.slice(1),10)||1)-1);
addEventListener('hashchange',()=>show((parseInt(location.hash.slice(1),10)||1)-1));
addEventListener('keydown',e=>{
  if(['ArrowRight','ArrowDown',' ','PageDown','n'].includes(e.key)){show(i+1);e.preventDefault();}
  else if(['ArrowLeft','ArrowUp','PageUp','p'].includes(e.key)){show(i-1);e.preventDefault();}
  else if(e.key==='Home'){show(0);}else if(e.key==='End'){show(slides.length-1);}
  else if(e.key==='f'){document.documentElement.requestFullscreen?.();}
  else if(e.key==='Escape'){document.exitFullscreen?.();}});
addEventListener('click',e=>{if(!e.target.closest('a'))show(i+1);});
"""

slides_html = "\n".join(f'<section class="slide">{s}</section>' for s in DECK)
html = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{TITLE} — {meta["conference"]}</title>
<style>
{font_faces()}
{CSS}
</style>
</head>
<body>
<div id="stage">
{slides_html}
</div>
<div id="num"></div>
<script>
{JS}
</script>
</body>
</html>
"""

OUT.write_text(html)
kb = len(html.encode()) // 1024
print(f"  wrote {OUT.name}  ({len(DECK)} slides, {kb} KB, self-contained)")

pending = html.count(TODO_MARK)
if pending:
    print(f"  TODO: {pending} guide(s) still show the {TODO_MARK} placeholder")
if "--open" in sys.argv:
    subprocess.run(["open", str(OUT)])
